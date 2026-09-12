#!/usr/bin/env python3
# license:BSD-3-Clause
"""MU2000 波形 ROM ダンパの受信スクリプト。

MIDI IN からダンパ（tools/dump/make_dumper.py）の SysEx を受け取り、波形 ROM を復元する。
各ブロックには加算和が付いているので、化けたブロックは捨てて取り直す。
途中経過は 30 秒ごとに .part へ保存される。Ctrl+C で中断でき、再実行すると続きから集める。

  python tools/dump/recv_dump.py --list
  python tools/dump/recv_dump.py --port "Babyface" --out roms/dump                 # 受信のみ
  python tools/dump/recv_dump.py --port "Babyface" --req-port "Babyface" --out roms/dump  # 双方向

USB 版ダンパ（tools/dump/make_usbdump.py）はこう指定する。**--prime-port が必須**で、
これが無いと 1 ブロックも来ない（ファームは受動的で、引き金を送った分だけ返す）。
--words-per-block はファームを組んだときの --words と揃えること（既定の usbdump64 は 64）。
再送要求は効かない（ブロック番号を指定する口が無い）ので --req-port は付けない。
穴はダンパが一巡したときに埋まる。

  python tools/dump/recv_dump.py --port "Yamaha MU2000-1" --prime-port "Yamaha MU2000-1" --words-per-block 64 --out roms/dump

--req-port を付けると PC 側から再送要求を出せる（PC MIDI OUT -> MU2000 MIDI IN A または B）。
穴が残っている状態で既取得のブロックが続いたら、次に欠けているブロックへ飛ばす。
これがないとダンパの一巡（約 3.8 時間）を待つことになる。

受信 SysEx: F0 43 7D 'M' 'U' 'W' 'D' <blk 4x7bit> <128 word x 5x7bit> <加算和 2x7bit> F7
送出 SysEx: F0 43 7D <blk 4x7bit> F7

Windows の MIDI ドライバは長い SysEx を 1024 byte 単位に分割して渡すので、F0..F7 を跨いで再結合する。
"""
import argparse
import collections
import struct
import sys
import time
from pathlib import Path

import rtmidi

HEADER = bytes([0xF0, 0x43, 0x7D, 0x4D, 0x55, 0x57, 0x44])
# 受信割り込み(IRQ3)を起こすためだけに投げるもの。ノートオフなので音は出ない。
# リアルタイムバイト(0xFE)は USB-MIDI のどこかで落とされることがあるので頼らない。
TRIGGER = [0x80, 60, 0]   # Note Off ch1。無音。1 個につき最大 1 ブロック返る

WORDS_PER_BLOCK = 128        # MIDI 版ダンパの既定。USB 版は 16（--words-per-block）
ROM_WORDS = 0x800000

REQ_MIN_INTERVAL = 0.4   # 再送要求を出す最短間隔（秒）
KNOWN_RUN_TRIGGER = 3    # 既に持っているブロックがこの数だけ続いたら飛ばす
IDLE_TRIGGER = 1.5       # この秒数だけ無音なら飛ばす


def parse(msg: bytes):
    """SysEx 1 個 -> (ブロック番号, 32bit ワードのリスト)。

    形が違えば None、加算和が合わなければ ("bad", ブロック番号) を返す。
    """
    if not msg.startswith(HEADER) or msg[-1] != 0xF7:
        return None
    body = msg[len(HEADER):-1]
    if len(body) < 6 or (len(body) - 6) % 5:
        return None
    blk = (body[0] << 21) | (body[1] << 14) | (body[2] << 7) | body[3]
    want = (body[-2] << 7) | body[-1]
    if sum(body[:-2]) & 0x3FFF != want:
        return ("bad", blk)
    payload = body[4:-2]
    words = []
    for i in range(0, len(payload), 5):
        a, b, c, d, e = payload[i:i + 5]
        words.append((a << 28) | (b << 21) | (c << 14) | (d << 7) | e)
    return blk, words


