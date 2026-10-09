# QIDI Q2 Klipper/Kalico builds

> [!CAUTION]
> This repository is currently in an unstable, work in progress state.
> Expect crashes.

Patched Klipper, Kalico and Katapult builds, plus a Linux kernel for the QIDI Q2.

> [!NOTE]
> I personally use Kalico (bleeding edge), so that variant is the most "stable" here.

## Flashing Katapult without an ST-Link

1. Connect to the printer over SSH.

2. Download and extract the latest Katapult deployer release on the printer:

```sh
# in ssh, not on your computer
mkdir -p ~/qidi-q2-klipper/katapult-deployer
cd ~/qidi-q2-klipper
wget -O katapult-deployer.tar.gz \
  'https://github.com/n3oney/qidi-q2-klipper/releases/latest/download/katapult-deployer.tar.gz'
tar -xzf katapult-deployer.tar.gz -C katapult-deployer
```

The `katapult-deployer` directory contains:

- `mcu-deployer.bin`
- `thr-deployer.bin`
- `mmu-deployer.bin`
- `LICENSE`

3. Stop `klipper` and `klipper-mcu`:

```sh
sudo systemctl stop klipper klipper-mcu
```

4. Flash the main MCU:

> [!CAUTION]
> If you lose power in the flashing process, you might brick one of your MCUs.  
> You can only recover that using an ST-Link.

```sh
sudo ~/mcu_update.sh ~/qidi-q2-klipper/katapult-deployer/mcu-deployer.bin
```

5. Flash the toolhead board:

```sh
sudo ~/mcu_update_THR.sh ~/qidi-q2-klipper/katapult-deployer/thr-deployer.bin
```

6. Flash the box (if you have it):

```sh
ls /dev/serial/by-id

# If it has something like usb-Klipper_QIDI_BOX_V2:
sudo ~/mcu_update_BOX_to_v2.sh ~/qidi-q2-klipper/katapult-deployer/mmu-deployer.bin '/dev/serial/by-id/usb-Klipper_QIDI_BOX_V2*-if00'

# OR
# If it has something like usb-Klipper_stm32f401xc:
sudo ~/mcu_update_BOX_to_v2.sh ~/qidi-q2-klipper/katapult-deployer/mmu-deployer.bin '/dev/serial/by-id/usb-Klipper_stm32f401xc_*-if00'
```

## Flashing Kalico

1. On the printer:

```sh
mkdir -p ~/qidi-q2-klipper/klipper
cd ~/qidi-q2-klipper
```

2. Download Kalico `bleeding-edge-v2`:

```sh
wget -O klipper.tar.gz \
  'https://github.com/n3oney/qidi-q2-klipper/releases/latest/download/kalico-bleeding-edge.tar.gz'
```

3. Unpack it:

```sh
tar -xzf klipper.tar.gz -C klipper
```

The extracted `klipper/firmwares` directory contains:

- `mcu.bin`
- `thr.bin`
- `mmu.bin`

These can now be flashed with Katapult.

4. Download Katapult's `flashtool.py` on the printer:

```sh
curl -L https://raw.githubusercontent.com/Arksine/Katapult/master/scripts/flashtool.py -o ~/flashtool.py
```

5. Check that the main MCU is running Katapult with the correct application offset:

```sh
~/klippy-env/bin/python ~/flashtool.py -d /dev/serial/by-id/usb-katapult_stm32f407xx_*-if00 -s
```

The status output must include:

```text
Application Start: 0x8008000
MCU type: stm32f407xx
```

6. Flash the main MCU firmware:

```sh
~/klippy-env/bin/python ~/flashtool.py -d /dev/serial/by-id/usb-katapult_stm32f407xx_*-if00 -f ~/qidi-q2-klipper/klipper/firmwares/mcu.bin
```

Wait for `Verification Complete` and `Programming Complete`.

7. Verify that the main MCU rebooted into Klipper:

```sh
ls /dev/serial/by-id
# entry starting with usb-Klipper_stm32f407xx is expected
```

8. Check that the THR is running Katapult with the right offset:

```sh
~/klippy-env/bin/python ~/flashtool.py -d /dev/ttyS4 -b 500000 -s
```

The output must include:

```text
Application Start: 0x8002000
MCU type: stm32f103xe
```

9. Flash the THR firmware:

```sh
~/klippy-env/bin/python ~/flashtool.py -d /dev/ttyS4 -b 500000 -f ~/qidi-q2-klipper/klipper/firmwares/thr.bin
```

