#!/usr/bin/env python3
# license:BSD-3-Clause
"""二つの WAV を突き合わせる。

移植の確かめに使う。MAME に鳴らさせたものと、こちらが鳴らしたものを並べて、
「どこで音が抜けているか」を出す。耳で聞いた印象を数にするのが目的。

  compare_wav.py <基準.wav> <比較.wav> [ずらし秒数]

ずらし秒数を省くと、包絡線の相関から自動で合わせる。
"""

import array
import sys
import wave


def load(path):
    w = wave.open(path)
    n, rate, ch = w.getnframes(), w.getframerate(), w.getnchannels()
    a = array.array("h")
    a.frombytes(w.readframes(n))
    w.close()
    left = a[0::ch] if ch > 1 else a
    return left, rate


def envelope(samples, rate, win_ms=50):
    """win_ms ごとの最大振幅。音の有無を見るだけなので RMS でなくてよい"""
    win = int(rate * win_ms / 1000)
    out = []
    for i in range(0, len(samples) - win, win):
        seg = samples[i:i + win]
        out.append(max(max(seg), -min(seg)))
    return out


def best_shift(a, b, limit):
    """b を何コマずらすと a に一番よく重なるか"""
    best, best_score = 0, -1.0
    for s in range(0, limit):
        n = min(len(a) - s, len(b))
        if n < 20:
            break
        score = sum(min(a[i + s], b[i]) for i in range(n)) / n
        if score > best_score:
            best, best_score = s, score
    return best


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ref, rate_a = load(sys.argv[1])
    cmp_, rate_b = load(sys.argv[2])
    # 出力周波数は違ってよい（MAME は 48kHz、こちらは 44.1kHz）。
    # 包絡線は時間の窓で取るので、そのまま並べられる

    win_ms = 50
    ea = envelope(ref, rate_a, win_ms)
    eb = envelope(cmp_, rate_b, win_ms)

    if len(sys.argv) > 3:
        shift = int(float(sys.argv[3]) * 1000 / win_ms)
    else:
        shift = best_shift(ea, eb, int(20 * 1000 / win_ms))
    print("ずらし %.2f 秒で重ねる" % (shift * win_ms / 1000))

    n = min(len(ea) - shift, len(eb))
    a = ea[shift:shift + n]
    b = eb[:n]

    la = sum(a) / n
    lb = sum(b) / n
    print("平均の大きさ  基準 %.0f / 比較 %.0f（比 %.2f）" % (la, lb, lb / la if la else 0))

    # 基準では鳴っているのに比較側が出ていないコマ
    thr = la * 0.15
    missing = [i for i in range(n) if a[i] > thr and b[i] < a[i] * 0.2]
    print("基準が鳴っているのに比較側がほぼ出ていない: %d / %d コマ (%.1f%%)"
          % (len(missing), n, 100.0 * len(missing) / n))

    if missing:
        runs, start, prev = [], missing[0], missing[0]
        for i in missing[1:]:
            if i != prev + 1:
                runs.append((start, prev))
                start = i
            prev = i
        runs.append((start, prev))
        runs.sort(key=lambda r: r[1] - r[0], reverse=True)
        print("長いところ:")
        for s, e in runs[:12]:
            print("  %7.2f 秒から %.2f 秒" % (s * win_ms / 1000, (e - s + 1) * win_ms / 1000))

    # 10 秒ごとの比較
    print()
    print(" 時刻      基準    比較")
    step = int(10 * 1000 / win_ms)
    for i in range(0, n, step):
        sa = max(a[i:i + step], default=0)
        sb = max(b[i:i + step], default=0)
        print("  %5.0f 秒  %6d  %6d  %s" % (i * win_ms / 1000, sa, sb,
                                           "" if sa == 0 else "%.2f" % (sb / sa)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
