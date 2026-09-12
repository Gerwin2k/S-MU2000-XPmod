#!/usr/bin/env python3
# license:BSD-3-Clause
"""Yamaha アップデータの .ydl（先頭 4 バイトを 'NZem' に変えた Standard MIDI File）から
MU2000 のプログラム Flash イメージ（4MB）を復元する。

使い方:
    python tools/dump/ydl_extract.py part1/images/v200U12k.ydl part2/images/v200u22k.ydl -o roms/mu2000_flash.bin

SysEx 形式（解析結果）:
  F0 43 10 59 01 a3 a2 a1 a0 F7                  セクタ選択/消去（a = 7bit×4 の絶対アドレス）
  F0 43 00 59 02 a3 a2 a1 a0 <256B> ck F7        224 バイト書き込み（a = セクタ内相対アドレス）
     7bit 詰め: データ 7 バイト + MSB 集約 1 バイト（末尾）で 8 バイト、bit6 が先頭バイトの MSB
  未書き込み領域（0x00C000-0x03FFFF, 0x3E0000-0x3FFFFF）は 0xFF。この状態で MAME の mu2000 v2.01 セットと SHA1 が一致する。
  F0 43 00 59 00 n  a2 a1 a0 <n B>   ck F7       端数書き込み
  F0 43 10 59 03 a3 a2 a1 a0 s2 s1 s0 05 c c c F7 セクタ書き込み完了（s = セクタサイズ, c = チェックサム）
  F0 43 10 59 04 s2 s1 s0 04 c c c F7            part1 完了
"""
import argparse
import hashlib
import sys
from pathlib import Path


def parse_smf(path: Path):
    d = bytearray(path.read_bytes())
    if d[:4] == b"NZem":
        d[:4] = b"MThd"
    assert d[:4] == b"MThd" and d[14:18] == b"MTrk", "SMF ではない"
    pos = 22
    end = pos + int.from_bytes(d[18:22], "big")
    out = []

    def vlq():
        nonlocal pos
        v = 0
        while True:
            b = d[pos]
            pos += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v

    while pos < end:
        vlq()
        st = d[pos]
        pos += 1
        if st == 0xF0:
            n = vlq()
            out.append(bytes(d[pos:pos + n]))
            pos += n
        elif st == 0xFF:
            pos += 1
            n = vlq()
            pos += n
        else:
            raise ValueError(f"unexpected status {st:02x} @ {pos}")
    return out


def addr(b: bytes) -> int:
    v = 0
    for x in b:
        v = (v << 7) | x
    return v


def unpack7(p: bytes, msb_first: bool) -> bytes:
    """8 バイト（MSB 集約 1 + 7 データ）→ 7 バイト。端数グループにも対応。"""
    out = bytearray()
    for i in range(0, len(p), 8):
        g = p[i:i + 8]
        if msb_first:
            hi, data = g[0], g[1:]
            out += bytes(((hi >> (6 - k)) & 1) << 7 | data[k] for k in range(len(data)))
        else:
            hi, data = g[-1], g[:-1]
            out += bytes(((hi >> (6 - k)) & 1) << 7 | data[k] for k in range(len(data)))
    return bytes(out)


def checksum_ok(msg: bytes) -> bool:
    # msg は 43 .. ck F7 ; 43 の次（デバイス番号）から ck までの和が 0 mod 128 (Yamaha 方式) を試す
    body = msg[1:-1]
    return sum(body) % 128 == 0 or sum(msg[3:-1]) % 128 == 0


