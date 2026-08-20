{
  gcc-arm-embedded,
  gcc,
  gnumake,
  python3,
  stdenvNoCC,
  src,
  config,
  pname,
  lib,
  buildDeployer ? false,
}: let
  firmware =
    if buildDeployer
    then "deployer.bin"
    else "katapult.bin";
in
  stdenvNoCC.mkDerivation {
    inherit pname;
    version = src.rev;

    inherit src;

    nativeBuildInputs = [
      gcc-arm-embedded
      gcc
      gnumake
      python3
    ];

    postPatch = ''
      patchShebangs scripts/check-gcc.sh
    '';

    dontConfigure = true;

    buildPhase = ''
      runHook preBuild
      cp ${config} .config
      make olddefconfig
      make -j$NIX_BUILD_CORES
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      mkdir -p "$out"
      install -m644 "out/${firmware}" "$out/${firmware}"
      install -Dm644 "${src}/LICENSE" "$out/LICENSE"
      runHook postInstall
    '';

    meta.license = lib.licenses.gpl3Only;
  }