def request(midiout, blk: int):
    """再送要求 SysEx を送る。"""
    midiout.send_message([0xF0, 0x43, 0x7D,
                          (blk >> 21) & 0x7F, (blk >> 14) & 0x7F,
                          (blk >> 7) & 0x7F, blk & 0x7F, 0xF7])


def open_port(cls, name, kind):
    dev = cls()
    ports = dev.get_ports()
    idx = next((i for i, p in enumerate(ports) if name.lower() in p.lower()), None)
    if idx is None:
        print(f"{kind}ポートが見つかりません: {name}")
        print(f"  候補: {ports}")
        return None, None
    dev.open_port(idx)
    return dev, ports[idx]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="MIDI ポート一覧")
    ap.add_argument("--port", help="MIDI 入力ポート名（部分一致）")
    ap.add_argument("--req-port", help="再送要求を出す MIDI 出力ポート名（部分一致）")
    ap.add_argument("--prime-port",
                    help="USB 版用。**必須**。引き金を送り続けないと 1 ブロックも来ない"
                         "（例: \"Yamaha MU2000-1\"）")
    ap.add_argument("--window", type=int, default=8,
                    help="返事待ちの引き金をこの数までに抑える。増やすと詰まりやすくなる")
    ap.add_argument("--credit-ttl", type=float, default=0.3,
                    help="返事の来ない引き金を、この秒数で失効させる")
    ap.add_argument("--out", type=Path, default=Path("roms/dump"))
    ap.add_argument("--words", type=lambda s: int(s, 0), default=ROM_WORDS)
    ap.add_argument("--words-per-block", type=int, default=WORDS_PER_BLOCK,
                    help="1 ブロックのワード数。MIDI 版 128、USB 版 16")
    ap.add_argument("--timeout", type=float, default=0, help="無通信でこの秒数経過したら終了（0 で無効）")
    ap.add_argument("--monitor", action="store_true",
                    help="受信した MIDI をそのまま表示するだけ（ケーブルの疎通確認用）")
    ap.add_argument("--fresh", action="store_true",
                    help="既存の wave.part を使わず最初から集め直す（元は .part.old に退避）")
    a = ap.parse_args()

    if a.list or not a.port:
        print("MIDI 入力ポート:")
        for i, p in enumerate(rtmidi.MidiIn().get_ports()):
            print(f"  {i}: {p}")
        print("MIDI 出力ポート:")
        for i, p in enumerate(rtmidi.MidiOut().get_ports()):
            print(f"  {i}: {p}")
        return 0

    midiin, inname = open_port(rtmidi.MidiIn, a.port, "入力")
    if midiin is None:
        return 1
    midiin.ignore_types(sysex=False, timing=True, active_sense=True)
    print(f"受信中: {inname}  （Ctrl+C で中断）")

    midiout = None
    if a.req_port:
        midiout, outname = open_port(rtmidi.MidiOut, a.req_port, "出力")
        if midiout is None:
            midiin.close_port()
            return 1
        print(f"再送要求: {outname}")

    if a.monitor:
        n = 0
        try:
            while True:
                m = midiin.get_message()
                if m is None:
                    time.sleep(0.001); continue
                d = bytes(m[0]); n += 1
                print(f"{n:5}: {len(d):5} byte  {d[:16].hex(' ')}{' ...' if len(d) > 16 else ''}")
        except KeyboardInterrupt:
            print("")
            print(f"{n} メッセージ受信")
        midiin.close_port()
        return 0

    wpb = a.words_per_block
    nblocks = (a.words + wpb - 1) // wpb
    a.out.mkdir(parents=True, exist_ok=True)
    part = a.out / "wave.part"
    data = bytearray(a.words * 4)
    have = bytearray(nblocks)
    if part.exists() and a.fresh:
        bak = part.with_suffix(".part.old")
        part.replace(bak)
        print(f"--fresh: 既存の途中経過を {bak} に退避した")
    elif part.exists():
        d = part.read_bytes()
        if len(d) == len(data) + nblocks:
            data[:] = d[:len(data)]; have[:] = d[len(data):]
            print(f"途中経過を読み込み: {sum(have)}/{nblocks} ブロック")
            if sum(have) == nblocks:
                print("")
                print("!! この途中経過は既に全ブロック揃っている。1 バイトも受信せずに")
                print("!! ROM ファイルを書き出して終了する。")
                print(f"!! 吸い直すなら {part} を消すか --fresh を付けること。")
                print("")

    def save():
        """途中経過を一時ファイル経由で保存する（書き込み中の中断で壊さないため）。

        33MB あるので、連結して 1 個の bytes を作ると余計なコピーが要る。
        2 回に分けて書けばコピーが要らない。
        """
        tmp = part.with_suffix(".tmp")
        with open(tmp, "wb") as f:
            f.write(data)
            f.write(have)
        tmp.replace(part)

    def next_missing(after):
        for i in range(nblocks):
            b = (after + i) % nblocks
            if not have[b]:
                return b
        return None

    # --- USB 版の引き金。ファームは完全に受動的で、バイトを送ると受信割り込み
    # (IRQ3) が上がり、rxisr が 1 ブロックだけ積んで帰る。送らなければ何も来ない。
    midiprime = None
    if a.prime_port:
        midiprime, primename = open_port(rtmidi.MidiOut, a.prime_port, "呼び水")
        if midiprime is None:
            midiin.close_port()
            if midiout is not None:
                midiout.close_port()
            return 1
        print(f"引き金: {primename} へ（無音のノートオフ。返事待ちを {a.window} 個までに抑える）")

    buf = bytearray()
    t0 = last = last_save = last_req = time.time()
    # **毎周 sum(have) を呼んではいけない。** 131,072 バイトの合計を毎周やると
    # Python 側が飽和して受信を捌けなくなり、全部詰まる（実機で 3750 ブロックあたりで停止）。
    ngot = sum(have)   # 取得済みブロック数。以降は増分で持つ
    got0 = ngot
    cur = 0            # 直近に受け取ったブロック番号
    known_run = 0      # 既に持っているブロックが続いた数
    bad = 0            # 加算和が合わなかった数
    reqs = 0           # 出した再送要求の数
    nrecv = 0          # 受け取ったブロックの総数（要求を出してよいかの判断に使う）
    mismatch = False   # ブロック長の食い違いを 1 回だけ知らせる
    primes = 0         # 送った引き金の数
    # 返事待ちの引き金を、送った時刻の並びで持つ。ブロックが 1 個返ったら古い方から
    # 1 つ消す。リングが満杯で捨てられた引き金には返事が来ないので、時間で失効させる。
    pending = collections.deque()
    send_err = False   # 引き金の送信失敗を 1 回だけ知らせる
    last_beat = 0.0    # 停滞を知らせた時刻
    try:
        while ngot < nblocks:
            now = time.time()
            # --- 引き金は「返ってきた分だけ」送る。
            # 一定の割合で投げ続けると、実際に返せる速さを超えたぶんが
            # MU2000 の USB 受信側に溜まって詰まる（Windows の送信キューも溢れる）。
            # 返事待ちを --window 個までに抑えれば、送りすぎが原理的に起きない。
            while pending and now - pending[0] > a.credit_ttl:
                pending.popleft()          # 返事の来ない引き金を失効させる
            if midiprime is not None and len(pending) < a.window:
                try:
                    midiprime.send_message(TRIGGER)
                    primes += 1
                    pending.append(now)
                except Exception as e:
                    # 詰まっているときは少し待つ。ここで落とすと途中経過を失う
                    if not send_err:
                        print("")
                        print(f"引き金の送信に失敗した（{e}）。間を空けて続けます。")
                        send_err = True
                    time.sleep(0.05)
            # --- 穴が残っているのに進まないときは、欠けているブロックへ飛ばす
            # 1 個も受け取っていないうちは出さない。出すとダンパを頭から飛ばしてしまう
            if midiout is not None and nrecv > 0 and now - last_req > REQ_MIN_INTERVAL and \
                    (known_run >= KNOWN_RUN_TRIGGER or now - last > IDLE_TRIGGER):
                m = next_missing(cur + 1)
                if m is not None:
                    request(midiout, m)
                    reqs += 1
                    last_req = now
                    known_run = 0

            # --- 止まったときに、PC 側か MU2000 側かを見分けられるようにする
            if now - last > 3.0 and now - last_beat > 3.0:
                print("")
                print(f"  [{now - last:.0f} 秒 ブロックが来ていない] "
                      f"引き金 {primes} 個（返事待ち {len(pending)}）。"
                      f"MU2000 の LCD が生きていれば PC 側、死んでいれば本体側。")
                last_beat = now

            msg_in = midiin.get_message()
            if msg_in is None:
                if a.timeout and now - last > a.timeout:
                    print("\nタイムアウト")
                    break
                time.sleep(0.001)
                continue
            chunk = bytes(msg_in[0])
            if chunk and chunk[0] == 0xF0:
                buf = bytearray(chunk)
            elif buf:
                buf += chunk
            else:
                continue
            if buf[-1] != 0xF7:
                continue
            msg = bytes(buf)
            buf = bytearray()
            r = parse(msg)
            if r is None:
                continue
            if r[0] == "bad":
                bad += 1
                if pending:
                    pending.popleft()   # 化けていても引き金 1 個は消費されている
                last = time.time()
                continue
            blk, words = r
            if len(words) != wpb:
                if not mismatch:
                    print("")
                    print("1 ブロック %d ワードで届いている。--words-per-block %d を付け直すこと。" % (len(words), len(words)))
                    mismatch = True
                continue
            if blk >= nblocks:
                continue
            cur = blk
            nrecv += 1
            if pending:
                pending.popleft()       # 1 ブロック返ったので窓を 1 つ空ける
            last = time.time()
            if have[blk]:
                known_run += 1
                continue
            known_run = 0
            off = blk * wpb
            for i, w in enumerate(words):
                if off + i < a.words:
                    struct.pack_into("<I", data, (off + i) * 4, w)
            have[blk] = 1
            ngot += 1
            n = ngot
            if time.time() - last_save > 60:
                save()
                last_save = time.time()
            if n % 50 == 0 or n == nblocks:
                el = time.time() - t0
                rate = (n - got0) / el if el else 0
                eta = (nblocks - n) / rate / 60 if rate else 0
                print(f"\r{n}/{nblocks} ブロック ({100*n/nblocks:5.1f}%)  {rate:.1f} blk/s  "
                      f"残り {eta:.0f} 分  加算和 NG {bad}  再送要求 {reqs}", end="")
    except KeyboardInterrupt:
        print("\n中断")
    midiin.close_port()
    if midiout is not None:
        midiout.close_port()
    if midiprime is not None:
        midiprime.close_port()

    save()
    print("")
    print(f"{ngot}/{nblocks} ブロック取得。加算和 NG {bad} 個、再送要求 {reqs} 回、"
          f"引き金 {primes} 個。途中経過: {part}")
    if ngot == nblocks and a.words == ROM_WORDS:
        write_roms(bytes(data), a.out)
    return 0


def deinterleave(buf: bytes):
    """32bit ワード列（リトルエンディアン）-> 下位 16bit の並びと上位 16bit の並び。"""
    lo = bytearray(len(buf) // 2)
    hi = bytearray(len(buf) // 2)
    lo[0::2] = buf[0::4]; lo[1::2] = buf[1::4]
    hi[0::2] = buf[2::4]; hi[1::2] = buf[3::4]
    return bytes(lo), bytes(hi)


def write_roms(data: bytes, out: Path):
    """32MB のリニアイメージ -> MAME 用 ic49/ic50/ic53/ic54"""
    names = [("xv364a0.ic49", "xv365a0.ic50", 0), ("xw848a0.ic53", "xw849a0.ic54", 0x400000)]
    for lo_name, hi_name, base in names:
        lo, hi = deinterleave(data[base * 4:(base + 0x400000) * 4])
        (out / lo_name).write_bytes(lo)
        (out / hi_name).write_bytes(hi)
        print(f"書き出し: {lo_name}, {hi_name}")
    print("tools/dump/verify_roms.py で MAME のハッシュと照合してください")


if __name__ == "__main__":
    sys.exit(main())
