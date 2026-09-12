#!/usr/bin/env python3
# license:BSD-3-Clause
"""割り込みハンドラを「包むだけ」のファーム。切り分け専用で、何も送らない。

## 何のためのものか

USB へデータを流す仕組みは、ヘッダ 0x0400D8 / 0x0400DC に自前のハンドラ番地を据え、
その中で純正のハンドラを JSR してから自分の処理をする、という形をとっている。

    txisr: JSR 0x00043486 (純正の送信ハンドラ) -> 自分の処理 -> RTS
    rxisr: JSR 0x00043630 (純正の受信ハンドラ) -> 自分の処理 -> RTS

**この「包む」構造そのものが実機で成立するのかを、一度も確かめていない。**
自分の処理を全部外して包むだけにすれば、それが分かる。

  - 純正どおりに動く（LCD が出る / DEMO が動く / usbping で THRU が返る）
      -> 包む構造は正しい。原因は自分の処理（fill）側にある
  - 沈黙する
      -> 包み方が間違っている。純正ハンドラは JSR で呼んで帰れるものではない

## 純正ハンドラの帰り方（静的解析）

  0x00043486 は末尾で BRA 0x0004354C。その先は LDC R0,SR; RTS で素直に帰る
  0x00043630 は 0x000436C0 で PR と R14 を戻してから BRA 0x000428D4 へ末尾ジャンプ。
             0x428D4 は R14/R13/R12/R11/R10/PR を積む普通の関数で、最後は RTS

どちらも JSR で呼べば帰ってくるはず、というのが静的解析の結論。それを実機で確かめる。

## レジスタの約束

ダウンローダの IRQ2/IRQ3 スタブ（0x0032DE / 0x00003318）が R0-R7 と PR を退避し、
割り込みスタックへ切り替えてから JSR してくる。よって R0-R7 は自由に使え、
R8-R15 は触らない。PR は入口で退避して戻す。
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY

# 純正 0x040100 が順に呼ぶもの（0x040100 の逆アセンブルと一致することを確認済み）
INIT_BSR = (0x00040192, 0x00040150)
INIT_CALLS = (0x001159F0, 0x00115F62, 0x00115AAC, 0x001155FC, 0x001155C8,
              0x0011658A, 0x00116098, 0x000C7694, 0x0011A4F0)
SR_MASK = 0xFF0F
MAIN_LOOP = 0x00040C5C

USB_TX_ISR = 0x00043486     # 純正の送信割り込みハンドラ（IRQ2）
USB_RX_ISR = 0x00043630     # 純正の受信割り込みハンドラ（IRQ3）

HEADER_PATCH = {0x000400D8: "txisr", 0x000400DC: "rxisr"}
LAST_LABELS = {}


def build():
    a = Asm(ENTRY)

    def call(addr):
        a.movl_imm(addr, "r3")
        a.rn("jsr@", "r3"); a.nop()

    # ================= 入口: 純正の初期化 -> 純正のメインループ =================
    a.stc_sr("r0"); a.movl_imm(SR_MASK, "r3"); a.rr("and", "r3", "r0")
    a.imm_r0("or", 0xF0); a.rn("ldc.sr", "r0")
    call(INIT_BSR[0]); call(INIT_CALLS[0]); call(INIT_BSR[1])
    a.movl_imm(INIT_CALLS[1], "r3"); a.movi(1, "r4")
    a.rn("jsr@", "r3"); a.nop()
    for adr in INIT_CALLS[2:]:
        call(adr)
    a.movl_imm(MAIN_LOOP, "r3")
    a.rn("jmp@", "r3"); a.nop()
    a.flush_pool()

    # ================= 包むだけ =================
    a.label("txisr")
    a.sts_pr_push()
    call(USB_TX_ISR)
    a.lds_pr_pop()
    a.rts(); a.nop()
    a.flush_pool()

    a.label("rxisr")
    a.sts_pr_push()
    call(USB_RX_ISR)
    a.lds_pr_pop()
    a.rts(); a.nop()
    a.flush_pool()

    code = a.assemble()
    LAST_LABELS.clear(); LAST_LABELS.update(a.labels)
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/usbwrap.bin"))
    a = ap.parse_args()
    code = build()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")
    for off, name in sorted(HEADER_PATCH.items()):
        print(f"  {off:#08x} -> {name}")


if __name__ == "__main__":
    sys.exit(main())
