{lib}:
configDir:
lib.mapAttrs' (configFile: _: {
  name = lib.removeSuffix ".config" configFile;
  value = configDir + "/${configFile}";
}) (lib.filterAttrs (configFile: type: type == "regular" && lib.hasSuffix ".config" configFile) (builtins.readDir configDir))
