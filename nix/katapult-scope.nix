{
  lib,
  newScope,
  runCommand,
  src,
}:
lib.makeScope newScope (
  self: let
    readConfigs = self.callPackage ./read-configs.nix {};
    directConfigs = readConfigs ../configs/katapult;
    deployerConfigs = readConfigs ../configs/katapult-deployer;

    patchedKatapult = self.callPackage ./patched-katapult-source.nix {
      inherit src;
    };

    mkFirmware = buildDeployer: board: config:
      self.callPackage ./katapult.nix {
        src = patchedKatapult;
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
          install -m644 "${patchedKatapult}/LICENSE" "$out/LICENSE"

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
    katapult-source = patchedKatapult;
    katapult = mkFirmwareSet directConfigs false;
    katapult-deployer = mkFirmwareSet deployerConfigs true;
  }
)
