// Extruder stepper pulse time generation
//
// Copyright (C) 2018-2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <math.h> // tanh
#include <stddef.h> // offsetof
#include <stdlib.h> // malloc
#include <string.h> // memset
#include "compiler.h" // __visible
#include "itersolve.h" // struct stepper_kinematics
#include "integrate.h" // struct smoother
#include "kin_shaper.h" // struct shaper_pulses
#include "list.h" // list_node
#include "pyhelper.h" // errorf
#include "trapq.h" // move_get_distance

// Without pressure advance, the extruder stepper position is:
//     extruder_position(t) = nominal_position(t)
// When pressure advance is enabled, additional filament is pushed
// into the extruder during acceleration (and retracted during
// deceleration). The formula is:
//     pa_position(t) = (nominal_position(t)
//                       + pressure_advance * nominal_velocity(t))
// The nominal position and velocity are then smoothed using a weighted average:
//     smooth_position(t) = (
//         definitive_integral(nominal_position(x+t_offs) * smoother(t-x) * dx,
//                             from=t-smooth_time/2, to=t+smooth_time/2)
//     smooth_velocity(t) = (
//         definitive_integral(nominal_velocity(x+t_offs) * smoother(t-x) * dx,
//                             from=t-smooth_time/2, to=t+smooth_time/2)
// and the final pressure advance value calculated as
//     smooth_pa_position(t) = smooth_position(t) + pa_func(smooth_velocity(t))
// where pa_func(v) = pressure_advance * v for linear velocity model or a more
// complicated function for non-linear pressure advance models.

// Calculate the definitive integral of extruder for a given move
static inline void
pa_move_integrate(const struct move *m, int axis
                  , double t0, const smoother_antiderivatives *ad
                  , double *pa_velocity_integral)
{
    // Calculate base position and velocity with pressure advance
    int can_pressure_advance = m->axes_r.x > 0. || m->axes_r.y > 0.;

    // Calculate definitive integral
    if (can_pressure_advance)
        *pa_velocity_integral += integrate_velocity(m, axis, t0, ad);
}

// Calculate the definitive integral of the extruder over a range of moves
static void
pa_range_integrate(const struct move *m, int axis, double move_time
                   , const struct smoother *sm
                   , double *pa_velocity_integral)
{
    move_time += sm->t_offs;
    while (unlikely(move_time < 0.)) {
        m = list_prev_entry(m, node);
        move_time += m->move_t;
    }
    while (unlikely(move_time > m->move_t)) {
        move_time -= m->move_t;
        m = list_next_entry(m, node);
    }
    // Calculate integral for the current move
    double start = move_time - sm->hst, end = move_time + sm->hst;
    double t0 = move_time;
    *pa_velocity_integral = 0.;
    if (unlikely(start >= 0. && end <= m->move_t)) {
        pa_move_integrate(m, axis, t0, &sm->pm_diff,
                          pa_velocity_integral);
        return;
    }
    smoother_antiderivatives left =
        likely(start < 0.) ? calc_antiderivatives(sm, t0) : sm->p_hst;
    smoother_antiderivatives right =
        likely(end > m->move_t) ? calc_antiderivatives(sm, t0 - m->move_t)
                                : sm->m_hst;
    smoother_antiderivatives diff = diff_antiderivatives(&right, &left);
    pa_move_integrate(m, axis, t0, &diff,
                      pa_velocity_integral);
    // Integrate over previous moves
    const struct move *prev = m;
    while (likely(start < 0.)) {
        prev = list_prev_entry(prev, node);
        start += prev->move_t;
        t0 += prev->move_t;
        smoother_antiderivatives r = left;
        left = likely(start < 0.) ? calc_antiderivatives(sm, t0)
                                  : sm->p_hst;
        diff = diff_antiderivatives(&r, &left);
        pa_move_integrate(prev, axis, t0, &diff,
                          pa_velocity_integral);
    }
    // Integrate over future moves
    t0 = move_time;
    while (likely(end > m->move_t)) {
        end -= m->move_t;
        t0 -= m->move_t;
        m = list_next_entry(m, node);
        smoother_antiderivatives l = right;
        right = likely(end > m->move_t) ? calc_antiderivatives(sm,
                                                               t0 - m->move_t)
                                        : sm->m_hst;
        diff = diff_antiderivatives(&right, &l);
        pa_move_integrate(m, axis, t0, &diff,
                          pa_velocity_integral);
    }
}

