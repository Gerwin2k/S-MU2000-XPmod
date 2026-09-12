#!/usr/bin/env python3
# license:BSD-3-Clause
"""MU2000 ROM ダンプを MAME の定義（ymmu2000.cpp）と照合し、MAME 用 romset を作る。

使い方:
    python tools/dump/verify_roms.py <ダンプを置いたディレクトリ> [--out roms/mu2000.zip]

ディレクトリ内の全ファイルについて、そのまま / バイトスワップ の 2 通りで CRC32 と SHA1 を計算し、
MAME に登録されている 10 個の ROM のどれかに一致すれば MAME 用ファイル名で報告する。
ファイル名は自由（ic25.bin など）。同一チップを 2 回読んだ場合は両方置いておけば重複として検出される。
"""
import argparse
import hashlib
import sys
import zipfile
import zlib
from pathlib import Path

# (MAME ファイル名, サイズ, CRC32, SHA1, 説明)
KNOWN = [
    ("xw87020.ic25",       0x200000, 0x79f6c158, "2213b9c661c6b1a79963321c37aff40be7cc1fff", "プログラム v1.01 上位 (IC25)"),
    ("xw86920.ic24",       0x200000, 0xafbac33c, "594b4c64aecf6a5204b058375e39e44b8fe373be", "プログラム v1.01 下位 (IC24)"),
    ("mu2000-v2.01-h.bin", 0x200000, 0xbe3668f6, "00d008b6a2536a71681ce2f4fd1a5853406f82f2", "プログラム v2.01 (EX) 上位 (IC25)"),
    ("mu2000-v2.01-l.bin", 0x200000, 0x55921a50, "fd8fe6a5cbba028d847453c004cb2dcf9ba02013", "プログラム v2.01 (EX) 下位 (IC24)"),
    ("xv364a0.ic49",       0x800000, 0xcda1afd6, "e7098246b33c3cf22ed8cc15ed6383f8a06d17e9", "波形 ROM 1 下位 (IC49)"),
    ("xv365a0.ic50",       0x800000, 0x10985ed0, "d45a2e85859e05046f3ede8317a9bb0b88898116", "波形 ROM 1 上位 (IC50)"),
    ("xw848a0.ic53",       0x800000, 0x34913e42, "9e8b55c2cbac3f69cc0b17aeaf02053145bfaeda", "波形 ROM 2 下位 (IC53)"),
    ("xw849a0.ic54",       0x800000, 0x3728f1f2, "7670d672e24d6388fa92799175f35869a140c451", "波形 ROM 2 上位 (IC54)"),
    # 参考: MU1000 のプログラム（MU2000 では不要）
    ("mu1000-v2.01-h.bin", 0x100000, 0xd0809297, "cee47062966b01ce72e8ebaf6f7fa9778b32f6ab", "MU1000 プログラム v2.01 上位"),
    ("mu1000-v2.01-l.bin", 0x100000, 0x048a2750, "19f51c6304e3550d0bb8b3cca647f1fc609b0994", "MU1000 プログラム v2.01 下位"),
]
BY_SHA1 = {k[3]: k for k in KNOWN}
WAVE = {"xv364a0.ic49", "xv365a0.ic50", "xw848a0.ic53", "xw849a0.ic54"}
PROG_SETS = [{"xw87020.ic25", "xw86920.ic24"}, {"mu2000-v2.01-h.bin", "mu2000-v2.01-l.bin"}]


def byteswap(data: bytes) -> bytes:
    if len(data) % 2:
        data += b"\x00"
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


def digest(data: bytes):
    return zlib.crc32(data) & 0xFFFFFFFF, hashlib.sha1(data).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump_dir", type=Path)
    ap.add_argument("--out", type=Path, default=Path("roms/mu2000.zip"))
    ap.add_argument("--dummy-wave", action="store_true",
                    help="波形 ROM が無いとき 0x00 で埋めたダミーを入れて zip を作る（MAME は警告付きで起動、無音）")
    a = ap.parse_args()

    files = sorted(p for p in a.dump_dir.iterdir() if p.is_file() and p.suffix.lower() != ".zip")
    if not files:
        print(f"ファイルがありません: {a.dump_dir}")
        return 1

    matched: dict[str, tuple[Path, bool]] = {}  # mame name -> (path, swapped)
    for p in files:
        data = p.read_bytes()
        crc, sha = digest(data)
        hit, swapped = BY_SHA1.get(sha), False
        if hit is None:
            crc2, sha2 = digest(byteswap(data))
            hit, swapped = BY_SHA1.get(sha2), True
        size_note = f"{len(data):>10,} byte"
        if hit:
            tag = "一致（バイトスワップ済で一致）" if swapped else "一致"
            print(f"[OK ] {p.name:<28} {size_note}  -> {hit[0]:<20} {hit[4]}  {tag}")
            if hit[0] in matched:
                print(f"      重複: {matched[hit[0]][0].name} と同一内容（2 回読みの一致確認 OK）")
            else:
                matched[hit[0]] = (p, swapped)
        else:
            sizes = sorted({k[1] for k in KNOWN})
            hint = "" if len(data) in sizes else f"  サイズが想定外（想定: {', '.join(f'{s:,}' for s in sizes)}）"
            print(f"[NG ] {p.name:<28} {size_note}  crc32={crc:08x} sha1={sha}{hint}")

    have_wave = WAVE <= matched.keys()
    prog = next((s for s in PROG_SETS if s <= matched.keys()), None)
    print()
    print(f"波形 ROM      : {len(WAVE & matched.keys())}/4")
    print(f"プログラム ROM: {'v2.01 (EX)' if prog and 'mu2000-v2.01-h.bin' in prog else 'v1.01' if prog else '未揃い'}")

    dummy = set()
    if not have_wave and prog and a.dummy_wave:
        dummy = WAVE - matched.keys()
        print(f"ダミー（0x00 埋め）で代用: {', '.join(sorted(dummy))}")
    elif not (have_wave and prog):
        missing = (WAVE - matched.keys()) | (set() if prog else {"IC24/IC25 のペア"})
        print(f"不足: {', '.join(sorted(missing))}")
        return 2

    a.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(WAVE | prog):
            if name in dummy:
                z.writestr(name, bytes(0x800000))
                continue
            path, swapped = matched[name]
            data = path.read_bytes()
            z.writestr(name, byteswap(data) if swapped else data)
    print(f"作成: {a.out}  ->  build/render.exe などで使えます"
          + ("（波形はダミーなので無音。MAME の ROM 警告画面は Enter で進む）" if dummy else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
