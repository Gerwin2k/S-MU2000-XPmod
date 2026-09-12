#!/usr/bin/env python3
# license:BSD-3-Clause
"""MAME 検証用: 自作ダンパを組み込んだ mu2000 romset を作る。

- プログラム ROM: 完成済みファームウェアイメージ（--firmware）をそのまま使うか、
  roms/mu2000_flash.bin の 0x040100 に生の SH-2 コード（--code）を上書きしたもの
- 波形 ROM: 既知パターン wave_word[i] = i | 0xE0000000 のダミー
  （MAME は ROM_REGION32_LE に ic49=下位16bit / ic50=上位16bit を 4 バイト毎にインターリーブする）

32MB のダミー波形を作るのは遅いので、既存の romset があれば --reuse でその波形を使い回す。

出力: <--out>/mu2000.zip （MAME の -rompath に指定する）
"""
import argparse
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTRY = 0x00040100
WAVE_NAMES = ("xv364a0.ic49", "xv365a0.ic50", "xw848a0.ic53", "xw849a0.ic54")


def split_hl(img: bytes):
    """4MB リニアイメージ -> MAME の h/l 2 ファイル（16bit ワード内バイトスワップ）。"""
    h = bytearray(); l = bytearray()
    for i in range(0, len(img), 4):
        h += img[i:i + 2][::-1]
        l += img[i + 2:i + 4][::-1]
    return bytes(h), bytes(l)


def pattern_wave():
    """wave_word[i] = i | 0xE0000000 を満たす ic49/ic50/ic53/ic54 を作る。"""
    files = {}
    for half, (lo_name, hi_name) in enumerate(
            [("xv364a0.ic49", "xv365a0.ic50"), ("xw848a0.ic53", "xw849a0.ic54")]):
        lo = bytearray(); hi = bytearray()
        base = half * 0x400000
        for i in range(base, base + 0x400000):
            v = i | 0xE0000000
            lo += struct.pack("<H", v & 0xFFFF)
            hi += struct.pack("<H", (v >> 16) & 0xFFFF)
        files[lo_name] = bytes(lo); files[hi_name] = bytes(hi)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--firmware", type=Path, help="4MB のファームウェアイメージをそのまま使う")
    ap.add_argument("--code", type=Path, default=Path("build/dumper.bin"),
                    help="生の SH-2 コードを 0x040100 に置く（--firmware 指定時は無視）")
    ap.add_argument("--base", type=Path, default=Path("roms/mu2000_flash.bin"))
    ap.add_argument("--out", type=Path, default=Path("build/mu2000_test"))
    ap.add_argument("--reuse", type=Path,
                    help="この romset（mu2000.zip）の波形 ROM を使い回して生成を省く")
    a = ap.parse_args()

    if a.firmware:
        img = bytearray(a.firmware.read_bytes())
        what = f"ファームウェア {a.firmware}"
    else:
        img = bytearray(a.base.read_bytes())
        code = a.code.read_bytes()
        img[ENTRY:ENTRY + len(code)] = code
        what = f"コード {len(code)} byte を {ENTRY:#08x} に配置"
    if len(img) != 0x400000:
        print(f"警告: プログラム ROM が 4MB ではない ({len(img):,} byte)")
    h, l = split_hl(bytes(img))

    a.out.mkdir(parents=True, exist_ok=True)
    if a.reuse and a.reuse.exists():
        with zipfile.ZipFile(a.reuse) as z:
            waves = {n: z.read(n) for n in WAVE_NAMES}
        print(f"波形 ROM を {a.reuse} から流用")
    else:
        print("ダミー波形 ROM を生成中（32MB）...")
        waves = pattern_wave()

    with zipfile.ZipFile(a.out / "mu2000.zip", "w", zipfile.ZIP_STORED) as z:
        z.writestr("mu2000-v2.01-h.bin", h)
        z.writestr("mu2000-v2.01-l.bin", l)
        for name, data in waves.items():
            z.writestr(name, data)
    for src in ("roms/mulcd.zip", "roms/swp30.zip"):
        (a.out / Path(src).name).write_bytes((ROOT / src).read_bytes())
    print(f"作成: {a.out/'mu2000.zip'}  （{what}）")


if __name__ == "__main__":
    sys.exit(main())
