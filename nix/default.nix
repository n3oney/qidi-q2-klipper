{
  gcc-arm-embedded,
  gnumake,
  python3,
  stdenvNoCC,
  src,
  config,
  pname,
  lib,
}:
stdenvNoCC.mkDerivation rec {
  inherit src pname;
  version = src.rev;

  nativeBuildInputs = [
    gcc-arm-embedded
    gnumake
    python3
  ];

  patchPhase = ''
    patchShebangs scripts/find-firmware-extras.sh scripts/check-gcc.sh
  '';

  KALICO_BUILD_VERSION = "${src.name}-${builtins.substring 0 12 version}";

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    cp ${config} .config
    make olddefconfig
    mkdir -p .build-tools
    ln -s ${gcc-arm-embedded}/bin/arm-none-eabi-readelf .build-tools/readelf
    PATH="$PWD/.build-tools:$PATH" make CPP=arm-none-eabi-cpp -j$NIX_BUILD_CORES
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp out/klipper.bin $out/klipper.bin
    runHook postInstall
  '';

  meta.license = lib.licenses.gpl3Only;
}
