{
  src,
  applyPatches,
}:
applyPatches {
  inherit src;
  name = "happy-hare";
  patches = [
    ./happy-hare-kalico-extruder.patch
    ./happy-hare-fm17550.patch
  ];
}
