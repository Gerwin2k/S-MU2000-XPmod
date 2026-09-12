#!/usr/bin/env python3
# license:BSD-3-Clause
"""MU2000 の Hello World。フロントパネルの LED を光らせるだけのファームウェア。

## これは何か

**純正ファームを一切呼ばない、完全に自前のコード**が実機で走ることを確かめる玩具。
音は出ないし MIDI も USB も動かない。LED が流れるだけ。それだけのために、
本体ファームの入口を自分のコードに差し替える。

波形 ROM を吸うための一連の作業（doc/dump/usb.md）の副産物で、そこで分かった
「どこを書き換えれば自分のコードが動くか」「どこを触ってはいけないか」を
いちばん小さい形に切り出したもの。

## 仕組み

ダウンローダ（0x000000-0x00C001）は電源投入時にバスと DRAM とスタックを整え、
最後に本体ファームの入口 `0x040100` を JSR で呼ぶ。**そこを乗っ取る。**

    0x040100      12 byte のトランポリン -> 0x3CC000 へ飛ぶ
    0x3CC000〜    本体（このファイルが生成するコード）

0x3CC000 を使うのは、本体ファーム領域 0x040000-0x3DFFFF で 0xFF が続く空きが
**0x3CBBB3-0x3DFFFF の 83KB しか無い**から。純正のコードが入っている番地
（0x041000 など）に置くと、純正を自分で壊すことになる。

ダウンローダがスタック（R15 = 0xFFFFFFFC、SH7043 内蔵 RAM の末尾）まで
用意してくれているので、こちらは何も初期化しなくていい。いきなり書き始められる。

## LED の叩き方

LED はメモリマップされた 2 個のラッチにぶら下がっている。書き込み専用。

| 番地 | 内容 |
|------|------|
| 0xE00000 | LED 8 個（bit0-7） |
| 0xC80000 | bit6-7 = LED 2 個（MU / PLG-1）、**bit0-5 はスイッチ走査の列選択** |

`MOV.B` で 1 バイト書くだけ。ポートの設定もクロックの設定も要らない。
**0xC80000 の bit0-5 は 0 のままにしておくこと。** あそこはパネルのスイッチを
読むための列選択で、勝手な値を書くと押していないキーを押したことにできてしまう。

## 割り込みは全部止める

入口で SR の割り込みマスクを 15（全禁止）にする。ダウンローダが据えた
ハンドラが残っていて、こちらの知らないところで動くと気持ちが悪いため。
これで CPU はこのループ以外は何もしない状態になる。

## 戻し方

**ダウンローダ領域には 1 バイトも触らない。** どんなに壊れても
[Drum]+[PLAY]+[VALUE+] を押しながら電源投入すればダウンロードモードに入れる。

    python tools/dump/stage_ydl.py --stock
    build/upgrade_dumper/Upgrade.exe

## 使い方

    python tools/dump/build_firmware.py --module blink -o build/firmware_blink.bin
    python tools/dump/make_ydl.py --image build/firmware_blink.bin -o build/blink.ydl
    python tools/dump/stage_ydl.py build/blink.ydl
    build/upgrade_dumper/Upgrade.exe
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY

LATCH_E = 0x00E00000   # LED 8 個（bit0-7）
LATCH_C = 0x00C80000   # bit6-7 = LED 2 個。bit0-5 はスイッチ走査なので 0 のまま
NLED = 10              # 0xE00000 の 8 個 + 0xC80000 の 2 個

DELAY = 0x20000        # 1 コマの長さ。28MHz で 0x20000 回まわすと 0.05 秒くらい


def build(delay=DELAY, mode="sweep"):
    a = Asm(ENTRY)

    # ---- 割り込みを全部止める。以降このループしか走らない
    a.stc_sr("r0")
    a.movl_imm(0xFFFFFF0F, "r3"); a.rr("and", "r3", "r0")
    a.imm_r0("or", 0xF0)
    a.rn("ldc.sr", "r0")

    a.movl_imm(LATCH_E, "r11")
    a.movl_imm(LATCH_C, "r12")

    def show():
        """r13 の下位 10bit を 2 個のラッチへ配る。r0 を壊す。"""
        a.rr("mov", "r13", "r0")
        a.imm_r0("and", 0xFF)
        a.rr("mov.br@", "r0", "r11")              # LED 8 個
        a.rr("mov", "r13", "r0")
        a.rn("shlr8", "r0")
        a.imm_r0("and", 0x03)                     # bit8-9 を取り出して
        a.rn("shll2", "r0"); a.rn("shll2", "r0"); a.rn("shll2", "r0")   # bit6-7 へ
        a.rr("mov.br@", "r0", "r12")              # bit0-5 は 0 のまま = 走査しない

    def wait():
        a.movl_imm(delay, "r14")
        lab = f"wait{len(a.items)}"
        a.label(lab); a.rn("dt", "r14"); a.br("bf", lab)

    if mode == "blink":
        # ---- 全部つける / 全部消す を繰り返すだけ
        a.movi(0, "r13")
        a.label("loop")
        show(); wait()
        a.movl_imm((1 << NLED) - 1, "r0")
        a.rr("xor", "r0", "r13")                  # 全ビット反転
        a.br("bra", "loop"); a.nop()
        a.flush_pool()
    else:
        # ---- 1 個だけ点いた光を、端まで行ったら折り返して往復させる
        a.movi(1, "r13")                          # 点いている位置
        a.movi(0, "r10")                          # 0 = 左へ, 1 = 右へ
        a.label("loop")
        show(); wait()
        a.rr("tst", "r10", "r10")
        a.br("bf", "goright")

        # 左へ。左端を越えたら折り返す
        a.rn("shll", "r13")
        a.movl_imm(1 << NLED, "r1")
        a.rr("cmp/eq", "r1", "r13")
        a.br("bf", "loop")
        a.movl_imm(1 << (NLED - 2), "r13")        # 折り返し先は 1 個内側
        a.movi(1, "r10")
        a.br("bra", "loop"); a.nop()
        a.flush_pool()

        # 右へ。右端を越えたら折り返す
        a.label("goright")
        a.rn("shlr", "r13")
        a.rr("tst", "r13", "r13")
        a.br("bf", "loop")
        a.movi(2, "r13")                          # 折り返し先は 1 個内側
        a.movi(0, "r10")
        a.br("bra", "loop"); a.nop()
        a.flush_pool()

    return a.assemble()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/blink.bin"))
    ap.add_argument("--delay", type=lambda s: int(s, 0), default=DELAY,
                    help="1 コマの長さ。小さくすると速くなる")
    ap.add_argument("--mode", choices=("sweep", "blink"), default="sweep",
                    help="sweep = 光が往復する, blink = 全部が点滅する")
    a = ap.parse_args()
    code = build(delay=a.delay, mode=a.mode)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}  （{a.mode}）")


if __name__ == "__main__":
    sys.exit(main())