def build(files, msb_first=True, size=0x400000, fill=0xFF):
    img = bytearray([fill]) * size
    written = bytearray(size)
    base = 0
    log = []
    bad_ck = 0
    for f in files:
        for m in parse_smf(f):
            if m[:3] not in (b"\x43\x10\x59", b"\x43\x00\x59"):
                continue
            cmd = m[3]
            if m[1] == 0x10 and cmd == 0x01:
                base = addr(m[4:8])
                log.append(f"  sector @ {base:07x}")
            elif m[1] == 0x00 and cmd == 0x02:
                a = base + addr(m[4:8])
                data = unpack7(m[8:-2], msb_first)
                img[a:a + len(data)] = data
                written[a:a + len(data)] = b"\x01" * len(data)
                bad_ck += not checksum_ok(m)
            elif m[1] == 0x00 and cmd == 0x00:
                n = m[4]
                a = base + addr(m[5:8])
                data = unpack7(m[8:8 + n], msb_first)
                img[a:a + len(data)] = data
                written[a:a + len(data)] = b"\x01" * len(data)
                bad_ck += not checksum_ok(m)
            elif m[1] == 0x10 and cmd == 0x03:
                log.append(f"  sector done @ {addr(m[4:8]):07x} size {addr(m[8:11]):06x} ck {m[12:15].hex()}")
            else:
                log.append(f"  ctrl: {m.hex(' ')}")
    return bytes(img), bytes(written), log, bad_ck


def ranges(written: bytes):
    out, start = [], None
    for i, w in enumerate(written + b"\x00"):
        if w and start is None:
            start = i
        elif not w and start is not None:
            out.append((start, i))
            start = None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ydl", nargs="+", type=Path)
    ap.add_argument("-o", "--out", type=Path, help="4MB 連続イメージの出力先")
    ap.add_argument("--mame-dir", type=Path, help="MAME 用 mu2000-v2.01-h.bin / -l.bin を書き出すディレクトリ")
    ap.add_argument("--msb-first", action="store_true", help="7bit 詰めの MSB バイトをグループ先頭とみなす（既定は末尾。末尾で MAME と一致することを確認済み）")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    img, written, log, bad_ck = build(a.ydl, msb_first=a.msb_first)
    if a.verbose:
        print("\n".join(log))
    print(f"チェックサム不一致ブロック: {bad_ck}")
    print("書き込み範囲:")
    for s, e in ranges(written):
        print(f"  {s:07x}-{e - 1:07x}  ({e - s:,} byte)")
    print(f"未書き込み: {written.count(0):,} byte（0xFF で埋め）")
    pc, sp = int.from_bytes(img[0:4], "big"), int.from_bytes(img[4:8], "big")
    print(f"SH-2 リセットベクタ: PC={pc:08x} SP={sp:08x}")
    for key in (b"MU2000", b"Extended", b"Revision", b"YAMAHA", b"XG"):
        i = img.find(key)
        if i >= 0:
            print(f"  文字列 {key.decode()!r} @ {i:07x}: {img[i:i + 40]!r}")
    print(f"SHA1(4MB 全体) = {hashlib.sha1(img).hexdigest()}")
    # MAME 形式（16bit ワードごとに h/l へ分割）を両バイト順で出す
    h = bytearray(); l = bytearray()
    for i in range(0, len(img), 4):
        h += img[i:i + 2][::-1]; l += img[i + 2:i + 4][::-1]
    hs, ls = hashlib.sha1(h).hexdigest(), hashlib.sha1(l).hexdigest()
    ok = (hs, ls) == ("00d008b6a2536a71681ce2f4fd1a5853406f82f2", "fd8fe6a5cbba028d847453c004cb2dcf9ba02013")
    print(f"MAME 形式: h={hs}  l={ls}  -> {'MAME の mu2000 v2.01 定義と一致' if ok else '不一致'}")
    if a.mame_dir:
        a.mame_dir.mkdir(parents=True, exist_ok=True)
        (a.mame_dir / "mu2000-v2.01-h.bin").write_bytes(h)
        (a.mame_dir / "mu2000-v2.01-l.bin").write_bytes(l)
        print(f"書き出し: {a.mame_dir}/mu2000-v2.01-h.bin, -l.bin")
    if a.out:
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_bytes(img)
        print(f"書き出し: {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
