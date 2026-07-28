{
  lib,
  newScope,
  runCommand,
  name,
  src,
  patches,
  applyPatches,
}:
lib.makeScope newScope (
  self: let
    readConfigs = self.callPackage ./read-configs.nix {};
    configs = readConfigs ../configs/kalico;

    kalico = applyPatches {
      inherit src name patches;
    };

    firmwares = lib.mapAttrs (configName: config:
      self.callPackage ./default.nix {
        src = kalico;
        inherit config;
        pname = "${name}-${configName}";
      })
    configs;

    allFirmwares = runCommand "${name}-firmwares" {
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

    full = runCommand "${name}-full" {
      meta.license = lib.licenses.gpl3Only;
    } ''
      mkdir -p "$out"
      cp -r "${kalico}/." "$out/"

      mkdir -p "$out/firmwares"
      cp -r "${allFirmwares}/." "$out/firmwares/"
    '';
  in {
    inherit kalico full;
    firmwares = firmwares // {all = allFirmwares;};
  }
)
