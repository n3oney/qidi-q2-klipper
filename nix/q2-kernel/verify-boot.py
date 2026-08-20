#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) // boundary * boundary


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--max-size", type=int, required=True)
    parser.add_argument("--resource", type=Path, required=True)
    args = parser.parse_args()

    image = args.image.read_bytes()
    if len(image) > args.max_size:
        raise SystemExit(
            f"boot image is {len(image)} bytes; partition limit is {args.max_size}"
        )
    if image[:8] != b"ANDROID!":
        raise SystemExit("missing Android boot image magic")

    (
        kernel_size,
        kernel_addr,
        ramdisk_size,
        ramdisk_addr,
        second_size,
        second_addr,
        tags_addr,
        page_size,
        header_version,
        _os_version,
    ) = struct.unpack_from("<10I", image, 8)

    expected = {
        "kernel_addr": (kernel_addr, 0x10008000),
        "ramdisk_size": (ramdisk_size, 0),
        "ramdisk_addr": (ramdisk_addr, 0),
        "second_addr": (second_addr, 0x10F00000),
        "tags_addr": (tags_addr, 0x10000100),
        "page_size": (page_size, 2048),
        "header_version": (header_version, 0),
    }
    mismatches = [
        f"{name}=0x{actual:x}, expected 0x{wanted:x}"
        for name, (actual, wanted) in expected.items()
        if actual != wanted
    ]
    if mismatches:
        raise SystemExit("unexpected boot header: " + "; ".join(mismatches))

    kernel_offset = page_size
    second_offset = kernel_offset + align(kernel_size, page_size)
    image_end = second_offset + align(second_size, page_size)
    if image_end > len(image):
        raise SystemExit("boot component sizes extend beyond the image")
    if image[kernel_offset : kernel_offset + 4] != b"\x04\x22\x4d\x18":
        raise SystemExit("kernel component is not an LZ4 frame")
    if image[second_offset : second_offset + 4] != b"RSCE":
        raise SystemExit("second component is not a Rockchip resource image")

    expected_resource = args.resource.read_bytes()
    resource = image[second_offset : second_offset + second_size]
    if resource != expected_resource:
        raise SystemExit(
            "second component does not match the packaged Rockchip resource image"
        )

    print(
        f"verified {args.image}: {len(image)} bytes, "
        f"kernel={kernel_size}, resource={second_size}"
    )


if __name__ == "__main__":
    main()
