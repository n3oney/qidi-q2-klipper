# CS1237 Support
#
# Copyright (C) 2026  The Klipper project
#
# This file may be distributed under the terms of the GNU GPLv3 license.
#
# CS1237 config/register logic based on code from:
# https://codeberg.org/kuwoyuki/klipper-q2
# Adapted to Kalico's LoadCellSensor interface (modeled on hx71x.py).
import logging

from klippy.mcu import MCU

from .. import bulk_sensor
from .interfaces import BulkAdcData, BulkAdcDataCallback, LoadCellSensor

#
# Constants
#
UPDATE_INTERVAL = 0.10
# Error sentinels emitted by src/sensor_cs1237.c through the bulk stream
SAMPLE_ERROR_TIMEOUT = -0x80000000
SAMPLE_ERROR_LONG_READ = 0x40000000
SAMPLE_ERROR_CONFIG = 0x20000000
# Config register bit fields
SAMPLE_RATE_OPTIONS = {10: 0, 40: 1, 640: 2, 1280: 3}
GAIN_OPTIONS = {"1": 0, "2": 1, "64": 2, "128": 3}
CHANNEL_OPTIONS = {"A": 0, "temperature": 2, "short": 3}


class CS1237(LoadCellSensor):
    def __init__(self, config):
        self.printer = printer = config.get_printer()
        self.name = config.get_name().split()[-1]
        self.last_error_count = 0
        self.consecutive_fails = 0
        self.sensor_type = "cs1237"
        # Chip options
        dout_pin_name = config.get("dout_pin")
        sclk_pin_name = config.get("sclk_pin")
        ppins = printer.lookup_object("pins")
        dout_ppin = ppins.lookup_pin(dout_pin_name)
        sclk_ppin = ppins.lookup_pin(sclk_pin_name)
        mcu: MCU = dout_ppin["chip"]
        self.mcu: MCU = mcu
        self.oid = mcu.create_oid()
        if sclk_ppin["chip"] is not mcu:
            raise config.error(
                "%s config error: All pins must be "
                "connected to the same MCU" % (self.name,)
            )
        self.dout_pin = dout_ppin["pin"]
        self.sclk_pin = sclk_ppin["pin"]
        # Config register setup
        self.sps = config.getchoice(
            "sample_rate", {k: k for k in SAMPLE_RATE_OPTIONS}, default=1280
        )
        speed_sel = SAMPLE_RATE_OPTIONS[self.sps]
        gain_bits = config.getchoice("gain", GAIN_OPTIONS, default="128")
        chan_sel = config.getchoice("channel", CHANNEL_OPTIONS, default="A")
        refout_off = int(config.getboolean("refout_off", default=False))
        config_reg = (refout_off << 6) | (speed_sel << 4)
        config_reg |= (gain_bits << 2) | chan_sel
        ## Bulk Sensor Setup
        chip_smooth = self.sps * UPDATE_INTERVAL * 2
        self.ffreader = bulk_sensor.FixedFreqReader(mcu, chip_smooth, "<i")
        self.batch_bulk = bulk_sensor.BatchBulkHelper(
            self.printer,
            self._process_batch,
            self._start_measurements,
            self._finish_measurements,
            UPDATE_INTERVAL,
        )
        # Command Configuration
        self.query_cs1237_cmd = None
        self.attach_probe_cmd = None
        mcu.add_config_cmd(
            "config_cs1237 oid=%d config=%d dout_pin=%s sclk_pin=%s"
            % (self.oid, config_reg, self.dout_pin, self.sclk_pin)
        )
        mcu.add_config_cmd(
            "query_cs1237 oid=%d rest_ticks=0" % (self.oid,), on_restart=True
        )
        mcu.register_config_callback(self._build_config)

    def _build_config(self):
        self.query_cs1237_cmd = self.mcu.lookup_command(
            "query_cs1237 oid=%c rest_ticks=%u"
        )
        self.attach_probe_cmd = self.mcu.lookup_command(
            "cs1237_attach_load_cell_probe oid=%c load_cell_probe_oid=%c"
        )
        self.ffreader.setup_query_command(
            "query_cs1237_status oid=%c",
            oid=self.oid,
            cq=self.mcu.alloc_command_queue(),
        )

    def get_mcu(self) -> MCU:
        return self.mcu

    def get_samples_per_second(self) -> int:
        return self.sps

    # returns a tuple of the minimum and maximum value of the sensor, used to
    # detect if a data value is saturated
    def get_range(self) -> tuple[int, int]:
        return -0x800000, 0x7FFFFF

    def get_channel_count(self) -> int:
        return 1

    # add_client interface, direct pass through to bulk_sensor API
    def add_client(self, callback: BulkAdcDataCallback):
        self.batch_bulk.add_client(callback)

    def attach_load_cell_probe(self, load_cell_probe_oid: int):
        self.attach_probe_cmd.send([self.oid, load_cell_probe_oid])

    # Measurement decoding
    def _convert_samples(self, samples):
        adc_factor = 1.0 / (1 << 23)
        count = 0
        for ptime, val in samples:
            if val in (
                SAMPLE_ERROR_TIMEOUT,
                SAMPLE_ERROR_LONG_READ,
                SAMPLE_ERROR_CONFIG,
            ):
                self.last_error_count += 1
                break  # additional errors are duplicates
            samples[count] = (round(ptime, 6), val, round(val * adc_factor, 9))
            count += 1
        del samples[count:]

    # Start, stop, and process message batches
    def _start_measurements(self):
        self.consecutive_fails = 0
        self.last_error_count = 0
        # Start bulk reading
        rest_ticks = self.mcu.seconds_to_clock(1.0 / (10.0 * self.sps))
        self.query_cs1237_cmd.send([self.oid, rest_ticks])
        logging.info("CS1237 starting '%s' measurements", self.name)
        # Initialize clock tracking
        self.ffreader.note_start()

    def _finish_measurements(self):
        # don't use serial connection after shutdown
        if self.printer.is_shutdown():
            return
        # Halt bulk reading
        self.query_cs1237_cmd.send_wait_ack([self.oid, 0])
        self.ffreader.note_end()
        logging.info("CS1237 finished '%s' measurements", self.name)

    def _process_batch(self, eventtime) -> BulkAdcData:
        prev_overflows = self.ffreader.get_last_overflows()
        prev_error_count = self.last_error_count
        samples = self.ffreader.pull_samples()
        self._convert_samples(samples)
        overflows = self.ffreader.get_last_overflows() - prev_overflows
        errors = self.last_error_count - prev_error_count
        if errors > 0:
            logging.error("%s: Forced sensor restart due to error", self.name)
            self._finish_measurements()
            self._start_measurements()
        elif overflows > 0:
            self.consecutive_fails += 1
            if self.consecutive_fails > 4:
                logging.error(
                    "%s: Forced sensor restart due to overflows", self.name
                )
                self._finish_measurements()
                self._start_measurements()
        else:
            self.consecutive_fails = 0
        return {
            "data": samples,
            "errors": self.last_error_count,
            "overflows": self.ffreader.get_last_overflows(),
        }


CS1237_SENSOR_TYPES = {"cs1237": CS1237, "c_sensor": CS1237}
