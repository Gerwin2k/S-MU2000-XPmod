#!/usr/bin/env python3
# license:BSD-3-Clause
"""純正ファームを丸ごと動かしたまま、USB へデータを流し込むフック版。

## 仕組み

差し込む口は 2 つ。

### 1. 呼び水: メインループのリテラル 0x040CE0

純正の 0x040C5C は RTOS のスケジューラで、呼び先が**リテラル**から読まれている。

    0x040CD0 -> 0x00041E94
    0x040CD4 -> 0x00041AAC
    0x040CD8 -> 0x00041A5C
    0x040CDC -> 0x00041B0C
    0x040CE0 -> 0x00041E50     ← ここを自前の関数に差し替える

4 バイト書き換えるだけで差し込める。ただし **MAME で数えたら起動時に 1 回しか
呼ばれなかった**（0x040C5C はタスク切り替えの入口であって、毎周回まわる
ループではない）。なのでここは**最初の 1 個を積む呼び水**として使う。

### 2. 本番: 送信割り込みハンドラ 0x0400D8

ダウンローダは本体ファームのヘッダ 0x0400D8 を RAM 0x401044 に写し、
IRQ2（USB 送信完了）でそこを JSR する。ここを自前のハンドラに差し替え、

    純正のハンドラ 0x00043486 を通す -> 空きがあれば次を積む

とすれば、**1 バイト送り終わるたびに呼ばれる**。メインループの回り方に
一切依存しない。IRQ2 のスタブは R0-R7 と PR を退避してから JSR してくるので、
自前のハンドラは R0-R7 を自由に使える（R8-R15 は触らない）。

## USB への送信

純正のドライバに任せる。解析で分かった API:

    0x000433A0(R4 = バイト列のポインタ, R5 = バイト数)
        割り込みを止めて送信リング(0x40A846, 128 バイト)に積み、
        書き位置 0x43DACE を更新して IPRA |= 0xC0 で送信割り込みを許可する
        **空き容量は一切見ない。** 溢れの判定は呼ぶ側の責任。

## 空になるのを待ってはいけない

読み位置 0x43DACD と書き位置 0x43DACE が同じなら空だが、**空になった時点で
純正の送信ハンドラは送信割り込みを止める**（0x434FC 以降）。止まったあとに
積み直しても、IRQ が立ち上がりで拾われる設定なら二度と再開しない。

なので判定は「空か」ではなく「**1 メッセージ分の空きがあるか**」にする。

    溜まり = (書き位置 - 読み位置) & 0x7F
    溜まり <= 127 - メッセージ長  なら積む

こうすればリングが空になる瞬間が無く、送信割り込みの連鎖が途切れない。

## この版の目的

一定長のメッセージを積み続ける。確かめたいのは 3 つ。

  1. 送信割り込みのフックが本当に回り続けるか
  2. 0x433A0 経由で USB へ流れるか
  3. **USB の実効速度はどれくらいか**（PC 側で byte/s を測る）

`build/usbrecv.py` で受ける。**1 個しか届かなければ呼び水だけが効いて
送信割り込みのフックが回っていない**、という切り分けになる。
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY

# 純正 0x040100 が順に呼ぶもの
INIT_BSR = (0x00040192, 0x00040150)
INIT_CALLS = (0x001159F0, 0x00115F62, 0x00115AAC, 0x001155FC, 0x001155C8,
              0x0011658A, 0x00116098, 0x000C7694, 0x0011A4F0)
SR_MASK = 0xFF0F
MAIN_LOOP = 0x00040C5C

# 純正が USB へ MIDI を出すのに使っている低レベル API。**待たない版**を使う。
#   0x00043782(R4 = 1 バイト)
#     ring[書き位置] = R4、書き位置を進めて IPRA |= 0xC0。待ちも割り込み禁止も無い
#   0x000437A0 は同じことを割り込みを止めて行い、**空きが出るまでスピンで待つ**。
#     待ちがあるので割り込み文脈では使えない。満杯だと、リングを減らす送信割り込みが
#     同じ優先度 12 で走れず永久に固まる（実機で本体が起動しなくなった）
MIDI_TX_BYTE = 0x00043782
TX_RD = 0x00409416          # 読み位置（word）
TX_WR = 0x00409418          # 書き位置（word）
RING_MASK = 0x3FF           # リングは 0x0040941A の 1024 バイト
MARGIN = 128                # 満杯まで詰めない余裕。off-by-one で満杯にすると危ない
USB_RX_ISR = 0x00043630     # 純正の受信割り込みハンドラ（IRQ3）

# メインループのリテラル（呼び水）と、送信割り込みハンドラ（本番）の両方を差し替える。
# 0x0400D8 はダウンローダが 0x401044 に写す IRQ2 のハンドラ番地。
# メインループのリテラル 0x040CE0 は**使わない**。あれは起動直後の制御できない
# タイミングで一度だけ走るので、そこで積むと本体の初期化と競合する。
# 起こし役は受信割り込みに任せ、PC がバイトを送った時だけ動かす。
# **送信割り込み（0x0400D8）は絶対にフックしないこと。**
# 積む -> IPRA が立つ -> 送信割り込み -> また積む、で永久に回り、割り込み優先度のまま
# CPU を占有してメインのタスクが動けなくなる（実機で起動バナーの後に沈黙した）。
# 受信割り込みだけにすれば、PC が 1 バイト送ったとき 1 ブロック返す受動的な動きになる。
HEADER_PATCH = {0x000400DC: "rxisr"}
LAST_LABELS = {}

# 送るもの。F0 43 7D 'M' 'U' 'H' 'K' <連番の代わりの詰め物> F7
PAYLOAD = 56


def build(payload=PAYLOAD):
    a = Asm(ENTRY)
    msg = bytes([0xF0, 0x43, 0x7D, 0x4D, 0x55, 0x48, 0x4B]
                + [i & 0x7F for i in range(payload)] + [0xF7])

    def call(addr):
        a.movl_imm(addr, "r3")
        a.rn("jsr@", "r3"); a.nop()

    # ================= 入口: 純正の初期化をそのまま走らせる =================
    a.stc_sr("r0"); a.movl_imm(SR_MASK, "r3"); a.rr("and", "r3", "r0")
    a.imm_r0("or", 0xF0); a.rn("ldc.sr", "r0")

    call(INIT_BSR[0])
    call(INIT_CALLS[0])
    call(INIT_BSR[1])
    a.movl_imm(INIT_CALLS[1], "r3"); a.movi(1, "r4")
    a.rn("jsr@", "r3"); a.nop()
    for adr in INIT_CALLS[2:]:
        call(adr)
    a.movl_imm(MAIN_LOOP, "r3")
    a.rn("jmp@", "r3"); a.nop()        # あとは純正のメインループに任せる
    a.flush_pool()

    # ================= 受信割り込みのフック（唯一の入口） =================
    # IPRA の IRQ2 は「誰かが積んだとき」に初めて立つので、一度も積めていないと
    # 送信割り込みは永久に上がらない。受信は実機で動いていることを確認済みなので、
    # **PC から何かバイトを送れば必ずここに来る**。ここで 1 個積めば連鎖が始まる。
    # 連鎖が切れても、また送れば必ず再開できる。
    # IRQ2 と IRQ3 は同じ優先度 12 なので、fill が互いに割り込まれることはない。
    a.label("rxisr")
    a.sts_pr_push()
    call(USB_RX_ISR)
    a.br("bsr", "fill"); a.nop()
    a.lds_pr_pop()
    a.rts(); a.nop()
    a.flush_pool()

    # ================= 送信リングが空なら 1 個積む =================
    # BSR で呼ばれる。PR を退避してから 0x433A0 を JSR する。
    a.label("fill")
    a.sts_pr_push()
    # 溜まり = (書き位置 - 読み位置) & 0x3FF。1 メッセージ分の空きが無ければ何もしない。
    # 0x437A0 は空き待ちでスピンするので、満杯で呼ぶと割り込み文脈で固まる。
    a.movl_imm(TX_RD, "r1"); a.rr("mov.w@r", "r1", "r2"); a.rr("extu.w", "r2", "r2")
    a.movl_imm(TX_WR, "r1"); a.rr("mov.w@r", "r1", "r3"); a.rr("extu.w", "r3", "r3")
    a.rr("sub", "r2", "r3")            # r3 = 書き位置 - 読み位置
    a.movl_imm(RING_MASK, "r1")
    a.rr("mov", "r3", "r0"); a.rr("and", "r1", "r0")    # r0 = 溜まっているバイト数
    a.movl_imm(RING_MASK - len(msg) - MARGIN, "r2")
    a.rr("cmp/hi", "r2", "r0")         # 溜まりすぎ = 空きが足りない
    a.br("bt", "busy")

    # --- 1 バイトずつ純正の API に渡す。0x437A0 は R0-R7 を壊すので r6/r7 は退避する
    a.movl_imm("msg", "r6")
    a.movi(len(msg), "r7")
    a.br("bra", "txbyte"); a.nop()
    a.flush_pool()
    a.label("txbyte")
    a.rr("mov.b@r+", "r6", "r4"); a.rr("extu.b", "r4", "r4")
    a.rr("mov.lr@-", "r6", "r15")
    a.rr("mov.lr@-", "r7", "r15")
    a.movl_imm(MIDI_TX_BYTE, "r3")
    a.rn("jsr@", "r3"); a.nop()
    a.rr("mov.l@r+", "r15", "r7")
    a.rr("mov.l@r+", "r15", "r6")
    a.rn("dt", "r7"); a.br("bf", "txbyte")

    a.label("busy")
    a.lds_pr_pop()
    a.rts(); a.nop()
    a.flush_pool()
    a.label("msg")
    a.data(msg)

    code = a.assemble()
    LAST_LABELS.clear(); LAST_LABELS.update(a.labels)
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/usbhook.bin"))
    ap.add_argument("--payload", type=int, default=PAYLOAD)
    a = ap.parse_args()
    code = build(payload=a.payload)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")
    for off, name in sorted(HEADER_PATCH.items()):
        print(f"  {off:#08x} -> {name}")


if __name__ == "__main__":
    sys.exit(main())