### Flashing the box/MMU

Skip this section if your printer does not have the box.

1. Check that the box/MMU is running Katapult with the correct application offset:

```sh
~/klippy-env/bin/python ~/flashtool.py -d /dev/serial/by-id/usb-katapult_stm32f401xc_*-if00 -s
```

The status output must include:

```text
Application Start: 0x8004000
MCU type: stm32f401xc
```

2. Flash the box/MMU firmware:

```sh
~/klippy-env/bin/python ~/flashtool.py -d /dev/serial/by-id/usb-katapult_stm32f401xc_*-if00 -f ~/qidi-q2-klipper/klipper/firmwares/mmu.bin
```

Wait for `Verification Complete` and `Programming Complete`.

3. Verify that the box/MMU rebooted into Klipper:

```sh
ls /dev/serial/by-id
# entry starting with usb-Klipper_stm32f401xc is expected
```

### Installing the Klipper host

Keep `klipper` and `klipper-mcu` stopped until every converted MCU has its matching Klipper firmware and the Klipper host tree is installed.

1. On the printer, back up the existing Klipper tree and install the downloaded Klipper host tree:

```sh
test ! -e ~/klipper.pre-mainline &&
  mv ~/klipper ~/klipper.pre-mainline &&
  git clone https://github.com/n3oney/qidi-q2-klipper --single-branch -b kalico-bleeding-edge klipper
```

This command chain stops before changing `~/klipper` if the `~/klipper.pre-mainline` backup path already exists.

> [!NOTE]
> You will have to re-install stuff into extras.

2. Install Klipper's pinned Python requirements into the printer's existing Klipper environment:

```sh
~/klippy-env/bin/pip install -r ~/klipper/scripts/klippy-requirements.txt
```

3. Start the services only after the host tree, Python requirements, and every converted MCU firmware are installed:

```sh
sudo systemctl start klipper klipper-mcu
```

### Config setup

Refer to [my config](https://github.com/n3oney/q2-config) for a full, functional example.

## Load cell probe: mainline `trigger_analog` port

Patch `nix/klipper/patches/0018-load-cell-probe-trigger-analog.patch` replaces
Kalico's `load_cell_probe` (MCU `load_cell_probe.c`, host tap analysis and
tap quality classifier) with the current mainline Klipper stack:

- MCU: `trigger_analog.c` + the new `sos_filter.c` (offset/scale, continuous
  tare), all four load cell sensors (`hx71x`, `cs1237`, `ads1220`,
  `ads131m0x`) report samples to `trigger_analog`. The Q2 `sensor_cs1237.c`
  keeps its DRDY interrupt on `PB4`.
- Host: `klippy/extras/trigger_analog.py` (mainline), a rewritten
  `klippy/extras/load_cell/load_cell_probe.py` that keeps Kalico's
  `PrinterProbe` (retry strategies, `bad_probe_*` options) and adds the
  mainline ascent least-squares fit after each tap.

The MCU protocol changes, so the `thr.bin` firmware must be flashed together
with the host tree (Katapult, see above).

Config changes for an existing `[load_cell_probe]` section:

- unchanged: `sensor_type`, pins, `sample_rate`, `gain`, `channel`,
  `refout_off`, `sensor_orientation`, `z_offset`, `speed`, `lift_speed`,
  `samples`, `sample_retract_dist`, `samples_*`, `trigger_force`,
  `force_safety_limit` (0 still disables), `counts_per_gram`,
  `reference_tare_counts`, `bad_probe_strategy`, `bad_probe_retries`.
- removed (delete them if present): `drift_safety_limit`,
  `disable_pullback_move`, `pullback_distance`, `pullback_speed`,
  `tap_classifier`, `min_tap_quality`, `decompression_angle`,
  `max_approach_force`, `max_departure_force`,
  `max_baseline_force_delta`, `max_dwell_force_drop`.
- `sample_retract_dist` is now the lift performed during the tap while the
  ascent samples are collected (the probe helper does not lift a second time).
- optional continuous tare filters (`drift_filter_cutoff_frequency`,
  `buzz_filter_cutoff_frequency`, `notch_filter_frequencies`) need SciPy in
  `~/klippy-env`.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).

## Acknowledgements

This project builds on the QIDI Q2 research and mainline Klipper work from:

- [MisterSheikh/Qidi_Q2_Mainline_Klipper](https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper)
- [kuwoyuki/klipper-q2](https://codeberg.org/kuwoyuki/klipper-q2)

Huge thanks to them for making this possible.
