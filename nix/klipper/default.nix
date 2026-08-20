{
  pkgs,
  kalicoBleedingEdge,
}: let
  inherit (pkgs) applyPatches fetchpatch2 newScope runCommand;
  lib = pkgs.lib;
  readConfigs = import ../lib/read-configs.nix {inherit lib;};
  configs = readConfigs ./configs;

  mkScope = {
    src,
    name,
    patches,
  }:
    lib.makeScope newScope (
      self: let
        patchedSource = applyPatches {
          inherit src name patches;
        };

        firmwares =
          lib.mapAttrs (
            configName: config:
              self.callPackage ./firmware.nix {
                src = patchedSource;
                inherit config;
                pname = "${name}-${configName}";
              }
          )
          configs;

        allFirmwares =
          runCommand "${name}-firmwares" {
            meta.license = lib.licenses.gpl3Only;
          } ''
            mkdir -p "$out"

            ${lib.concatStringsSep "\n" (
              lib.mapAttrsToList (firmwareName: drv: ''
                cp "${drv}/klipper.bin" "$out/${firmwareName}.bin"
              '')
              firmwares
            )}
          '';

        full =
          runCommand "${name}-full" {
            meta.license = lib.licenses.gpl3Only;
          } ''
            mkdir -p "$out"
            cp -r "${patchedSource}/." "$out/"

            mkdir -p "$out/firmwares"
            cp -r "${allFirmwares}/." "$out/firmwares/"
          '';
      in {
        kalico = patchedSource;
        inherit full;
        firmwares = firmwares // {all = allFirmwares;};
      }
    );

  kalicoBleedingEdgePatches = [
    ./patches/0001-q2-gd32f425-usb.patch
    ./patches/0002-q2-cs1237.patch

    ./patches/0003-q2-multi-mcu-timeout.patch

    ./patches/0004-q2-gd32f303-spi2.patch
    ./patches/0005-q2-gd32f425-temperature.patch

    (fetchpatch2 {
      url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0006-stm32-add-Q2-GD32F303-120MHz-target.patch";
      excludes = ["src/stm32/Kconfig"];
      hash = "sha256-l6QpT54icFUWB9t5KhC5C7WmozGv+OHiTnWIkZM7BNM=n";
    })
    # Everything from the upstream patch is compatible with Kalico except Kconfig.
    ./patches/0006-q2-gd32f303-120mhz-kconfig.patch

    (fetchpatch2 {
      url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0007-stm32-add-Q2-GD32F425-200MHz-support.patch";
      excludes = ["src/stm32/stm32f4.c"];
      hash = "sha256-9XM20Tfqc11blDLEr3X7Dmh7bBh+WHLLhAOfSbSxpwQ=";
    })
    # Everything from the upstream patch works with Kalico except stm32f4.c.
    ./patches/0007-q2-gd32f425-200mhz-stm32f4.patch

    ./patches/0008-build-version-override.patch
    ./patches/0009-load-cell-reduce-status-allocations.patch
    ./patches/0010-thr-uart-tx-buffer.patch
    ./patches/0011-webhooks-msgspec-json.patch
    ./patches/0013-compat-ignore-namespace-packages.patch
    ./patches/0014-manual-home-probe-position.patch
    # https://github.com/KalicoCrew/kalico/pull/932
    ./patches/0012-motion-queuing-steppersync.patch
    (fetchpatch2 {
      url = "https://github.com/Klipper3d/klipper/commit/c49dbb5a879df16ebf3014ef0901eb9dd61e6225.patch";
      hash = "sha256-xodhwjuTUHh7KNghpLwnxi4ff7M4Ms49W1OG2pIRbjg=";
    })
    ./patches/0015-motion-queuing-disconnect-cleanup.patch
  ];

in {
  kalico-bleeding-edge = mkScope {
    src = kalicoBleedingEdge;
    name = "qidi-q2-kalico-bleeding-edge-v2";
    patches = kalicoBleedingEdgePatches;
  };
}
