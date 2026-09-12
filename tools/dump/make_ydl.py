#!/usr/bin/env python3
# license:BSD-3-Clause
"""任意のファームウェアイメージから MU2000 用の .ydl（アップデータのデータファイル）を作る。

純正 `v200u22k.ydl` を解析し、**コマンド列・デルタタイム・メッセージ構造をそのまま流用**して、
中身のデータとチェックサムだけ差し替える。これにより未解析のコマンド（0x00 のパラメータ、0x0E）を
推測せずに済み、ダウンローダから見て純正と同じ手順になる。

検証:
    python tools/dump/make_ydl.py --image roms/mu2000_flash.bin --verify
  純正イメージを入れると出力が公式 .ydl とバイト単位で一致することを確認する。

使い方:
    python tools/dump/make_ydl.py --image build/firmware.bin -o build/dumper.ydl

チェックサム:
  - メッセージ単位: コマンドバイト（0x43 0xnn 0x59 の次）からチェックサム直前までの総和が 128 の倍数になるよう調整
  - セクタ単位 (cmd 03): 書き込んだ範囲のデータバイトの 28bit 単純加算を 4×7bit で送る
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ydl_extract import addr, unpack7

REF_YDL = Path("roms/updater/x/mu2r1_uw/part2/images/v200u22k.ydl")


def pack7(b: bytes) -> bytes:
    """生バイト列 -> 7bit 詰め（7 バイトごとに MSB 集約バイトを末尾に付ける）。"""
    out = bytearray()
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        hi = 0
        for k, v in enumerate(g):
            out.append(v & 0x7F)
            hi |= ((v >> 7) & 1) << (6 - k)
        out.append(hi)
    return bytes(out)


def msg_checksum(body: bytes) -> int:
    """body = コマンドバイト以降チェックサム直前まで。総和が 128 の倍数になる値を返す。"""
    return (-sum(body)) & 0x7F


def septets(v: int, n: int) -> bytes:
    return bytes((v >> (7 * (n - 1 - i))) & 0x7F for i in range(n))


def parse_smf_events(path: Path):
    """(デルタ, SysEx 本体 or None, 生イベントバイト) のリストとヘッダを返す。"""
    d = bytearray(path.read_bytes())
    if d[:4] == b"NZem":
        d[:4] = b"MThd"
    head = bytes(d[:22])
    pos = 22
    end = pos + int.from_bytes(d[18:22], "big")
    evs = []

    def vlq():
        nonlocal pos
        v = 0
        while True:
            b = d[pos]; pos += 1
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v

    while pos < end:
        delta = vlq()
        st = d[pos]
        if st == 0xF0:
            pos += 1
            n = vlq()
            evs.append((delta, bytes(d[pos:pos + n])))
            pos += n
        elif st == 0xFF:
            ty = d[pos + 1]; pos += 2
            n = vlq()
            evs.append((delta, None, ty, bytes(d[pos:pos + n])))
            pos += n
        else:
            raise ValueError(f"想定外のステータス {st:02x} @ {pos}")
    return head, evs


def wvlq(v: int) -> bytes:
    out = bytes([v & 0x7F])
    v >>= 7
    while v:
        out = bytes([(v & 0x7F) | 0x80]) + out
        v >>= 7
    return out


def build(image: bytes, ref: Path) -> bytes:
    head, evs = parse_smf_events(ref)
    base = 0
    track = bytearray()
    for ev in evs:
        delta = ev[0]
        if ev[1] is None:                                   # メタイベント
            _, _, ty, data = ev
            body = bytes([0xFF, ty]) + wvlq(len(data)) + data
        else:
            m = ev[1]
            if m[:4] == b"\x43\x10\x59\x01":                # セクタ選択
                base = addr(m[4:8])
                body = bytes([0xF0]) + wvlq(len(m)) + m
            elif m[:4] == b"\x43\x00\x59\x02":              # 通常の書き込みブロック
                rel = addr(m[4:8])
                nraw = len(m[8:-2]) // 8 * 7
                data = image[base + rel: base + rel + nraw]
                nm = m[:8] + pack7(data)
                nm += bytes([msg_checksum(nm[3:])]) + b"\xf7"
                body = bytes([0xF0]) + wvlq(len(nm)) + nm
            elif m[:4] == b"\x43\x00\x59\x00":              # 端数の書き込みブロック
                n = m[4]
                rel = addr(m[5:8])
                nraw = len(unpack7(m[8:8 + n], False))
                data = image[base + rel: base + rel + nraw]
                nm = m[:8] + pack7(data)
                nm += bytes([msg_checksum(nm[3:])]) + b"\xf7"
                body = bytes([0xF0]) + wvlq(len(nm)) + nm
            elif m[3] == 0x0E and m[1] == 0x10:            # 転送完了 + 全体チェックサム
                # 4MB 全体（ダウンローダ領域を含む）のバイト総和の下位 16bit。
                # 実機の LCD に "DL Check Sum <hex>" として表示される値。
                ck = sum(image) & 0xFFFF
                nm = m[:5] + septets(ck, 6) + bytes([0xF7])
                body = bytes([0xF0]) + wvlq(len(nm)) + nm
            elif m[:4] == b"\x43\x10\x59\x03":              # セクタ完了 + チェックサム
                a = addr(m[4:8]); size = addr(m[8:11])
                ck = sum(image[a:a + size]) & 0x0FFFFFFF
                nm = m[:11] + septets(ck, 4) + b"\xf7"
                body = bytes([0xF0]) + wvlq(len(nm)) + nm
            else:                                            # その他はそのまま
                body = bytes([0xF0]) + wvlq(len(m)) + m
        track += wvlq(delta) + body
    out = bytearray(head)
    out[18:22] = struct.pack(">I", len(track))
    out += track
    out[:4] = b"NZem"
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, required=True, help="4MB のフラッシュイメージ")
    ap.add_argument("--ref", type=Path, default=REF_YDL, help="雛形にする純正 .ydl")
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--verify", action="store_true", help="出力が雛形とバイト単位で一致するか確認する")
    a = ap.parse_args()

    img = a.image.read_bytes()
    if len(img) != 0x400000:
        print(f"警告: イメージが 4MB ではない ({len(img):,} byte)")
    out = build(img, a.ref)

    if a.verify:
        ref = a.ref.read_bytes()
        if out == ref:
            print(f"一致: 生成結果は {a.ref.name} とバイト単位で同一（{len(out):,} byte）")
            return 0
        print(f"不一致: 生成 {len(out):,} byte / 雛形 {len(ref):,} byte")
        for i in range(min(len(out), len(ref))):
            if out[i] != ref[i]:
                print(f"  最初の差異 @ {i}: 生成 {out[i]:02x} / 雛形 {ref[i]:02x}")
                print(f"  生成: {out[max(0,i-8):i+16].hex(' ')}")
                print(f"  雛形: {ref[max(0,i-8):i+16].hex(' ')}")
                break
        return 1

    if not a.out:
        print("出力先を -o で指定してください")
        return 1
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(out)
    print(f"作成: {a.out} ({len(out):,} byte)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
