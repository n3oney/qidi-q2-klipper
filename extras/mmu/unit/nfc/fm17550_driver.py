# klippy/extras/mmu/unit/nfc/fm17550_driver.py
#
# FM17550 SPI driver. The FM17550 register interface and ISO14443-A command
# engine are compatible with the MFRC522. Keep the protocol implementation in
# RC522Driver and specialize only chip identification/health diagnostics here.
#
# SPDX-License-Identifier: GPL-3.0-or-later

from .log import logger
from .rc522_driver import RC522Driver, _ModeReg, _TxControlReg


class FM17550Driver(RC522Driver):
    """Fudan FM17550 using its MFRC522-compatible SPI command engine."""

    def _report_antenna_failure(self, tx_final):
        mode = self._read(_ModeReg)
        if tx_final == 0xFF or mode == 0xFF:
            cause = "MISO is undriven or chip-select/bus is wrong"
        elif tx_final == 0x00 and mode == 0x00:
            cause = "no register data; check power, ground, NPD, MOSI and MISO"
        elif mode != 0x3D:
            cause = "configuration writes do not read back; try spi_speed 100000"
        else:
            cause = "register writes land but the antenna TX outputs remain disabled"
        logger.warning(
            "[%s fm17550] antenna initialization failed (TxControl=0x%02X "
            "Mode=0x%02X %s): %s",
            self._name, tx_final, mode, self._bus_description(), cause)

    def is_alive(self):
        """Require real register values; an undriven SPI MISO reads as 0xFF."""
        try:
            tx = self._read(_TxControlReg)
            mode = self._read(_ModeReg)
            alive = tx not in (0x00, 0xFF) and mode == 0x3D and bool(tx & 0x03)
            if not alive:
                logger.warning(
                    "[%s fm17550] not responding (TxControl=0x%02X Mode=0x%02X)",
                    self._name, tx, mode)
            elif self._debug >= 4:
                logger.info(
                    "[%s fm17550] alive (TxControl=0x%02X Mode=0x%02X)",
                    self._name, tx, mode)
            return alive
        except Exception as e:
            logger.warning("[%s fm17550] health check failed: %s", self._name, e)
            return False
