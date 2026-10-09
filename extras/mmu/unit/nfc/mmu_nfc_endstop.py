# Happy Hare MMU Software
#
# Copyright (C) 2022-2026  moggieuk#6538 (discord)
#                          moggieuk@hotmail.com
#
# Goal: Per-gate NFC/RFID reader exposed as a "software" homing endstop that
#       "triggers" when a tag is detected. Lets a gear/filament homing move stop
#       as soon as the spool's RFID tag reaches the reader.
#
# The manager polls for tag presence on the host. Detection also triggers the
# MCU's homing stop handler so queued steps cannot carry the tag past the reader.
#
# (\_/)
# ( *,*)
# (")_(") Happy Hare Ready
#
# This file may be distributed under the terms of the GNU GPLv3 license.
#

import mcu

from ...mmu_sensor_utils import MmuVirtualEndstopSensor


class MmuNfcEndstop(MmuVirtualEndstopSensor):
    """
    A host-polled NFC endstop with MCU step cancellation. Standalone scans own a
    trigger dispatch; compound scans share the physical gate endstop's dispatch
    because a stepper can only have one armed stop handler. The host-request
    reason stops the motor without reporting a false physical gate hit.
    """

    def __init__(self, config, gate, reader, poll_controller, register=False):
        # name becomes "<SENSOR_NFC_PREFIX>_<gate>" via the MmuSensor base
        from ...mmu_constants import SENSOR_NFC_PREFIX
        super().__init__(config, SENSOR_NFC_PREFIX, gate, register=register)
        self.gate = gate
        self.reader = reader
        self._poll_controller = poll_controller
        self._dispatch = None
        self._homing_mcu_endstop = None
        self._stop_dispatch = None
        self._owns_dispatch = False


    def add_stepper(self, stepper):
        super().add_stepper(stepper)
        if self._dispatch is None:
            self._dispatch = mcu.TriggerDispatch(stepper.get_mcu())
        self._dispatch.add_stepper(stepper)


    def set_homing_mcu_endstop(self, endstop):
        # Used for one home only; the compound owns this dispatch's lifecycle.
        self._homing_mcu_endstop = endstop


    def trigger_handler(self, eventtime, state):
        if self._homing and state and self._last_trigger_time is None:
            # Use the already-armed dispatch on every participating MCU. These
            # commands cancel queued steps, not just future host scheduling.
            for trsync in self._stop_dispatch._trsyncs:
                trsync._trsync_trigger_cmd.send(
                    [trsync.get_oid(), mcu.MCU_trsync.REASON_HOST_REQUEST])
            self._poll_controller.mmu.log_debug(
                "NFC: gate %d requested MCU stop at tag detection" % self.gate)
        super().trigger_handler(eventtime, state)


    def _endstop_trigger_time(self, eventtime):
        # The manager's poll drives trigger_handler from a host reactor timer, so
        # convert to MCU print_time for the homing trigger position calc.
        # (estimated_print_time is inherited from MmuVirtualEndstopSensor.)
        return self.estimated_print_time(eventtime)


    # Endstop homing interface -----------------------------------------------------

    def home_start(self, print_time, sample_time, sample_count, rest_time, triggered):
        shared, self._homing_mcu_endstop = self._homing_mcu_endstop, None
        if shared is not None:
            if any(s not in shared.get_steppers() for s in self.get_steppers()):
                raise self.printer.command_error(
                    "NFC and gate endstops must share their homing steppers")
            self._stop_dispatch = shared._dispatch
        else:
            self._stop_dispatch = self._dispatch
        if self._stop_dispatch is None:
            raise self.printer.command_error("No stepper bound to NFC endstop")
        self._owns_dispatch = shared is None

        self.runout_helper.note_filament_present(self.runout_helper.reactor.monotonic(), False)
        # Presence always triggers, including a backward scan.
        completion = super().home_start(
            print_time, sample_time, sample_count, rest_time, True)
        if self._owns_dispatch:
            # The MCU completion also stops the move on communication failure.
            completion = self._stop_dispatch.start(print_time)
        self._poll_controller.start_homing_poll(self)
        return completion


    def home_wait(self, home_end_time):
        try:
            self._poll_controller.stop_homing_poll()
            if self._owns_dispatch:
                try:
                    self._stop_dispatch.wait_end(home_end_time)
                finally:
                    reason = self._stop_dispatch.stop()
                if reason >= mcu.MCU_trsync.REASON_COMMS_TIMEOUT:
                    raise self.printer.command_error("Communication timeout during NFC homing")
                if reason != mcu.MCU_trsync.REASON_HOST_REQUEST:
                    self._last_trigger_time = None
            return super().home_wait(home_end_time)
        finally:
            self._homing = False
            self._trigger_completion = None
            self._stop_dispatch = None
            self._owns_dispatch = False
