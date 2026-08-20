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

    rockchip-kernel = {
      url = "github:rockchip-linux/kernel/develop-6.12";
      flake = false;
    };
    rockchip-uboot = {
      url = "github:rockchip-linux/u-boot/next-dev";
      flake = false;
    };
    rtl8189fs = {
      url = "github:jwrdegoede/rtl8189ES_linux/rtl8189fs";
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
    rockchip-kernel,
    rockchip-uboot,
    rtl8189fs,
    ...
  }: let
    lib = nixpkgs.lib;
    forAllSystems = lib.genAttrs lib.systems.flakeExposed;

    perSystem = forAllSystems (
      system: let
        pkgs = nixpkgs.legacyPackages.${system};

        klipperScopes = import ./nix/klipper {
          inherit pkgs klipper;
          kalicoMain = kalico-main;
          kalicoBleedingEdge = kalico-bleeding-edge;
        };

        katapultScope = import ./nix/katapult {
          inherit pkgs;
          src = katapult;
        };

        q2Kernel = pkgs.callPackage ./nix/q2-kernel {
          kernelSrc = rockchip-kernel;
          ubootSrc = rockchip-uboot;
          wifiSrc = rtl8189fs;
        };
      in {
        legacyPackages =
          klipperScopes
          // {
            inherit
              (katapultScope)
              katapult
              katapult-deployer
              katapult-source
              ;
            q2-kernel = q2Kernel;
          };

        packages =
          (lib.mapAttrs (_: scope: scope.full) klipperScopes)
          // {
            kalico-main-source = klipperScopes.kalico-main.kalico;
            kalico-bleeding-edge-source = klipperScopes.kalico-bleeding-edge.kalico;
            klipper-source = klipperScopes.klipper.kalico;
            inherit (katapultScope) katapult-source;
            katapult = katapultScope.katapult.all;
            katapult-deployer = katapultScope.katapult-deployer.all;
            q2-kernel = q2Kernel;
          };
      }
    );
  in {
    legacyPackages = lib.mapAttrs (_: outputs: outputs.legacyPackages) perSystem;
    packages = lib.mapAttrs (_: outputs: outputs.packages) perSystem;
  };
}
