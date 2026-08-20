{
  bc,
  bison,
  dtc,
  flex,
  kmod,
  lib,
  lz4,
  openssl,
  perl,
  pkgsCross,
  python3,
  stdenv,
  kernelSrc,
  ubootSrc,
  wifiSrc,
  forQemu ? false,
}: let
  crossCc = pkgsCross.aarch64-multiplatform.stdenv.cc;
  crossPrefix = "${crossCc}/bin/${crossCc.targetPrefix}";
  board = "rk3308bs-evb-amic-v11";
  bootPartitionSize = 9437184;
in
  stdenv.mkDerivation {
    pname = if forQemu then "qidi-q2-kernel-qemu" else "qidi-q2-kernel";
    version = "6.12.69";
    src = kernelSrc;
    patches = lib.optional (!forQemu) ./0001-rockchip-fix-non-iommu-mmap.patch;

    nativeBuildInputs = [
      bc
      bison
      crossCc
      dtc
      flex
      kmod
      lz4
      openssl
      perl
      python3
    ];

    hardeningDisable = ["all"];
    dontConfigure = true;
    dontStrip = true;
    dontPatchELF = true;

    postPatch = ''
      patchShebangs scripts
    '';

    buildPhase = ''
      runHook preBuild

      export ARCH=arm64
      export CROSS_COMPILE=${crossPrefix}
      export KBUILD_BUILD_HOST=nix
      export KBUILD_BUILD_USER=nix
      export KBUILD_BUILD_TIMESTAMP="Thu Jan 1 00:00:01 UTC 1970"
      export KBUILD_BUILD_VERSION=1
      buildDir="$PWD/build"
      make O="$buildDir" rk3308_linux_defconfig
      scripts/kconfig/merge_config.sh -m -O "$buildDir" \
        "$buildDir/.config" ${./q2.config} ${lib.optionalString forQemu "${./qemu.config}"}
      make O="$buildDir" olddefconfig
      make -j"$NIX_BUILD_CORES" O="$buildDir" ${if forQemu then "Image" else "Image.lz4 modules"}

      ${lib.optionalString (!forQemu) ''
        cp -R ${wifiSrc} wifi
        chmod -R u+w wifi
        make -j"$NIX_BUILD_CORES" -C wifi \
          ARCH=arm64 \
          CROSS_COMPILE="$CROSS_COMPILE" \
          KSRC="$buildDir"
        $CC -O2 -Wall -Wextra \
          ${ubootSrc}/tools/rockchip/resource_tool.c \
          -o scripts/resource_tool
        mkdir resource-files
        ${dtc}/bin/dtc -I dts -O dtb -o resource-files/rk-kernel.dtb ${./output.dts}
        install -m644 ${./logo.bmp} resource-files/logo.bmp
        install -m644 ${./logo.bmp} resource-files/logo_kernel.bmp
        (
          cd resource-files
          ../scripts/resource_tool --pack --image=../resource.img \
            rk-kernel.dtb logo.bmp logo_kernel.bmp
        )
        install -Dm644 resource-files/rk-kernel.dtb \
          "$buildDir/arch/arm64/boot/dts/rockchip/${board}.dtb"
        scripts/mkbootimg \
          --kernel "$buildDir/arch/arm64/boot/Image.lz4" \
          --second resource.img \
          -o zboot.img
      ''}

      runHook postBuild
    '';

    doCheck = !forQemu;
    checkPhase = ''
      ${python3}/bin/python ${./verify-boot.py} zboot.img \
        --max-size ${toString bootPartitionSize} \
        --resource resource.img
      runHook postCheck
    '';

    installPhase = ''
      runHook preInstall

      buildDir="$PWD/build"
      kernelRelease="$(make -s O="$buildDir" kernelrelease)"

      mkdir -p "$out/boot"
      install -m644 "$buildDir/arch/arm64/boot/Image" "$out/boot/Image"
      install -m644 "$buildDir/.config" "$out/boot/kernel.config"
      install -m644 COPYING "$out/COPYING"
      ${lib.optionalString (!forQemu) ''
        mkdir -p "$out/rootfs"
        install -m644 zboot.img "$out/boot/boot.img"
        install -m644 resource.img "$out/boot/resource.img"
        install -m644 "$buildDir/arch/arm64/boot/Image.lz4" \
          "$out/boot/Image.lz4"
        install -m644 \
          "$buildDir/arch/arm64/boot/dts/rockchip/${board}.dtb" \
          "$out/boot/${board}.dtb"
        make O="$buildDir" \
          INSTALL_MOD_PATH="$out/rootfs" \
          modules_install
        rm -f \
          "$out/rootfs/lib/modules/$kernelRelease/build" \
          "$out/rootfs/lib/modules/$kernelRelease/source"
        install -Dm644 wifi/8189fs.ko \
          "$out/rootfs/lib/modules/$kernelRelease/extra/8189fs.ko"
        depmod -b "$out/rootfs" "$kernelRelease"
      ''}

      runHook postInstall
    '';

    meta = {
      description = if forQemu then "QIDI Q2 kernel for QEMU virt" else "QIDI Q2 Rockchip RK3308B-S kernel and boot image";
      homepage = "https://github.com/rockchip-linux/kernel";
      license = lib.licenses.gpl2Only;
      platforms = lib.platforms.linux;
    };
  }
