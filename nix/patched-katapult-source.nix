{
  applyPatches,
  fetchpatch2,
  src,
}:
applyPatches {
  name = "katapult-qidi";
  inherit src;

  patches = [
    (fetchpatch2 {
      url = "https://raw.githubusercontent.com/MisterSheikh/Qidi_Q2_Mainline_Klipper/f9f6cf04e5e4b9442c23aefeff6061a8bedcbf29/patches/katapult/0001-q2-mainboard-usb.patch";
      hash = "sha256-aSBnGJ4EU3rK3y4aCD1lk/w6z8Y4/9LAWThGHg/sHyM=";
    })
  ];
}
