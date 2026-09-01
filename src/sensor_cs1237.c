// Support for bit-banging commands to CS1237 ADC chips
//
// Copyright (C) 2026  The Klipper project
//
// This file may be distributed under the terms of the GNU GPLv3 license.
//
// CS1237 sampling/timing logic based on code from:
// https://codeberg.org/kuwoyuki/klipper-q2
// Interrupt-driven acquisition based on Anycubic's MCU implementation:
// https://github.com/ANYCUBIC-3D/K3-klipper-mcu/blob/main/src/sensor_cs1237.c
// Adapted to Kalico's load_cell_probe interface.

#include <stdint.h>
#include "autoconf.h" // CONFIG_MACH_AVR
#include "basecmd.h" // oid_alloc
#include "board/gpio.h" // gpio_out_write
#include "board/irq.h" // irq_poll
#include "board/misc.h" // timer_read_time
#include "command.h" // DECL_COMMAND
#include "sched.h" // sched_add_timer
#include "sensor_bulk.h" // sensor_bulk_report
#include "load_cell_probe.h" // load_cell_probe_report_sample
#if CONFIG_MACH_GD32F303_Q2
#include "generic/armcm_boot.h" // armcm_enable_irq
#include "stm32/internal.h" // GPIO and STM32 register definitions
#endif

struct cs1237_adc {
    struct timer timer;
    uint8_t config;
    uint8_t flags;
    uint32_t rest_ticks;
    uint32_t period_ticks;    // one sample period
    uint32_t pending_since;   // time CS_PENDING was set
    uint32_t last_error;
    int32_t last_sample;
    struct gpio_in dout_in; // bidirectional data-ready / data pin
    struct gpio_out dout_out;
    struct gpio_out sclk;
    struct sensor_bulk sb;
    struct load_cell_probe *lce;
#if CONFIG_MACH_GD32F303_Q2
    uint8_t use_drdy_irq;
#endif
};

enum {
    CS_PENDING = 1 << 0, CS_OVERFLOW = 1 << 1, CS_READING = 1 << 2,
    CS_NEED_CONFIG = 1 << 3,
};

// Internal sample error values reported over bulk transport
#define BYTES_PER_SAMPLE 4
#define SAMPLE_ERROR_TIMEOUT (1L << 31)
#define SAMPLE_ERROR_READ_TOO_LONG (1L << 30)
#define SAMPLE_ERROR_CONFIG (1L << 29)

// CS1237 command words (7-bit)
#define CMD_WRITE_CONFIG 0x65
#define CMD_READ_CONFIG 0x56

static struct task_wake wake_cs1237;
#if CONFIG_MACH_GD32F303_Q2

static uint8_t cs1237_is_data_ready(struct cs1237_adc *cs1237);

// The Q2 toolhead routes CS1237 DOUT to PB4 / EXTI4.
#define CS1237_DRDY_PIN GPIO('B', 4)
#define CS1237_DRDY_EXTI EXTI_IMR_MR4

static struct cs1237_adc *cs1237_exti4;

void
cs1237_exti4_irq(void)
{
    if (!(EXTI->PR & CS1237_DRDY_EXTI))
        return;
    EXTI->PR = CS1237_DRDY_EXTI;
    struct cs1237_adc *cs1237 = cs1237_exti4;
    if (!cs1237 || cs1237->flags)
        return;
    // Mask this line until the background task consumes the sample.
    EXTI->IMR &= ~CS1237_DRDY_EXTI;
    cs1237->pending_since = timer_read_time();
    cs1237->flags = CS_PENDING;
    sched_wake_task(&wake_cs1237);
}

static void
cs1237_drdy_irq_disable(struct cs1237_adc *cs1237)
{
    if (cs1237->use_drdy_irq)
        EXTI->IMR &= ~CS1237_DRDY_EXTI;
}

static void
cs1237_drdy_irq_enable(struct cs1237_adc *cs1237)
{
    if (!cs1237->use_drdy_irq)
        return;
    irqstatus_t flag = irq_save();
    EXTI->PR = CS1237_DRDY_EXTI;
    EXTI->IMR |= CS1237_DRDY_EXTI;
    // Enabling an EXTI line does not synthesize an edge if DOUT is already
    // low, so explicitly queue a sample in that case.
    if (cs1237_is_data_ready(cs1237)) {
        EXTI->IMR &= ~CS1237_DRDY_EXTI;
        EXTI->PR = CS1237_DRDY_EXTI;
        cs1237->pending_since = timer_read_time();
        cs1237->flags = CS_PENDING;
        sched_wake_task(&wake_cs1237);
    }
    irq_restore(flag);
}

