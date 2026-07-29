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
            url = "https://raw.githubusercontent.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/f9f6cf04e5e4b9442c23aefeff6061a8bedcbf29/patches/klipper/0001-q2-mainboard-usb-and-cs1237.patch";
            hash = "sha256-UNbFaSd+0mcBe9NDS6CJkbWq1Oc9rS9HhTwfVGvcRNY=";
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
