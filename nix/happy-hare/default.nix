{
  src,
  applyPatches,
}:
applyPatches {
  inherit src;
  name = "happy-hare";
  patches = [
    ./happy-hare-kalico-extruder.patch
    ./happy-hare-rc522-tag-state.patch
    ./happy-hare-nfc-mcu-stop.patch
  ];
}