static void
cs1237_drdy_irq_setup(struct cs1237_adc *cs1237, uint32_t dout_pin)
{
    if (dout_pin != CS1237_DRDY_PIN)
        shutdown("Q2 CS1237 DOUT must use PB4");
    if (cs1237_exti4)
        shutdown("Q2 supports only one CS1237");
    cs1237_exti4 = cs1237;
    cs1237->use_drdy_irq = 1;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    RCC->APB2ENR;
    AFIO->EXTICR[1] = (AFIO->EXTICR[1] & ~AFIO_EXTICR2_EXTI4)
                      | AFIO_EXTICR2_EXTI4_PB;
    EXTI->IMR &= ~CS1237_DRDY_EXTI;
    EXTI->RTSR &= ~EXTI_RTSR_TR4;
    EXTI->FTSR |= EXTI_FTSR_TR4;
    EXTI->PR = CS1237_DRDY_EXTI;
    armcm_enable_irq(cs1237_exti4_irq, EXTI4_IRQn, 2);
}

#endif

/****************************************************************
 * Low-level bit-banging
 ****************************************************************/

#define MIN_PULSE_TIME nsecs_to_ticks(500) // datasheet min pulse ~455ns

static uint32_t
nsecs_to_ticks(uint32_t ns)
{
    return timer_from_us(ns * 1000) / 1000000;
}

// Pause for a minimum pulse time with irq disabled
static void
cs1237_delay_noirq(void)
{
    if (CONFIG_MACH_AVR) {
        asm("nop\n    nop");
        return;
    }
    uint32_t end = timer_read_time() + MIN_PULSE_TIME;
    while (timer_is_before(timer_read_time(), end))
        ;
}

// Pause for a minimum pulse time
static void
cs1237_delay(void)
{
    if (CONFIG_MACH_AVR)
        return;
    uint32_t end = timer_read_time() + MIN_PULSE_TIME;
    while (timer_is_before(timer_read_time(), end))
        irq_poll();
}

static uint8_t
cs1237_is_data_ready(struct cs1237_adc *cs1237)
{
    return !gpio_in_read(cs1237->dout_in);
}

static void
cs1237_clock_pulse(struct cs1237_adc *cs1237)
{
    irq_disable();
    gpio_out_toggle_noirq(cs1237->sclk);
    cs1237_delay_noirq();
    gpio_out_toggle_noirq(cs1237->sclk);
    irq_enable();
    cs1237_delay();
}

static uint8_t
cs1237_read_bit(struct cs1237_adc *cs1237)
{
    irq_disable();
    gpio_out_toggle_noirq(cs1237->sclk);
    cs1237_delay_noirq();
    uint8_t bit = gpio_in_read(cs1237->dout_in);
    gpio_out_toggle_noirq(cs1237->sclk);
    irq_enable();
    cs1237_delay();
    return bit;
}

static void
cs1237_write_bit(struct cs1237_adc *cs1237, uint8_t bit)
{
    gpio_out_write(cs1237->dout_out, !!bit);
    cs1237_clock_pulse(cs1237);
}

// Read one CS1237 ADC frame and move to next conversion.
// Returns a 24-bit value (in the low bits of the return value).
static uint32_t
cs1237_read_frame(struct cs1237_adc *cs1237, uint8_t *status)
{
    uint32_t counts = 0;
    uint8_t i;
    for (i = 0; i < 24; i++)
        counts = (counts << 1) | cs1237_read_bit(cs1237);
    uint8_t update1 = cs1237_read_bit(cs1237);
    uint8_t update2 = cs1237_read_bit(cs1237);
    // 27th pulse forces DOUT high until the next conversion is ready
    cs1237_clock_pulse(cs1237);
    if (status)
        *status = update1 | (update2 << 1);
    return counts;
}

static void
cs1237_send_command7(struct cs1237_adc *cs1237, uint8_t cmd)
{
    int_fast8_t i;
    for (i = 6; i >= 0; i--)
        cs1237_write_bit(cs1237, (cmd >> i) & 0x01);
}

static int
cs1237_wait_data_ready(struct cs1237_adc *cs1237, uint32_t timeout_ticks)
{
    uint32_t end = timer_read_time() + timeout_ticks;
    while (timer_is_before(timer_read_time(), end)) {
        if (cs1237_is_data_ready(cs1237))
            return 0;
        irq_poll();
    }
    return -1;
}

