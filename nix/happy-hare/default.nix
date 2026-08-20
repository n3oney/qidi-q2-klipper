{
  src,
  applyPatches,
}:
applyPatches {
  inherit src;
  name = "happy-hare";
  patches = [./happy-hare-kalico-extruder.patch];
}
