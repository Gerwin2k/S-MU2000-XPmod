#!/usr/bin/env python3
# license:BSD-3-Clause
#
# MAME の mu2000.lay から panel.txt と絵の SVG を起こす。
#
#   python tools/lay2panel.py <mame>/src/mame/layout/mu2000.lay <出す先>
#
# MAME の配置は CC0-1.0（hap、Felipe Sanches）なので、そのまま使ってよい。
# 出てくるのは
#   <出す先>/panel.txt          こちらの配置ファイル
#   <出す先>/mu2000-mame.svg    埋め込まれていた絵
#
# MAME の座標（1640 x 680）を、こちらの論理座標（1000 x 400、本体は 385）へ
# 移す。縦横比は保ったまま横を真ん中に寄せるので、絵と部品の位置がぴたりと
# 合う。

import re
import sys
import os

MAME_W, MAME_H = 1640.0, 680.0
OUR_W, OUR_BODY = 1000.0, 385.0

K = min(OUR_W / MAME_W, OUR_BODY / MAME_H)
OX = (OUR_W - MAME_W * K) / 2
OY = 0.0


def x(v):  return round(OX + float(v) * K, 1)
def y(v):  return round(OY + float(v) * K, 1)
def s(v):  return round(float(v) * K, 1)


def bounds(tag):
    d = {}
    for k in ("x", "y", "width", "height", "left", "right", "top", "bottom"):
        m = re.search(r'\b%s="([-\d.]+)"' % k, tag)
        if m:
            d[k] = float(m.group(1))
    if "left" in d:
        d["x"], d["y"] = d["left"], d["top"]
        d["width"], d["height"] = d["right"] - d["left"], d["bottom"] - d["top"]
    return d


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    lay = open(sys.argv[1], encoding="utf-8").read()
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    # 1. 埋め込まれている絵を取り出す
    m = re.search(r"<image><data><!\[CDATA\[(.*?)\]\]></data></image>", lay, re.S)
    if not m:
        print("絵が見つからない")
        return 1
    svg_path = os.path.join(out_dir, "mu2000-mame.svg")
    open(svg_path, "w", encoding="utf-8").write(m.group(1).strip() + "\n")

    # 2. element の定義から、名前 -> 出す文字
    texts = dict(re.findall(r'<element name="([\w.]+)"><text string="([^"]*)"', lay))

    # 3. view の中の置き場所を順に拾う
    view = lay[lay.index("<view "):]
    items = []
    for mm in re.finditer(
            r'<element(?P<attrs>[^>]*?)>(?P<mid>.*?)<bounds(?P<b>[^/]*)/>', view, re.S):
        a = mm.group("attrs")
        ref = re.search(r'ref="([\w.]+)"', a)
        nm = re.search(r'name="([\w.]+)"', a)
        cmt = re.search(r"<!--\s*(.*?)\s*-->", mm.group("mid"))
        items.append({
            "ref": ref.group(1) if ref else "",
            "name": nm.group(1) if nm else "",
            "comment": cmt.group(1) if cmt else "",
            "b": bounds(mm.group("b")),
        })

    screen = bounds(re.search(r"<screen[^>]*>\s*<bounds([^/]*)/>", view).group(1))

    leds = [it for it in items if it["name"].startswith("LED")]
    leds.sort(key=lambda it: int(it["name"][3:]))
    cats = [it for it in items if it["ref"] == "rect_yellow_button"]
    navs = [it for it in items if it["ref"] == "rect_white_button"]
    rounds = [it for it in items if it["ref"] == "round_white_button"]

    if len(cats) != 18 or len(navs) != 9 or len(rounds) != 2 or len(leds) < 10:
        print("数が合わない: cat %d nav %d round %d led %d"
              % (len(cats), len(navs), len(rounds), len(leds)))
        return 1

    # 音色カテゴリは 6 列 3 行。左上から並んでいる。
    # 同じ列でも 1 ドットずれていることがあるので、近いものはまとめる
    def cluster(vals, gap=8.0):
        out = []
        for v in sorted(vals):
            if out and v - out[-1][-1] <= gap:
                out[-1].append(v)
            else:
                out.append([v])
        return [sum(g) / len(g) for g in out]

    cat_x = cluster([c["b"]["x"] + c["b"]["width"] / 2 for c in cats])
    cat_y = cluster([c["b"]["y"] for c in cats])
    if len(cat_x) != 6 or len(cat_y) != 3:
        print("音色カテゴリの列と行がまとまらない: %d 列 %d 行" % (len(cat_x), len(cat_y)))
        return 1

    f = open(os.path.join(out_dir, "panel.txt"), "w", encoding="utf-8")
    w = f.write
    w("# MAME の mu2000.lay から起こしたパネルの配置\n"
      "#\n"
      "# もとは MAME の src/mame/layout/mu2000.lay。license:CC0-1.0、\n"
      "# 作った人は hap と Felipe Sanches。CC0 なので好きに使ってよい。\n"
      "#\n"
      "# tools/lay2panel.py が起こした。手で直してもよい。\n"
      "# 直したら gui.exe の窓で F5 を押すと読み直す。\n\n")

    w("body_h %g\n" % OUR_BODY)
    w("lcd    %g %g %g %g\n\n" % (x(screen["x"]), y(screen["y"]),
                                  s(screen["width"]), s(screen["height"])))

    w("cat.x  " + " ".join("%g" % x(v) for v in cat_x) + "\n")
    w("cat.y  " + " ".join("%g" % y(v) for v in cat_y) + "\n")
    w("cat.size %g %g\n\n" % (s(cats[0]["b"]["width"]), s(cats[0]["b"]["height"])))

    names = ["play", "edit", "util", "effect", "sampling", "seq"]
    for i in range(6):
        b = leds[i]["b"]
        w("mode.%-9s %g %g\n" % (names[i], x(b["x"] + b["width"] / 2),
                                 y(b["y"] + b["height"] / 2)))
    w("mode.r %g %g\n\n" % (s(leds[0]["b"]["width"] / 2 + 1), s(leds[0]["b"]["width"] / 2)))

    nav_names = ["mute_solo", "part-", "part+", "enter", "select-", "select+",
                 "exit", "value-", "value+"]
    for i in range(9):
        b = navs[i]["b"]
        w("nav.%-10s %g %g %g %g\n" % (nav_names[i], x(b["x"]), y(b["y"]),
                                       s(b["width"]), s(b["height"])))
    w("\n")
    for i, nm in enumerate(["select", "audition"]):
        b = rounds[i]["b"]
        w("round.%-8s %g %g %g %g\n" % (nm, x(b["x"] + b["width"] / 2),
                                        y(b["y"] + b["height"] / 2),
                                        s(b["width"]), s(b["height"])))

    # MU / PLG-1..3 の表示灯は LED6-9
    p0, p1 = leds[6]["b"], leds[7]["b"]
    w("\nplg %g %g %g\n" % (x(p0["x"] + p0["width"] / 2),
                            s(p1["x"] - p0["x"]),
                            y(p0["y"] + p0["height"] / 2)))

    # 音量つまみ。MAME の .lay には無く絵の中なので、VOLUME の札から起こす
    vol = next(it for it in items if it["ref"] == "volume_text")
    vb = vol["b"]
    w('volume %g %g %g "../parts/knob.svg"\n'
      % (x(vb["x"] + vb["width"] / 2), y(vb["y"] - 34), s(31)))

    # 大きなダイヤルも .lay には無く絵の中。実測で合わせた位置を使う
    w('dial 893 268 60 "../parts/dial.svg"\n')

    # ボタンと表示灯の絵。art/parts/ の見本を指す
    w('mode.art  "../parts/btn.svg" "../parts/btn-on.svg" "../parts/btn-down.svg"\n')
    w('nav.art   "../parts/key.svg" "../parts/key-down.svg"\n')
    w('cat.art   "../parts/cat.svg" "../parts/cat-down.svg"\n')
    w('round.art "../parts/rnd.svg" "../parts/rnd-down.svg"\n')
    w('plg.art   "../parts/plg.svg" "../parts/plg-on.svg"\n')

    # 窓の下の札の高さは PART の字から
    part = next(it for it in items if it["ref"] == "part_text")
    w("columns.y %g\n" % y(part["b"]["y"]))
    w("\n# LCD 下段の並びはこちらの実測のまま（doc/lcd-segments.md）\n")
    w("low.x 0 12 30 48 55 61 70 78 86 93 0\n")
    w("low.w 10 15 15 2 2 8 7 7 7 8 3\n")

    w("\n# ---- 飾り。まず MAME の絵、そのうえに字を置く\n")
    w('art 0 0 %g %g "mu2000-mame.svg"\n\n' % (OUR_W, OUR_BODY))

    # 字。ボタンの名札はこちらが描くので、それ以外を出す
    skip = {"part_text", "bank_pgm_text", "vol_text", "exp_text", "pan_text",
            "rev_text", "cho_text", "var_text", "key_text",
            "mute_text", "solo_text", "part_button_text", "enter_text",
            "select_button_text", "exit_text", "value_text",
            "play_text", "edit_text", "util_text", "effect_text",
            "sampling_text", "seq_text", "select_text", "audition_text",
            "mu_text", "plg1_text", "plg2_text", "plg3_text"}
    for it in items:
        ref = it["ref"]
        if ref not in texts or ref in skip:
            continue
        cat_labels = {"piano", "chrom. perc.", "organ", "guitar", "bass",
                      "strings", "ensemble", "brass", "reed", "pipe",
                      "synth lead", "synth pad", "synth effects", "ethnic",
                      "percussive", "sfx", "model excl.", "drum", "volume"}
        if texts[ref].lower() in cat_labels:
            continue                      # 音色カテゴリと VOLUME はこちらが描く
        b = it["b"]
        big = ref in ("yamaha_text", "mu2000_text")
        w('text %g %g %g %g %s left ink "%s"\n'
          % (x(b["x"]), y(b["y"]), s(b["width"]) + 6, s(b["height"]) + 4,
             "label" if big else "small", texts[ref]))
    f.close()

    print("書き出した:")
    print("  " + os.path.join(out_dir, "panel.txt"))
    print("  " + svg_path)
    print("MAME の配置は CC0-1.0（hap、Felipe Sanches）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