static void
shaper_pa_range_integrate(const struct move *m, int axis, double move_time
                          , const struct shaper_pulses *sp
                          , const struct smoother *sm
                          , double *pa_velocity_integral)
{
    *pa_velocity_integral = 0.;
    int num_pulses = sp->num_pulses, i;
    for (i = 0; i < num_pulses; ++i) {
        double t = sp->pulses[i].t, a = sp->pulses[i].a;
        double p_pa_vel_int;
        pa_range_integrate(m, axis, move_time + t, sm,
                           &p_pa_vel_int);
        *pa_velocity_integral += a * p_pa_vel_int;
    }
}

struct pressure_advance_params {
    union {
        struct {
            double pressure_advance;
        };
        struct {
            double linear_advance, nonlinear_offset, linearization_velocity;
        };
        double params[3];
    };
};

typedef double (*pressure_advance_func)(
        double, double, struct pressure_advance_params *pa_params);

struct pa_params {
    struct pressure_advance_params params;
    pressure_advance_func func;
    double time_offset, active_print_time;
    struct list_node node;
};

struct extruder_stepper {
    struct stepper_kinematics sk;
    struct shaper_pulses sp[3];
    struct smoother sm[3];
    struct list_head pa_list;
};

double __visible
pressure_advance_linear_model_func(double position, double pa_velocity
                                   , struct pressure_advance_params *pa_params)
{
    return position + pa_velocity * pa_params->pressure_advance;
}

double __visible
pressure_advance_tanh_model_func(double position, double pa_velocity
                                 , struct pressure_advance_params *pa_params)
{
    position += pa_params->linear_advance * pa_velocity;
    if (pa_params->nonlinear_offset) {
        double rel_velocity = pa_velocity / pa_params->linearization_velocity;
        position += pa_params->nonlinear_offset * tanh(rel_velocity);
    }
    return position;
}

double __visible
pressure_advance_recipr_model_func(double position, double pa_velocity
                                   , struct pressure_advance_params *pa_params)
{
    position += pa_params->linear_advance * pa_velocity;
    if (pa_params->nonlinear_offset) {
        double rel_velocity = pa_velocity / pa_params->linearization_velocity;
        position += pa_params->nonlinear_offset * (1. - 1. / (1. + rel_velocity));
    }
    return position;
}

static struct pa_params *
extruder_lookup_pa(struct extruder_stepper *es, double print_time)
{
    struct pa_params *pa = list_last_entry(
        &es->pa_list, struct pa_params, node);
    while (unlikely(pa->active_print_time > print_time)
           && !list_is_first(&pa->node, &es->pa_list))
        pa = list_prev_entry(pa, node);
    return pa;
}

static double
extruder_calc_position(struct stepper_kinematics *sk, struct move *m
                       , double move_time)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    struct pa_params *pa = extruder_lookup_pa(es, m->print_time);
    move_time += pa->time_offset;
    while (unlikely(move_time < 0.)) {
        m = list_prev_entry(m, node);
        move_time += m->move_t;
    }
    while (unlikely(move_time >= m->move_t)) {
        move_time -= m->move_t;
        m = list_next_entry(m, node);
    }
    int i;
    struct coord e_pos, pa_vel;
    double move_dist = move_get_distance(m, move_time);
    for (i = 0; i < 3; ++i) {
        int axis = 'x' + i;
        const struct shaper_pulses* sp = &es->sp[i];
        const struct smoother* sm = &es->sm[i];
        int num_pulses = sp->num_pulses;
        e_pos.axis[i] = num_pulses
            ? shaper_calc_position(m, axis, move_time, sp)
            : m->start_pos.axis[i] + m->axes_r.axis[i] * move_dist;
        if (!sm->hst) {
            pa_vel.axis[i] = 0.;
        } else if (num_pulses) {
            shaper_pa_range_integrate(m, axis, move_time, sp, sm,
                                      &pa_vel.axis[i]);
        } else {
            pa_range_integrate(m, axis, move_time, sm, &pa_vel.axis[i]);
        }
    }
    double position = e_pos.x + e_pos.y + e_pos.z;
    double pa_velocity = pa_vel.x + pa_vel.y + pa_vel.z;
    if (pa_velocity > 0.)
        return pa->func(position, pa_velocity, &pa->params);
    return position;
}