static int
cs1237_write_config(struct cs1237_adc *cs1237, uint8_t config)
{
    if (cs1237_wait_data_ready(cs1237, timer_from_us(500000)))
        return -1;
    // Enter config sequence by reading the current conversion frame.
    cs1237_read_frame(cs1237, NULL);
    gpio_out_reset(cs1237->dout_out, 1);
    // Clocks 28..29
    cs1237_clock_pulse(cs1237);
    cs1237_clock_pulse(cs1237);
    // Clocks 30..36: 7-bit write command
    cs1237_send_command7(cs1237, CMD_WRITE_CONFIG);
    // Clock 37: direction-select clock
    cs1237_clock_pulse(cs1237);
    // Clocks 38..45: 8-bit payload
    int_fast8_t i;
    for (i = 7; i >= 0; i--)
        cs1237_write_bit(cs1237, (config >> i) & 0x01);
    // Clock 46 finalizes the transaction
    gpio_out_write(cs1237->dout_out, 1);
    cs1237_delay();
    gpio_in_reset(cs1237->dout_in, 1);
    cs1237_clock_pulse(cs1237);
    return 0;
}

static int
cs1237_read_config(struct cs1237_adc *cs1237, uint8_t *config)
{
    if (cs1237_wait_data_ready(cs1237, timer_from_us(500000)))
        return -1;
    // Clocks 1..27
    cs1237_read_frame(cs1237, NULL);
    // Clocks 28..29: direction-switch clocks
    gpio_out_reset(cs1237->dout_out, 1);
    cs1237_clock_pulse(cs1237);
    cs1237_clock_pulse(cs1237);
    // Clocks 30..36: 7-bit read command
    cs1237_send_command7(cs1237, CMD_READ_CONFIG);
    // Clock 37: switch bus direction for readback
    gpio_in_reset(cs1237->dout_in, 1);
    cs1237_clock_pulse(cs1237);
    // Clocks 38..45: 8-bit register payload
    uint8_t readback = 0;
    uint8_t i;
    for (i = 0; i < 8; i++)
        readback = (readback << 1) | cs1237_read_bit(cs1237);
    // Clock 46: CS1237 drives DOUT/DRDY high
    cs1237_clock_pulse(cs1237);
    *config = readback;
    return 0;
}

static int
cs1237_configure_chip(struct cs1237_adc *cs1237)
{
    // Datasheet power-down exit: keep SCLK low for at least 10us
    gpio_out_write(cs1237->sclk, 0);
    uint32_t wake_end = timer_read_time() + timer_from_us(20);
    while (timer_is_before(timer_read_time(), wake_end))
        irq_poll();
    gpio_in_reset(cs1237->dout_in, 1);
    int ret = cs1237_write_config(cs1237, cs1237->config);
    if (ret)
        return ret;
    uint8_t readback = 0;
    ret = cs1237_read_config(cs1237, &readback);
    if (ret)
        return ret;
    if ((readback & 0x7f) != (cs1237->config & 0x7f))
        return -1;
    return 0;
}

/****************************************************************
 * CS1237 Sensor Support
 ****************************************************************/

// CS1237 config register bits [5:4] select ODR:
//   00 = 10 Hz, 01 = 40 Hz, 10 = 640 Hz, 11 = 1280 Hz
static uint32_t
cs1237_sample_period_ticks(uint8_t config)
{
    switch ((config >> 4) & 0x3) {
    case 0: return timer_from_us(100000); // 10 Hz
    case 1: return timer_from_us(25000);  // 40 Hz
    case 2: return timer_from_us(1563);   // 640 Hz
    case 3: return timer_from_us(782);    // 1280 Hz
    }
    return timer_from_us(100000);
}

