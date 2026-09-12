#!/usr/bin/env python3
# license:BSD-3-Clause
"""MIDI IN が SH7043 の SCI に届いているかを確かめるだけの試験ファームウェア。

ダンパ本体を流す前に、経路（ケーブル・HOST SELECT・PA21・SCI の設定）が
生きているかを切り分けるために使う。MAME でも実機でも同じものが動く。

動作:
  1. SCI0 を 31250bps・送受信有効にする（--rx-sci 1 なら受信は SCI1）
  2. 起動したことを知らせる  F0 43 7D 'R' 'D' 'Y' F7
  3. 1 バイト受信するたびに  F0 43 7D 'E' <上位4bit> <下位4bit> F7 を返す（既定 64 バイトまで）
  4. 何も来ないまま一定時間たつと  F0 43 7D 'S' <SSR 上位4bit> <SSR 下位4bit> F7 を送る

echo の上限があるのは、仮想 MIDI ポートで送受を同じ線に繋いだときに
自分の返事が自分に返って無限に増えるのを防ぐため。

SSR の周期報告を見れば、受信そのものが来ていないのか（0x84 のまま）、
オーバーランで止まっているのか（bit5 = ORER が立つ）が分かる。
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY


def build(echo_limit=64, idle=400000, rx_sci=0):
    a = Asm(ENTRY)
    rx_base = M.SCI1 if rx_sci else M.SCI0

    def send_byte(v):
        a.movi(v & 0xFF if v < 0x80 else v - 0x100, "r0")
        a.br("bsr", "send"); a.nop()

    def send_nibbles(src):
        """src の下位 8bit を 4bit ずつ 2 個の MIDI データバイトで送る。"""
        a.rr("mov", src, "r0")
        a.rn("shlr2", "r0"); a.rn("shlr2", "r0")
        a.imm_r0("and", 0x0F)
        a.br("bsr", "send"); a.nop()
        a.rr("mov", src, "r0")
        a.imm_r0("and", 0x0F)
        a.br("bsr", "send"); a.nop()

    def sci_init(base, scr, tag):
        a.movl_imm(base, "r13")
        a.movi(0, "r0")
        a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")   # SCR = 0
        a.rr("mov.br@", "r0", "r13")                                                # SMR = 0
        a.movi(M.BRR_31250, "r0")
        a.rr("mov", "r13", "r14"); a.addi(1, "r14"); a.rr("mov.br@", "r0", "r14")   # BRR
        a.movl_imm(20000, "r14")
        a.label(tag); a.rn("dt", "r14"); a.br("bf", tag)
        a.movi(scr, "r0")
        a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")

    # ---- 割り込みを全マスク
    a.stc_sr("r0"); a.imm_r0("or", 0xF0); a.rn("ldc.sr", "r0")

    sci_init(M.SCI0, 0x30 if rx_sci == 0 else 0x20, "w0")     # TE|RE / TE のみ
    if rx_sci:
        sci_init(M.SCI1, 0x10, "w1")                          # RE のみ

    # ---- PA21 を出力・1 にする（背面 MIDI IN A を RXD0 へ）
    a.movl_imm(M.PAIOR_H, "r13"); a.movi(M.PA21, "r0"); a.rr("mov.wr@", "r0", "r13")
    a.movl_imm(M.PADR_H, "r13"); a.movi(M.PA21, "r0"); a.rr("mov.wr@", "r0", "r13")

    a.movl_imm(M.SCI0 + 4, "r1")        # 送出用 SSR
    a.movl_imm(rx_base + 4, "r2")       # 受信用 SSR（+1 が RDR）
    a.movl_imm(echo_limit, "r7")        # 返事の残り回数
    a.br("bra", "ready"); a.nop()
    a.flush_pool()

    a.label("ready")
    for b in (0xF0, 0x43, 0x7D, 0x52, 0x44, 0x59, 0xF7):      # F0 43 7D "RDY" F7
        send_byte(b)

    a.label("outer")
    a.movl_imm(idle, "r6")

    a.label("wait")
    a.rr("mov.b@r", "r2", "r0")
    a.imm_r0("and", 0x78)                                     # RDRF|ORER|FER|PER
    a.rr("tst", "r0", "r0")
    a.br("bt", "tick")
    a.imm_r0("and", 0x40)                                     # RDRF だけ残す
    a.rr("tst", "r0", "r0")
    a.br("bf", "gotbyte")
    a.rr("mov.b@r", "r2", "r0")                               # エラーだけ -> 落とす
    a.imm_r0("and", 0x87)
    a.rr("mov.br@", "r0", "r2")
    a.label("tick")
    a.rn("dt", "r6"); a.br("bf", "wait")

    # ---- 無音のまま時間切れ: SSR をそのまま報告する
    a.rr("mov.b@r", "r2", "r5"); a.rr("extu.b", "r5", "r5")
    for b in (0xF0, 0x43, 0x7D, 0x53):                        # F0 43 7D 'S'
        send_byte(b)
    send_nibbles("r5")
    send_byte(0xF7)
    a.br("bra", "outer"); a.nop()

    a.label("gotbyte")
    a.movb_disp_r0(1, "r2")                                   # RDR
    a.rr("mov", "r0", "r5"); a.rr("extu.b", "r5", "r5")
    a.rr("mov.b@r", "r2", "r0")                               # RDRF/ORER/FER/PER を落とす
    a.imm_r0("and", 0x87)
    a.rr("mov.br@", "r0", "r2")
    a.rr("tst", "r7", "r7"); a.br("bt", "outer")              # 返事の上限に達した
    a.addi(-1, "r7")
    for b in (0xF0, 0x43, 0x7D, 0x45):                        # F0 43 7D 'E'
        send_byte(b)
    send_nibbles("r5")
    send_byte(0xF7)
    a.br("bra", "outer"); a.nop()

    # ---- send: r0 の下位 8bit を SCI0 から送る。r13, r14 を破壊する
    a.label("send")
    a.rr("mov", "r0", "r13")
    a.label("sendwait")
    a.rr("mov.b@r", "r1", "r14"); a.rr("extu.b", "r14", "r14")
    a.rr("mov", "r14", "r0"); a.imm_r0("tst", 0x80); a.br("bt", "sendwait")
    a.rr("mov", "r1", "r14"); a.addi(-1, "r14"); a.rr("mov.br@", "r13", "r14")
    a.rr("mov.b@r", "r1", "r0"); a.rr("extu.b", "r0", "r0")
    a.imm_r0("and", 0x7F); a.rr("mov.br@", "r0", "r1")
    a.rts(); a.nop()
    a.flush_pool()
    return a.assemble()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/rxtest.bin"))
    ap.add_argument("--echo-limit", type=int, default=64)
    ap.add_argument("--rx-sci", type=int, choices=(0, 1), default=0)
    a = ap.parse_args()
    code = build(echo_limit=a.echo_limit, rx_sci=a.rx_sci)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")


if __name__ == "__main__":
    sys.exit(main())
