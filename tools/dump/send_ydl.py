#!/usr/bin/env python3
# license:BSD-3-Clause
"""`.ydl` を MIDI OUT から MU2000 のダウンロードモードへ送り込む。

純正の Upgrade.exe は USB 経由で送るが、これは DIN の MIDI OUT から送る。
Yamaha の USB-MIDI ドライバを入れずに済み、HOST SELECT を MIDI にしたまま
書き込みとダンプの両方ができる。

送出ペースは 2 つの条件を両方満たすように待つ:
  - `.ydl` のデルタタイム（セクタ消去の 3 分半、セクタ書き込み後の待ちなど）
  - MIDI の実効転送速度 31250bps（送りすぎてドライバのバッファに溜めない）

  python tools/dump/send_ydl.py --list
  python tools/dump/send_ydl.py --port "Babyface" build/dumper.ydl --dry-run
  python tools/dump/send_ydl.py --port "Babyface" build/dumper.ydl
"""
import argparse
import sys
import time
from pathlib import Path

import rtmidi

sys.path.insert(0, str(Path(__file__).parent))
from make_ydl import parse_smf_events

MIDI_BYTES_PER_SEC = 3125.0     # 31250bps / 10bit
DEFAULT_TEMPO_US = 500000       # .ydl は 120BPM 固定
DEFAULT_TPQN = 96


def plan(path: Path):
    """(累積秒, SysEx バイト列) のリストと総バイト数を返す。"""
    head, evs = parse_smf_events(path)
    tpqn = int.from_bytes(head[12:14], "big") or DEFAULT_TPQN
    tempo = DEFAULT_TEMPO_US
    sec_per_tick = tempo / 1e6 / tpqn
    t = 0.0
    out = []
    total = 0
    for ev in evs:
        t += ev[0] * sec_per_tick
        if ev[1] is None:
            _, _, ty, data = ev
            if ty == 0x51 and len(data) == 3:        # テンポ変更
                tempo = int.from_bytes(data, "big")
                sec_per_tick = tempo / 1e6 / tpqn
            continue
        msg = bytes([0xF0]) + ev[1]
        out.append((t, msg))
        total += len(msg)
    return out, total, tpqn


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ydl", nargs="?", type=Path)
    ap.add_argument("--list", action="store_true", help="MIDI 出力ポート一覧")
    ap.add_argument("--port", help="MIDI 出力ポート名（部分一致）")
    ap.add_argument("--dry-run", action="store_true", help="送らずに所要時間だけ見積もる")
    a = ap.parse_args()

    midiout = rtmidi.MidiOut()
    ports = midiout.get_ports()
    if a.list or (not a.port and not a.dry_run):
        print("MIDI 出力ポート:")
        for i, p in enumerate(ports):
            print(f"  {i}: {p}")
        return 0
    if not a.ydl:
        print(".ydl ファイルを指定してください")
        return 1

    events, total, tpqn = plan(a.ydl)
    wire = total / MIDI_BYTES_PER_SEC
    delta = events[-1][0] if events else 0
    est = max(wire, delta)
    print(f"{a.ydl}: {len(events):,} メッセージ / {total:,} byte / tpqn={tpqn}")
    print(f"  デルタ合計 {delta/60:.1f} 分, 転送時間 {wire/60:.1f} 分 -> 所要 約 {est/60:.0f} 分")
    if a.dry_run:
        return 0

    idx = next((i for i, p in enumerate(ports) if a.port.lower() in p.lower()), None)
    if idx is None:
        print(f"ポートが見つかりません: {a.port}")
        return 1
    print(f"送信先: {ports[idx]}")
    print("MU2000 をダウンロードモードにしてから続行してください。")
    print("（電源を切り、[Drum]+[PLAY]+[VALUE+] を押しながら電源投入）")
    print("")
    print("!! HOST SELECT を必ず MIDI にすること。")
    print("!! USB のままだとダウンローダは MIDI 受信を無効にする")
    print("!! （転送モード 2 のとき SCI の割り込み優先度を 0 にする）。")
    print("!! こちらは相手の返事を見ないので、何も書き込まれていなくても進捗は進む。")
    print("!! 転送が始まったら LCD を見ること。表示が変わらなければ受信していない。")
    print("")
    if input("開始しますか？ [y/N] ").strip().lower() != "y":
        print("中止しました")
        return 1

    midiout.open_port(idx)
    t0 = time.perf_counter()
    wire_free = 0.0
    try:
        for i, (t, msg) in enumerate(events):
            due = max(t, wire_free)
            while True:
                d = due - (time.perf_counter() - t0)
                if d <= 0:
                    break
                time.sleep(min(d, 0.05))
            midiout.send_message(list(msg))
            wire_free = max(due, time.perf_counter() - t0) + len(msg) / MIDI_BYTES_PER_SEC
            if i % 200 == 0 or i == len(events) - 1:
                el = time.perf_counter() - t0
                print(f"\r{i+1}/{len(events)} ({100*(i+1)/len(events):5.1f}%)  経過 {el/60:5.1f} 分", end="")
    except KeyboardInterrupt:
        print("\n中断しました。MU2000 はダウンロードモードのままのはずです。")
        print("純正アップデータの part2 で復旧できます。")
        midiout.close_port()
        return 1
    midiout.close_port()
    print(f"\n送信完了。{(time.perf_counter()-t0)/60:.1f} 分")
    print("MU2000 の電源を入れ直してください。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