// Timer event scheduled periodically to poll the DRDY pin
static uint_fast8_t
cs1237_event(struct timer *timer)
{
    struct cs1237_adc *cs1237 = container_of(timer, struct cs1237_adc, timer);
    uint32_t rest_ticks = cs1237->rest_ticks;
    uint8_t flags = cs1237->flags;

    if (flags & (CS_READING | CS_NEED_CONFIG)) {
        // Do not poll if the task is currently reading or configuring
    } else if (flags & CS_PENDING) {
        // Check for overflow if the task is late to read the sample
        if (!(flags & CS_OVERFLOW)) {
            uint32_t elapsed = timer_read_time() - cs1237->pending_since;
            if (elapsed > cs1237->period_ticks) {
                cs1237->sb.possible_overflows++;
                cs1237->flags = flags | CS_OVERFLOW;
            }
        }
    } else if (cs1237_is_data_ready(cs1237)) {
        // New sample is ready - wake the task to read it
        cs1237->pending_since = timer_read_time();
        cs1237->flags = flags | CS_PENDING;
        sched_wake_task(&wake_cs1237);

        // Schedule next check 75% into the next sample period. This
        // phase-locks the timer to the chip's sample rate, avoiding
        // excessive polling and preventing timer drift.
        cs1237->timer.waketime = cs1237->pending_since + cs1237->period_ticks
                                 - (cs1237->period_ticks / 4);
        return SF_RESCHEDULE;
    }

    // Schedule next poll; resync if we're already past due
    uint32_t next = cs1237->timer.waketime + rest_ticks;
    uint32_t now = timer_read_time();
    if (!timer_is_before(now, next))
        next = now + rest_ticks;

    cs1237->timer.waketime = next;
    return SF_RESCHEDULE;
}

static void
add_sample(struct cs1237_adc *cs1237, uint8_t oid, uint32_t counts,
           uint8_t force_flush)
{
    cs1237->sb.data[cs1237->sb.data_count] = counts;
    cs1237->sb.data[cs1237->sb.data_count + 1] = counts >> 8;
    cs1237->sb.data[cs1237->sb.data_count + 2] = counts >> 16;
    cs1237->sb.data[cs1237->sb.data_count + 3] = counts >> 24;
    cs1237->sb.data_count += BYTES_PER_SAMPLE;
    if (cs1237->sb.data_count + BYTES_PER_SAMPLE > ARRAY_SIZE(cs1237->sb.data)
        || force_flush)
        sensor_bulk_report(&cs1237->sb, oid);
}

// CS1237 ADC query
static void
cs1237_read_adc(struct cs1237_adc *cs1237, uint8_t oid)
{
    // claim bus
#if CONFIG_MACH_GD32F303_Q2
    cs1237_drdy_irq_disable(cs1237);
    if (cs1237->use_drdy_irq
        && timer_read_time() - cs1237->pending_since
           > cs1237->period_ticks)
        cs1237->sb.possible_overflows++;
#endif
    cs1237->flags = CS_READING;
    uint32_t counts = 0;
    if (cs1237->last_error == 0) {
        if (cs1237_is_data_ready(cs1237)) {
            uint8_t status_bits = 0;
            counts = cs1237_read_frame(cs1237, &status_bits);
            // update2 (bit 1) is datasheet-reserved and must read as 0.
            // A nonzero value indicates bit-bang desync or a wrong chip.
            if (status_bits & 0x02) {
                cs1237->last_error = SAMPLE_ERROR_CONFIG;
                counts = cs1237->last_error;
            } else {
                // Sign-extend 24-bit two's complement to 32 bits
                if (counts & 0x800000)
                    counts |= 0xFF000000;
                cs1237->last_sample = (int32_t)counts;
            }
        } else {
            // Task woke but DOUT is high: likely task starvation or race
            cs1237->last_error = SAMPLE_ERROR_READ_TOO_LONG;
            counts = cs1237->last_error;
        }
    } else {
        counts = cs1237->last_error;
    }
    // Release the bus
    cs1237->flags = 0;
#if CONFIG_MACH_GD32F303_Q2
    if (cs1237->last_error == 0)
        cs1237_drdy_irq_enable(cs1237);
#endif
    if (cs1237->last_error != 0) {
        // Report the error sentinel through the bulk stream and stop polling.
        // The host detects the sentinel and restarts capture to recover.
        sched_del_timer(&cs1237->timer);
        add_sample(cs1237, oid, counts, 1);
    } else {
        // probe is optional, report if enabled
        if (cs1237->lce)
            load_cell_probe_report_sample(cs1237->lce, counts);
        add_sample(cs1237, oid, counts, 0);
    }
}

// Create a cs1237 sensor
void
command_config_cs1237(uint32_t *args)
{
    struct cs1237_adc *cs1237 = oid_alloc(args[0]
                , command_config_cs1237, sizeof(*cs1237));
    cs1237->timer.func = cs1237_event;
    cs1237->config = args[1] & 0x7f;
    cs1237->dout_out = gpio_out_setup(args[2], 1);
    cs1237->dout_in = gpio_in_setup(args[2], 1);
    cs1237->sclk = gpio_out_setup(args[3], 1); // high -> power down
    cs1237->last_sample = 0;
    cs1237->lce = NULL;
#if CONFIG_MACH_GD32F303_Q2
    cs1237_drdy_irq_setup(cs1237, args[2]);
#endif
}
DECL_COMMAND(command_config_cs1237, "config_cs1237 oid=%c config=%c"
             " dout_pin=%u sclk_pin=%u");

