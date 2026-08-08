{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

    kalico-main = {
      url = "github:KalicoCrew/kalico";
      flake = false;
    };
    kalico-bleeding-edge = {
      url = "github:KalicoCrew/kalico/bleeding-edge-v2";
      flake = false;
    };
    klipper = {
      url = "github:klipper3d/klipper";
      flake = false;
    };

    katapult = {
      url = "github:Arksine/Katapult";
      flake = false;
    };
  };

  nixConfig = {
    extra-trusted-public-keys = "neoney.cachix.org-1:bsFaTdG04tfzci0osGfosbRX8KX94Ih/2hU0HpJ+qRM=";
    extra-substituters = "https://neoney.cachix.org";
  };

  outputs = {
    nixpkgs,
    kalico-main,
    kalico-bleeding-edge,
    klipper,
    katapult,
    ...
  }: let
    lib = nixpkgs.lib;
    forAllSystems = lib.genAttrs lib.systems.flakeExposed;

    kalicoPatches = [
      ./patches/0001-q2-gd32f425-usb.patch
      ./patches/0002-q2-cs1237.patch
      ./patches/0003-build-version-override.patch
    ];

    kalicoScopes = forAllSystems (
      system: let
        pkgs = nixpkgs.legacyPackages.${system};
        mkKalicoScope = src: name: patches:
          pkgs.callPackage ./nix/kalico-scope.nix {
            inherit src name patches;
          };
      in {
        kalico-main = mkKalicoScope kalico-main "qidi-q2-kalico" kalicoPatches;
        kalico-bleeding-edge = mkKalicoScope kalico-bleeding-edge "qidi-q2-kalico-bleeding-edge-v2" kalicoPatches;
        klipper = mkKalicoScope klipper "qidi-q2-klipper" [
          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0001-stm32-add-GD32F425-USB-workaround.patch";
            hash = "sha256-lDHhrkxYHuBsYM/nDtyLrMuhhBMpsKl0YgjfwuRRKhA=";
          })

          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0002-load_cell-add-CS1237-ADC-support.patch";
            hash = "sha256-cV8WcTNa2Xf7WMdT3Iu8qJSg6EALIKG4H5QBBh8F8FQ=";
          })

          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0003-mcu-extend-Q2-multi-MCU-trigger-synchronization-time.patch";
            hash = "sha256-Zngp/pSsvSPCq5eaZbvSE5eUCBlfqNNqag8kZ9UmNa4=";
          })

          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0004-stm32-add-Qidi-Q2-GD32F303-SPI2-mapping.patch";
            hash = "sha256-5bQr4cedNkDQ1+H7lJakrobZ/maL0iSRe40JwGm1Aw0=";
          })

          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0005-stm32-add-Q2-GD32F425-MCU-temperature-support.patch";
            hash = "sha256-l5kBf+za5O0HPmWJrQKCH6HnI7UbSaYCQQirwTmUBV8=";
          })

          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0006-stm32-add-Q2-GD32F303-120MHz-target.patch";
            hash = "sha256-X3kZ4uZRW3dT/L8zil1rEyiRSX9O17Z7DopuxL7xkgw=";
          })

          (pkgs.fetchpatch2 {
            url = "https://github.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/raw/d3f7f86db676e5fa9aad2fec2175927bc06beb2a/patches/klipper/0007-stm32-add-Q2-GD32F425-200MHz-support.patch";
            hash = "sha256-cLSbYokF7Cnw17y0Xa3ScjT9N9hc2bydUnzAUjLcDcM=";
          })
        ];
      }
    );

    katapultScopes = forAllSystems (
      system:
        nixpkgs.legacyPackages.${system}.callPackage ./nix/katapult-scope.nix {
          src = katapult;
        }
    );
  in {
    legacyPackages =
      lib.mapAttrs (
        system: scopes:
          scopes
          // {
            inherit (katapultScopes.${system}) katapult katapult-deployer katapult-source;
          }
      )
      kalicoScopes;

    packages =
      lib.mapAttrs (
        system: scopes:
          (lib.mapAttrs (_: scope: scope.full) scopes)
          // {
            inherit (katapultScopes.${system}) katapult-source;
            katapult = katapultScopes.${system}.katapult.all;
            katapult-deployer = katapultScopes.${system}.katapult-deployer.all;
          }
      )
      kalicoScopes;
  };
}
