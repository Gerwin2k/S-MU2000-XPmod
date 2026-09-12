#!/usr/bin/env python3
# license:BSD-3-Clause
"""USB（M37640）を起こすビットを探す探査ファームウェア。

## ここまでに分かっていること

  - M37640 のホスト側インタフェースは 0xF80000（データ）と 0xF80001（状態/送信）の 2 本
  - /IBF0 /OBF0 が SH-2 の IRQ2/IRQ3 に繋がっている（サービスマニュアル UD:IC3 のピン表）
  - 割り込みは RAM の関数ポインタ経由。ダウンローダが本体ファームのヘッダから写す
        0x0400D8 -> 0x401044 (IRQ2 = 送信)   0x0400DC -> 0x401040 (IRQ3 = 受信)
  - IPRA(0xFFFF8348) の許可はダウンロードモードでしか行われない

自前ハンドラを据えて IPRA も SR も同じ状態にしたが、**割り込みは一度も来なかった**。
0xF80001 は常に 0x33 を返し、書き込みも効かない。M37640 が動いていない。

## この探査の狙い

ダウンローダには LED ラッチのビットを立てる／落とすルーチンがある。

    0x000CC4:  シャドウ(0x401048) |= ビット ; byte @0x00E00000 = シャドウ
    0x000D10:  シャドウ          &= ~ビット ; byte @0x00E00000 = シャドウ

初期化はこのラッチに 0 を書く。ダウンロードモードではその後 LED を光らせるので
ビットが立つ。**このラッチのどれかが M37640 のリセットや電源を握っている**なら、
通常起動では 0 のままで止まったままになる。0xC80000 も同じ性質の 8bit ラッチ。

そこで 2 つのラッチを 1 ビットずつ、最後に全ビットを立てて総当たりする。
各段階は約 16 秒保持するので、PC 側の PnP 監視（3 秒ごと）が必ず何度か見る。

割り込みの仕掛けは最初から入れてあるので、M37640 が起きた瞬間に
IRQ2/IRQ3 の回数が増えて分かる。

MIDI OUT に出るもの:
  F0 43 7D 'R' 'D' 'Y' F7
  F0 43 7D 'L' <段階> <0xE00000の値> <0xC80000の値> <F80001> <IRQ2回数> <IRQ3回数> F7
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY

USB_DATA = 0x00F80000
USB_STAT = 0x00F80001
IPRA = 0xFFFF8348
IPRA_ON = 0xDD             # IRQ2(送信) と IRQ3(受信) を優先度 13 で許可
IPRA_TX_OFF = 0xFF2F

LATCH_E = 0x00E00000       # LED ラッチ 2（シャドウ 0x401048）
LATCH_C = 0x00C80000       # LED/スイッチ ラッチ 1

RAM = 0x00420000
OFF_TXCNT, OFF_RXCNT, OFF_STAT, OFF_BYTE, OFF_TAB = 0, 4, 8, 0x0C, 0x40

SWEEP = (0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xFF)
REPEATS = 6                # 1 段階あたりの報告回数（≒ 秒）
INNER = 150000
GAP = 50

HEADER_PATCH = {
    0x0400D8: "irq_tx",
    0x0400DC: "irq_rx",
    0x0400B4: "irq_null",
    0x0400B8: "irq_null",
    0x0400BC: "irq_null",
    0x0400C0: "irq_null",
}
LAST_LABELS = {}


def steps():
    """(0xE00000 に書く値, 0xC80000 に書く値) の並び。"""
    out = [(v, 0x00) for v in SWEEP]          # E00000 を掃く
    out += [(0x00, v) for v in SWEEP[1:]]     # C80000 を掃く（0,0 は重複なので飛ばす）
    out += [(0xFF, 0xFF)]                     # 最後に両方立てる
    return out


def build(inner=INNER, repeats=REPEATS, gap=GAP):
    a = Asm(ENTRY)
    ST = steps()

    def send_byte(v):
        a.movi(v & 0xFF if v < 0x80 else v - 0x100, "r0")
        a.br("bsr", "send"); a.nop()

    def send16(reg):
        for sh in (14, 7, 0):
            a.rr("mov", reg, "r0")
            s = sh
            for n, op in ((8, "shlr8"), (2, "shlr2"), (1, "shlr")):
                while s >= n:
                    a.rn(op, "r0"); s -= n
            a.imm_r0("and", 0x7F)
            a.br("bsr", "send"); a.nop()

    # ---- 初期化
    a.stc_sr("r0"); a.imm_r0("or", 0xF0); a.rn("ldc.sr", "r0")

    a.movl_imm(M.SCI0, "r13")
    a.movi(0, "r0")
    a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")
    a.rr("mov.br@", "r0", "r13")
    a.movi(M.BRR_31250, "r0")
    a.rr("mov", "r13", "r14"); a.addi(1, "r14"); a.rr("mov.br@", "r0", "r14")
    a.movl_imm(20000, "r14")
    a.label("iw"); a.rn("dt", "r14"); a.br("bf", "iw")
    a.movi(0x20, "r0")
    a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")

    a.movl_imm(RAM, "r13"); a.movi(0, "r0")
    for off in (OFF_TXCNT, OFF_RXCNT, OFF_STAT, OFF_BYTE):
        a.rr("mov", "r13", "r14"); a.addi(off, "r14"); a.rr("mov.lr@", "r0", "r14")

    # ---- 総当たりの表を RAM に作る（展開するとコードが膨らんで分岐が届かない）
    a.movl_imm(RAM + OFF_TAB, "r13")
    for i, (ve, vc) in enumerate(ST):
        a.movi(ve if ve < 0x80 else ve - 0x100, "r0")
        a.rr("mov", "r13", "r14"); a.addi(2 * i, "r14"); a.rr("mov.br@", "r0", "r14")
        a.movi(vc if vc < 0x80 else vc - 0x100, "r0")
        a.rr("mov", "r13", "r14"); a.addi(2 * i + 1, "r14"); a.rr("mov.br@", "r0", "r14")

    # ---- 割り込みを最初から許可しておく（起きた瞬間に分かるように）
    a.movl_imm(IPRA, "r13")
    a.rr("mov.w@r", "r13", "r0"); a.rr("extu.w", "r0", "r0")
    a.imm_r0("or", IPRA_ON)
    a.rr("mov.wr@", "r0", "r13")
    a.stc_sr("r0"); a.movl_imm(0xFFFFFF0F, "r7"); a.rr("and", "r7", "r0")
    a.rn("ldc.sr", "r0")

    a.movl_imm(M.SCI0 + 4, "r1")
    a.movl_imm(USB_STAT, "r2")
    a.br("bra", "ready"); a.nop()
    a.flush_pool()

    a.label("ready")
    for b in (0xF0, 0x43, 0x7D, 0x52, 0x44, 0x59, 0xF7):
        send_byte(b)

    # ---- 総当たり本体（表を引くループ。r9 = 0xE00000 の値, r10 = 0xC80000 の値）
    a.movi(0, "r5")                                            # 段階
    a.label("outer")
    a.movl_imm(RAM + OFF_TAB, "r13")
    a.rr("mov", "r5", "r0"); a.rn("shll", "r0"); a.rr("add", "r0", "r13")
    a.rr("mov.b@r", "r13", "r9"); a.rr("extu.b", "r9", "r9")
    a.addi(1, "r13")
    a.rr("mov.b@r", "r13", "r10"); a.rr("extu.b", "r10", "r10")
    a.movl_imm(LATCH_E, "r13"); a.rr("mov.br@", "r9", "r13")   # ラッチに書く
    a.movl_imm(LATCH_C, "r13"); a.rr("mov.br@", "r10", "r13")

    a.movi(repeats, "r6")
    a.label("rep")
    for b in (0xF0, 0x43, 0x7D, 0x4C):                         # 'L'
        send_byte(b)
    send16("r5"); send16("r9"); send16("r10")
    a.rr("mov.b@r", "r2", "r3"); a.rr("extu.b", "r3", "r3"); send16("r3")
    a.movl_imm(RAM + OFF_TXCNT, "r13"); a.rr("mov.l@r", "r13", "r3"); send16("r3")
    a.movl_imm(RAM + OFF_RXCNT, "r13"); a.rr("mov.l@r", "r13", "r3"); send16("r3")
    send_byte(0xF7)
    a.movl_imm(inner, "r14")
    a.label("wait")
    a.movi(gap, "r3")
    a.label("gapw"); a.rn("dt", "r3"); a.br("bf", "gapw")
    a.rn("dt", "r14"); a.br("bf", "wait")
    a.rn("dt", "r6"); a.long_br("bf", "rep")   # 報告が長いので bra を挟む

    a.addi(1, "r5")                                            # 次の段階へ
    a.movi(len(ST), "r7")
    a.rr("cmp/hs", "r7", "r5")
    a.br("bf", "nowrap")
    a.movi(0, "r5")
    a.label("nowrap")
    a.br("bra", "outer"); a.nop()
    a.flush_pool()

    # ---- send
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

    # ================= 割り込みハンドラ（R0-R7 のみ使用）=================
    a.label("irq_null")
    a.rts(); a.nop()

    a.label("irq_tx")
    a.movl_imm(RAM, "r1")
    a.rr("mov.l@r", "r1", "r0"); a.addi(1, "r0"); a.rr("mov.lr@", "r0", "r1")
    a.movl_imm(IPRA, "r5")                                     # 送るものは無いので止める
    a.rr("mov.w@r", "r5", "r0"); a.rr("extu.w", "r0", "r0")
    a.movl_imm(IPRA_TX_OFF, "r6"); a.rr("and", "r6", "r0")
    a.rr("mov.wr@", "r0", "r5")
    a.rts(); a.nop()
    a.flush_pool()

    a.label("irq_rx")
    a.movl_imm(RAM, "r1")
    a.rr("mov", "r1", "r2"); a.addi(OFF_RXCNT, "r2")
    a.rr("mov.l@r", "r2", "r0"); a.addi(1, "r0"); a.rr("mov.lr@", "r0", "r2")
    a.movl_imm(USB_STAT, "r3")
    a.rr("mov.b@r", "r3", "r0"); a.rr("extu.b", "r0", "r0")
    a.rr("mov", "r1", "r4"); a.addi(OFF_STAT, "r4"); a.rr("mov.lr@", "r0", "r4")
    a.imm_r0("and", 0x01)
    a.rr("tst", "r0", "r0")
    a.br("bt", "rx_end")
    a.movl_imm(USB_DATA, "r6")
    a.rr("mov.b@r", "r6", "r0"); a.rr("extu.b", "r0", "r0")
    a.rr("mov", "r1", "r7"); a.addi(OFF_BYTE, "r7"); a.rr("mov.lr@", "r0", "r7")
    a.label("rx_end")
    a.rts(); a.nop()
    a.flush_pool()

    code = a.assemble()
    LAST_LABELS.clear(); LAST_LABELS.update(a.labels)
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/usbprobe.bin"))
    ap.add_argument("--inner", type=lambda s: int(s, 0), default=INNER)
    ap.add_argument("--repeats", type=int, default=REPEATS)
    ap.add_argument("--gap", type=int, default=GAP)
    a = ap.parse_args()
    code = build(inner=a.inner, repeats=a.repeats, gap=a.gap)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    n = len(steps())
    # 実測: inner=150000, gap=50 で 1 報告あたり約 2.7 秒（見積もりより遅い）
    per = a.repeats * 2.7 * (a.inner / INNER) * (a.gap / GAP)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")
    print(f"{n} 段階 x 約 {per:.0f} 秒 = 一巡 約 {n * per / 60:.1f} 分")
    print(f"  -> python build/usbmon.py --seconds {int(n * per * 1.15)}")
    for i, (ve, vc) in enumerate(steps()):
        print(f"  段階 {i:2}: 0xE00000={ve:#04x}  0xC80000={vc:#04x}")


if __name__ == "__main__":
    sys.exit(main())
