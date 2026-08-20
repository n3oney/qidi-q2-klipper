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
          };

        packages =
          (lib.mapAttrs (_: scope: scope.full) klipperScopes)
          // {
            inherit (katapultScope) katapult-source;
            katapult = katapultScope.katapult.all;
            katapult-deployer = katapultScope.katapult-deployer.all;
          };
      }
    );
  in {
    legacyPackages = lib.mapAttrs (_: outputs: outputs.legacyPackages) perSystem;
    packages = lib.mapAttrs (_: outputs: outputs.packages) perSystem;
  };
}