static void
extruder_note_generation_time(struct extruder_stepper *es)
{
    double pre_active = 0., post_active = 0.;
    int i;
    for (i = 0; i < 3; ++i) {
        struct shaper_pulses* sp = &es->sp[i];
        const struct smoother* sm = &es->sm[i];
        double time_offset = list_last_entry(
            &es->pa_list, struct pa_params, node)->time_offset;
        double pre_active_axis = sm->hst + sm->t_offs + time_offset +
            (sp->num_pulses ? sp->pulses[sp->num_pulses-1].t : 0.);
        double post_active_axis = sm->hst - sm->t_offs - time_offset +
            (sp->num_pulses ? -sp->pulses[0].t : 0.);
        if (pre_active_axis > pre_active)
            pre_active = pre_active_axis;
        if (post_active_axis > post_active)
            post_active = post_active_axis;
    }
    es->sk.gen_steps_pre_active = pre_active;
    es->sk.gen_steps_post_active = post_active;
}

void __visible
extruder_set_pressure_advance(struct stepper_kinematics *sk, double print_time
                              , pressure_advance_func func
                              , int n_params, double params[]
                              , double time_offset)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    if (n_params < 0 || n_params > 3)
        return;

    // Discard settings that can no longer be observed by step generation.
    double cleanup_time = sk->last_flush_time
        - (sk->gen_steps_pre_active > sk->gen_steps_post_active
           ? sk->gen_steps_pre_active : sk->gen_steps_post_active);
    struct pa_params *first = list_first_entry(
        &es->pa_list, struct pa_params, node);
    while (!list_is_last(&first->node, &es->pa_list)) {
        struct pa_params *next = list_next_entry(first, node);
        if (next->active_print_time >= cleanup_time)
            break;
        list_del(&first->node);
        free(first);
        first = next;
    }

    struct pa_params candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.func = func;
    candidate.time_offset = time_offset;
    candidate.active_print_time = print_time;
    memcpy(&candidate.params, params, n_params * sizeof(params[0]));
    struct pa_params *last = list_last_entry(
        &es->pa_list, struct pa_params, node);
    if (last->func == candidate.func
        && last->time_offset == candidate.time_offset
        && !memcmp(&last->params, &candidate.params, sizeof(candidate.params)))
        return;

    struct pa_params *pa = malloc(sizeof(*pa));
    *pa = candidate;
    list_add_tail(&pa->node, &es->pa_list);
    extruder_note_generation_time(es);
}

int __visible
extruder_set_shaper_params(struct stepper_kinematics *sk, char axis
                           , int n, double a[], double t[])
{
    if (axis != 'x' && axis != 'y')
        return -1;
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    struct shaper_pulses *sp = &es->sp[axis-'x'];
    int status = init_shaper(n, a, t, sp);
    extruder_note_generation_time(es);
    return status;
}

int __visible
extruder_set_smoothing_params(struct stepper_kinematics *sk, char axis
                              , int n, double a[], double t_sm, double t_offs)
{
    if (axis != 'x' && axis != 'y' && axis != 'z')
        return -1;
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    struct smoother *sm = &es->sm[axis-'x'];
    int status = init_smoother(n, a, t_sm, sm);
    sm->t_offs = t_offs;
    extruder_note_generation_time(es);
    return status;
}

struct stepper_kinematics * __visible
extruder_stepper_alloc(void)
{
    struct extruder_stepper *es = malloc(sizeof(*es));
    memset(es, 0, sizeof(*es));
    es->sk.calc_position_cb = extruder_calc_position;
    es->sk.active_flags = AF_X | AF_Y | AF_Z;
    list_init(&es->pa_list);
    struct pa_params *pa = malloc(sizeof(*pa));
    memset(pa, 0, sizeof(*pa));
    pa->func = pressure_advance_linear_model_func;
    list_add_tail(&pa->node, &es->pa_list);
    return &es->sk;
}

void __visible
extruder_stepper_free(struct stepper_kinematics *sk)
{
    struct extruder_stepper *es = container_of(sk, struct extruder_stepper, sk);
    while (!list_empty(&es->pa_list)) {
        struct pa_params *pa = list_first_entry(
            &es->pa_list, struct pa_params, node);
        list_del(&pa->node);
        free(pa);
    }
    free(es);
}