void
cs1237_attach_load_cell_probe(uint32_t *args)
{
    struct cs1237_adc *cs1237 = oid_lookup(args[0], command_config_cs1237);
    cs1237->lce = load_cell_probe_oid_lookup(args[1]);
}
DECL_COMMAND(cs1237_attach_load_cell_probe, "cs1237_attach_load_cell_probe"
             " oid=%c load_cell_probe_oid=%c");

// Start/stop capturing ADC data
void
command_query_cs1237(uint32_t *args)
{
    struct cs1237_adc *cs1237 = oid_lookup(args[0], command_config_cs1237);
    uint8_t oid = args[0];
    sched_del_timer(&cs1237->timer);
    cs1237->flags = 0;
    cs1237->last_error = 0;
#if CONFIG_MACH_GD32F303_Q2
    cs1237_drdy_irq_disable(cs1237);
#endif

    if (!args[1]) {
        // End measurements - SCLK high for >100us enters power-down
        if (cs1237->sb.data_count)
            sensor_bulk_report(&cs1237->sb, oid);
        gpio_out_write(cs1237->sclk, 1);
        gpio_in_reset(cs1237->dout_in, 1);
        return;
    }

    // Calculate polling interval dynamically based on configured sample rate.
    // Poll at 1/8th of the sample period when searching for DRDY.
    cs1237->period_ticks = cs1237_sample_period_ticks(cs1237->config);
    cs1237->rest_ticks = cs1237->period_ticks / 8;
    if (cs1237->rest_ticks < timer_from_us(50))
        cs1237->rest_ticks = timer_from_us(50);

    // Start new measurements - defer chip config to the background task
    sensor_bulk_reset(&cs1237->sb);
    gpio_out_write(cs1237->sclk, 0); // begin power-up
    cs1237->flags = CS_NEED_CONFIG;
    sched_wake_task(&wake_cs1237);
#if CONFIG_MACH_GD32F303_Q2
    if (cs1237->use_drdy_irq)
        return;
#endif
    irq_disable();
    cs1237->timer.waketime = timer_read_time() + cs1237->rest_ticks;
    sched_add_timer(&cs1237->timer);
    irq_enable();
}
DECL_COMMAND(command_query_cs1237, "query_cs1237 oid=%c rest_ticks=%u");

void
command_query_cs1237_status(const uint32_t *args)
{
    uint8_t oid = args[0];
    struct cs1237_adc *cs1237 = oid_lookup(oid, command_config_cs1237);
    irq_disable();
    const uint32_t start_t = timer_read_time();
    uint8_t busy = cs1237->flags & (CS_READING | CS_NEED_CONFIG);
    uint8_t is_data_ready = !busy && cs1237_is_data_ready(cs1237);
    irq_enable();
    uint8_t pending_bytes = is_data_ready ? BYTES_PER_SAMPLE : 0;
    sensor_bulk_status(&cs1237->sb, oid, start_t, 0, pending_bytes);
}
DECL_COMMAND(command_query_cs1237_status, "query_cs1237_status oid=%c");

// Background task that performs measurements
void
cs1237_capture_task(void)
{
    if (!sched_check_wake(&wake_cs1237))
        return;
    uint8_t oid;
    struct cs1237_adc *cs1237;
    foreach_oid(oid, cs1237, command_config_cs1237) {
        uint8_t f = cs1237->flags;
        if (f & CS_NEED_CONFIG) {
            int ret = cs1237_configure_chip(cs1237);
            cs1237->flags &= ~CS_NEED_CONFIG;
            if (ret) {
                cs1237->last_error = SAMPLE_ERROR_CONFIG;
                sched_del_timer(&cs1237->timer);
                add_sample(cs1237, oid, cs1237->last_error, 1);
            }
#if CONFIG_MACH_GD32F303_Q2
            else
                cs1237_drdy_irq_enable(cs1237);
#endif
            continue;
        }
        if ((f & (CS_PENDING | CS_OVERFLOW)) && !(f & CS_READING))
            cs1237_read_adc(cs1237, oid);
    }
}
DECL_TASK(cs1237_capture_task);
