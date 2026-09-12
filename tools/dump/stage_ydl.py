#!/usr/bin/env python3
# license:BSD-3-Clause
"""指定した .ydl を純正アップデータ（Upgrade.exe）が読む場所に配置する。

USB 経由の書き込みは約 12 分。MIDI 経由（tools/dump/send_ydl.py）の約 24 分 + 消去 3 分半
より速いので、何度も書き換えるときは USB を使う。Upgrade.exe は
build/upgrade_dumper/images/v200u22k.ydl を読むので、そこへ差し替える。

  python tools/dump/stage_ydl.py build/usbprobe.ydl
  → build/upgrade_dumper/Upgrade.exe を実行する

元の純正 .ydl は roms/updater/x/mu2r1_uw/part2/images/v200u22k.ydl に残っているので、
戻したいときはそれを指定すればよい。
"""
import argparse
import hashlib
import shutil
import sys
from pathlib import Path

DEST = Path("build/upgrade_dumper/images/v200u22k.ydl")
STOCK = Path("roms/updater/x/mu2r1_uw/part2/images/v200u22k.ydl")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ydl", type=Path, nargs="?", help="配置する .ydl")
    ap.add_argument("--stock", action="store_true", help="純正 part2 の .ydl に戻す")
    ap.add_argument("--dest", type=Path, default=DEST)
    a = ap.parse_args()

    src = STOCK if a.stock else a.ydl
    if src is None:
        print("配置する .ydl を指定してください（--stock で純正に戻せます）")
        return 1
    if not src.exists():
        print(f"見つかりません: {src}")
        return 1
    if not a.dest.parent.exists():
        print(f"配置先がありません: {a.dest.parent}")
        return 1

    data = src.read_bytes()
    if len(data) != 4583144:
        print(f"警告: .ydl のサイズが純正と違う（{len(data):,} byte）")
    before = hashlib.sha1(a.dest.read_bytes()).hexdigest()[:12] if a.dest.exists() else "（無し）"
    shutil.copyfile(src, a.dest)
    after = hashlib.sha1(data).hexdigest()[:12]

    print(f"配置: {src}")
    print(f"  -> {a.dest}")
    print(f"  SHA1 {before} -> {after}  ({len(data):,} byte)")
    print("")
    print("次の手順:")
    print("  1. MU2000 の電源を切る")
    print("  2. [Drum] + [PLAY] + [VALUE+] を押しながら電源投入（ダウンロードモード）")
    print("  3. HOST SELECT を USB にして PC と直結（ハブは使わない）")
    print("  4. build/upgrade_dumper/Upgrade.exe を実行する（約 12 分）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
