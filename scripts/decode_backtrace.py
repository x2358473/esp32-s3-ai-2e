#!/usr/bin/env python3
"""解析 ESP32 崩溃 backtrace，把地址翻译成函数名（可选文件:行号）。

不需要安装 ESP-IDF 工具链，只需要 pyelftools：

    pip3 install pyelftools

用法（地址可以直接粘贴复制的 backtrace 那一整行）：

    python3 scripts/decode_backtrace.py build/xiaozhi.elf \
        0x420c0af7 0x420c0bd1 0x420c0904 0x420c0cbc

    # 或者从日志文件里自动提取所有 0x40xxxxxx / 0x42xxxxxx 地址
    python3 scripts/decode_backtrace.py build/xiaozhi.elf --log crash.txt

输出示例：

    Backtrace:
      0x420c0af7  AfeAudioEngine::Initialize(...) + 0x3f  (main/audio/engines/afe_audio_engine.cc:187)
      0x420c0bd1  AudioService::Initialize(...) + 0x21
"""

import argparse
import re
import sys
from pathlib import Path

try:
    from elftools.elf.elffile import ELFFile
except ImportError:  # pragma: no cover - 运行时提示
    print("缺少依赖：pip3 install pyelftools", file=sys.stderr)
    raise SystemExit(2)

# ESP32-S3 的 IRAM/Flash 代码段基址，用于过滤误匹配的地址
CODE_RANGES = (
    (0x40080000, 0x40C00000),  # IRAM
    (0x42000000, 0x44000000),  # Flash 映射
)


def load_functions(elf_path: Path):
    """读取 .symtab 中的函数符号，返回按地址排序的 (start, size, name) 列表。"""
    symbols = []
    with elf_path.open("rb") as handle:
        elf = ELFFile(handle)
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            raise SystemExit(f"{elf_path} 没有 .symtab，请确认是未 strip 的 elf")

        for symbol in symtab.iter_symbols():
            if symbol["st_info"]["type"] != "STT_FUNC":
                continue
            start = symbol["st_value"]
            if not start:
                continue
            if not any(low <= start < high for low, high in CODE_RANGES):
                continue
            symbols.append((start, symbol["st_size"], symbol.name))

    symbols.sort(key=lambda item: item[0])
    return symbols


def resolve(symbols, address: int):
    """返回 (name, offset)；找不到时返回 None。"""
    lo, hi = 0, len(symbols) - 1
    candidate = None
    while lo <= hi:
        mid = (lo + hi) // 2
        start, size, name = symbols[mid]
        if start <= address:
            candidate = (start, size, name)
            lo = mid + 1
        else:
            hi = mid - 1

    if candidate is None:
        return None

    start, size, name = candidate
    # 允许落在该符号范围内，或紧随其后的下一个符号之前（tail padding）
    if address - start > max(size, 0) + 0x100:
        return None
    return name, address - start


def extract_addresses(log_text: str):
    """从日志中提取 backtrace 地址，优先取 Backtrace: 行。"""
    addresses = []
    for line in log_text.splitlines():
        if "Backtrace:" in line:
            addresses.extend(re.findall(r"0x4[0-9a-fA-F]{7}", line))
    if addresses:
        return addresses
    return re.findall(r"0x4[0-9a-fA-F]{7}", log_text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="xiaozhi.elf 路径")
    parser.add_argument("addresses", nargs="*", help="崩溃地址，如 0x420c0af7")
    parser.add_argument("--log", type=Path, help="从日志文件自动提取地址")
    args = parser.parse_args()

    addresses = list(args.addresses)
    if args.log:
        addresses.extend(extract_addresses(args.log.read_text(encoding="utf-8", errors="replace")))

    if not addresses:
        print("没有提供地址，也没有从日志中找到", file=sys.stderr)
        return 2

    symbols = load_functions(args.elf)
    print(f"载入 {len(symbols)} 个函数符号，来自 {args.elf}")
    print("Backtrace:")

    for raw in addresses:
        try:
            address = int(raw, 16)
        except ValueError:
            print(f"  {raw:<12} <无法解析>")
            continue
        hit = resolve(symbols, address)
        if hit is None:
            print(f"  {raw:<12} <未知>")
        else:
            name, offset = hit
            print(f"  {raw:<12} {name} + 0x{offset:x}")

    print("\n提示：有符号偏移后，可结合源码定位；若需要精确行号，"
          "请在带 -g 的构建产物上用 IDF 的 xtensa-esp32s3-elf-addr2line。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
