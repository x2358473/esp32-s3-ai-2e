#!/usr/bin/env python3
"""把 assets 分区镜像覆盖到合并固件的指定偏移，产出可直接从 0x0 烧录的单文件镜像。

背景：
    idf.py merge-bin 产出的 merged-binary.bin 长度可能已经超过 assets 分区起始地址
    （本仓库 16MB 分区表下约 9.5MB > 0x800000），因此不能用 esptool merge-bin 再合并
    assets（会报 "Detected overlap"），只能按偏移原地覆盖。

用法：
    python scripts/make_full_image.py build/merged-binary.bin \
        build/generated_assets.bin out_full.bin [--offset 0x800000]
"""

import argparse
from pathlib import Path

DEFAULT_ASSETS_OFFSET = 0x800000


def build_full_image(app_path: Path, assets_path: Path, offset: int) -> bytes:
    image = bytearray(app_path.read_bytes())
    assets = assets_path.read_bytes()

    # merged-binary 在 assets 分区起点之后可能还带填充/残留数据，
    # 该区间在分区表里只属于 assets，先整体清成 0xFF 再写入，避免旧文件系统残留。
    if len(image) > offset:
        image[offset:] = b"\xff" * (len(image) - offset)

    need = offset + len(assets)
    if len(image) < need:
        image.extend(b"\xff" * (need - len(image)))

    image[offset:need] = assets
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path, help="merged-binary.bin 路径")
    parser.add_argument("assets", type=Path, help="assets 分区镜像路径")
    parser.add_argument("output", type=Path, help="输出的完整镜像路径")
    parser.add_argument(
        "--offset",
        type=lambda value: int(value, 0),
        default=DEFAULT_ASSETS_OFFSET,
        help="assets 分区起始地址，默认 0x800000",
    )
    args = parser.parse_args()

    image = build_full_image(args.app, args.assets, args.offset)
    args.output.write_bytes(image)
    size_mb = len(image) / 1024 / 1024
    assets_kb = args.assets.stat().st_size / 1024
    print(
        f"full image: {args.output} ({size_mb:.2f} MB), "
        f"assets {assets_kb:.1f} KB @0x{args.offset:x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
