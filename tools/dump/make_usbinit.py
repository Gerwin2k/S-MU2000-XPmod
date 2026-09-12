#!/usr/bin/env python3
# license:BSD-3-Clause
"""純正ファームの初期化をそのまま走らせてから乗っ取るファームウェア。

## 考え方

自作ファームでは USB が立ち上がらないが、純正では立ち上がる（実機で確認済み）。
どのビットが M37640 を起こすのかを探すのは行き詰まった。そこで**探すのをやめて、
純正の初期化列をそのまま実行する**。

純正の入口 0x040100 は、初期化ルーチンを順に呼んで最後にメインループへ飛ぶだけの
きれいな構造だった。

    0x040100: SR のマスクを 0xF0 に
              BSR 0x040192
              JSR 0x1159F0
              BSR 0x040150
              JSR 0x115F62 (R4=1)
              JSR 0x115AAC
              JSR 0x1155FC
              JSR 0x1155C8
              JSR 0x11658A
              JSR 0x116098
              JSR 0x0C7694
              JSR 0x11A4F0     ← この中で USB 受信割り込みが許可される（0x04347C）
              JMP 0x040C5C     ← メインループ。ここだけ自分のコードに差し替える

自作ファームは本体ファーム領域の 0x040100（12 byte）と 0x041000 以降しか
書き換えないので、**呼び先の純正ルーチンはすべて無傷で残っている**。
そのまま呼べる。スタックもダウンローダが 0x400 で設定済み（R15 = 0xFFFFFFFC、
SH7043 の内蔵 RAM の末尾）。

## この版がやること

初期化を全部走らせたあと、LED を点滅させながら待つだけ。目的は 1 つ。

    **純正の初期化を通せば USB は立ち上がるのか。**

立ち上がるなら、あとは純正の送信リング（0x40A846、読み書き位置 0x43DACD/CE）に
波形データを積んで IPRA の IRQ2 を許可すれば、純正のハンドラが USB へ送ってくれる。

LED の点滅は「ここまで来た」という合図。8 回点滅したあと **SR の割り込みマスクを外す**。
純正の 0x040100 は全マスクしてから初期化を呼ぶので、外すのはメインループ側のはず。
IPRA で許可されていても SR で止まっていれば M37640 の割り込みは届かない。
外したあとも点滅が続けば生きている。止まったらそこで落ちている。

注意: 割り込みベクタのヘッダ（0x0400B4-0x0400DC）は**純正のまま**にする。
純正の初期化が用意したデータ構造を、純正のハンドラに使わせるため。
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY

# 純正 0x040100 が順に呼ぶもの（逆アセンブルで確定）
INIT_BSR = (0x00040192, 0x00040150)
INIT_CALLS = (0x001159F0, 0x00115F62, 0x00115AAC, 0x001155FC, 0x001155C8,
              0x0011658A, 0x00116098, 0x000C7694, 0x0011A4F0)
SR_MASK = 0xFF0F
LED = 0x00E00000
BLINK = 1500000
MASKED_BLINKS = 8          # この回数だけマスクしたまま点滅してから割り込みを許可する
MAIN_LOOP = 0x00040C5C     # 純正のメインループ

# ヘッダは純正のまま使う（HEADER_PATCH は定義しない）
LAST_LABELS = {}


def build(blink=BLINK, masked_blinks=8, tail="blink"):
    a = Asm(ENTRY)

    def call(addr):
        a.movl_imm(addr, "r3")
        a.rn("jsr@", "r3"); a.nop()

    # ---- 純正と同じ順序で初期化する
    a.stc_sr("r0"); a.movl_imm(SR_MASK, "r3"); a.rr("and", "r3", "r0")
    a.imm_r0("or", 0xF0); a.rn("ldc.sr", "r0")

    call(INIT_BSR[0])                      # 純正では BSR だが遠いので JSR で呼ぶ
    call(INIT_CALLS[0])                    # 0x1159F0
    call(INIT_BSR[1])                      # 0x040150
    a.movl_imm(INIT_CALLS[1], "r3")        # 0x115F62 は R4=1 で呼ぶ
    a.movi(1, "r4")
    a.rn("jsr@", "r3"); a.nop()
    for adr in INIT_CALLS[2:]:
        call(adr)
    if tail == "stock":
        # 純正とまったく同じ。メインループへ飛んで、あとは純正に任せる。
        # 土台（トランポリンと空き領域配置）が健全かどうかの対照実験。
        a.movl_imm(MAIN_LOOP, "r3")
        a.rn("jmp@", "r3"); a.nop()
        a.flush_pool()
        code = a.assemble()
        LAST_LABELS.clear(); LAST_LABELS.update(a.labels)
        return code

    a.br("bra", "alive"); a.nop()
    a.flush_pool()

    # ---- ここから先はメインループの代わり
    # 純正の 0x040100 は SR を全マスクしてから初期化を呼ぶ。マスクを外すのは
    # メインループ側のはず。IPRA で許可されていても SR で止まっていれば
    # M37640 の割り込みは届かない。まず数回点滅してから外す。
    a.label("alive")
    a.movl_imm(LED, "r11")
    a.movi(0, "r12")                       # 点灯状態
    a.movi(masked_blinks, "r10")           # この回数だけマスクしたまま点滅する
    a.label("loop")
    a.rr("mov", "r12", "r0"); a.rr("mov.br@", "r0", "r11")
    a.rr("mov", "r12", "r0")
    a.rr("tst", "r0", "r0")
    a.br("bf", "on")
    a.movi(-1, "r12")                      # 0xFF
    a.br("bra", "blinked"); a.nop()
    a.label("on")
    a.movi(0, "r12")
    a.label("blinked")

    a.movl_imm(blink, "r14")
    a.label("wait"); a.rn("dt", "r14"); a.br("bf", "wait")

    a.rr("tst", "r10", "r10")
    a.br("bt", "loop")                     # もう外してある
    a.rn("dt", "r10")
    a.br("bf", "loop")                     # まだ回数が残っている
    # ---- 割り込みマスクを外す（純正のハンドラがそのまま据わっている）
    a.stc_sr("r0"); a.movl_imm(0xFFFFFF0F, "r3"); a.rr("and", "r3", "r0")
    a.rn("ldc.sr", "r0")
    a.br("bra", "loop"); a.nop()
    a.flush_pool()

    code = a.assemble()
    LAST_LABELS.clear(); LAST_LABELS.update(a.labels)
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/usbinit.bin"))
    ap.add_argument("--blink", type=lambda s: int(s, 0), default=BLINK)
    ap.add_argument("--masked-blinks", type=int, default=MASKED_BLINKS,
                    help="割り込みを許可するまでの点滅回数")
    ap.add_argument("--tail", choices=("blink", "stock"), default="blink",
                    help="初期化のあと。blink = LED 点滅して待つ, stock = 純正のメインループへ飛ぶ")
    a = ap.parse_args()
    code = build(blink=a.blink, masked_blinks=a.masked_blinks, tail=a.tail)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")
    if a.tail == "stock":
        print("純正の初期化を全部呼んでから純正のメインループへ飛ぶ（= 純正と同じ動作）")
    else:
        print(f"純正の初期化を全部呼ぶ -> {a.masked_blinks} 回点滅 -> 割り込みマスクを外す -> 点滅を続ける")
        print("点滅が止まったらマスクを外した時点で落ちている")


if __name__ == "__main__":
    sys.exit(main())
