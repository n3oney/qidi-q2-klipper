{
  pkgs,
  src,
}: let
  inherit (pkgs) applyPatches fetchpatch2 newScope runCommand;
  lib = pkgs.lib;
  readConfigs = import ../lib/read-configs.nix {inherit lib;};
  firmwareConfigs = readConfigs ./configs/firmware;
  deployerConfigs = readConfigs ./configs/deployer;

  patchedSource = applyPatches {
    name = "katapult-qidi";
    inherit src;

    patches = [
      (fetchpatch2 {
        url = "https://raw.githubusercontent.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/f9f6cf04e5e4b9442c23aefeff6061a8bedcbf29/patches/katapult/0001-q2-mainboard-usb.patch";
        hash = "sha256-aSBnGJ4EU3rK3y4aCD1lk/w6z8Y4/9LAWThGHg/sHyM=";
      })
    ];
  };
in
  lib.makeScope newScope (
    self: let
      mkFirmware = buildDeployer: board: config:
        self.callPackage ./firmware.nix {
          src = patchedSource;
          inherit config buildDeployer;
          pname = "katapult-qidi-${board}${lib.optionalString buildDeployer "-deployer"}";
        };

      mkFirmwareSet = configs: buildDeployer: let
        firmwares = lib.mapAttrs (mkFirmware buildDeployer) configs;
        firmware =
          if buildDeployer
          then "deployer.bin"
          else "katapult.bin";
        suffix =
          if buildDeployer
          then "deployer"
          else "katapult";
        all =
          runCommand "katapult-qidi-${suffix}-firmwares" {
            meta.license = lib.licenses.gpl3Only;
          } ''
            mkdir -p "$out"
            install -m644 "${patchedSource}/LICENSE" "$out/LICENSE"

            ${lib.concatStringsSep "\n" (
              lib.mapAttrsToList (board: drv: ''
                cp "${drv}/${firmware}" "$out/${board}-${suffix}.bin"
              '')
              firmwares
            )}
          '';
      in
        firmwares // {inherit all;};
    in {
      katapult-source = patchedSource;
      katapult = mkFirmwareSet firmwareConfigs false;
      katapult-deployer = mkFirmwareSet deployerConfigs true;
    }
  )
