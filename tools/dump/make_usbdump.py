#!/usr/bin/env python3
# license:BSD-3-Clause
"""純正ファームを動かしたまま、USB 経由で波形 ROM を吸い出すファームウェア。

## 仕組み

`make_usbhook.py` と同じ土台。差し込む口は 2 つで、

  - **呼び水**: メインループのリテラル 0x040CE0（起動時に 1 回だけ効く）
  - **本番**: 送信割り込みハンドラ 0x0400D8（1 バイト送るたびに呼ばれる）

どちらも同じ `fill` を呼ぶ。`fill` は送信リングに 1 メッセージ分の空きが
あるときだけ動く。

  1. 作業領域から次のブロック番号を取る（壊れていたら 0 に戻す）
  2. SWP30 の wave direct access で 16 ワード読む
  3. SysEx を組み立てる
       F0 43 7D 'M' 'U' 'W' 'D' <ブロック番号 4x7bit> <16 ワード x 5x7bit> <加算和 2x7bit> F7
       = 94 バイト（純正の送信リングは 128 バイトなので収まる）
  4. 0x000433A0 に渡す（純正のドライバが USB へ流す）
  5. ブロック番号を進める

MIDI 版と同じ SysEx 形式なので、`tools/dump/recv_dump.py --words-per-block 16` で受けられる。
再送要求は効かない（MIDI IN を見ていない）。取りこぼしは次の一巡で埋まる。

## 空になるのを待ってはいけない

純正の送信ハンドラは**リングが空になった時点で送信割り込みを止める**。
止まったあとに積み直しても再開する保証がないので、判定は「空か」ではなく
「1 メッセージ分の空きがあるか」にしてある（`make_usbhook.py` の説明を参照）。

## 割り込みは止めない

SWP30 は純正ファームも使っている（0x11C1E4, 0x1366A4, 0x136770）。読み出しの
最中に取り合いになると値が化ける可能性があるが、**各ブロックに加算和が付いている
ので受信側が捨てる**。止めると音声処理を邪魔するので、化けは検出に任せる。
純正側を巻き添えにする恐れがあるので、**吸っている間はサンプリング系の操作をしない**こと。

## 作業領域

内蔵 RAM の 0xFFFFF000。純正ファーム全体のリテラルを解決して数えたところ、
0xFFFFF000-0xFFFFF7FF を指すのは 0xFFFFF170 だけだった。確証は無いので
**先頭に合言葉を置いて毎回検査**し、壊れていたらブロック 0 から入れ直す。
受信側は順不同・重複に強いので、それで実害は出ない。
外していた場合は**ブロック 0 だけが延々と届く**ので、すぐ分かる。

## レジスタの約束

メインループからは JSR、割り込みからはダウンローダのスタブ経由で呼ばれる。
どちらも **R8-R15 は壊さない**。R0-R7 だけ使い、PR は入口で退避して戻す。
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm
import make_dumper as M

ENTRY = M.ENTRY

INIT_BSR = (0x00040192, 0x00040150)
INIT_CALLS = (0x001159F0, 0x00115F62, 0x00115AAC, 0x001155FC, 0x001155C8,
              0x0011658A, 0x00116098, 0x000C7694, 0x0011A4F0)
SR_MASK = 0xFF0F
MAIN_LOOP = 0x00040C5C

# 純正が USB へ MIDI を出す低レベル API。0x433A0 は M37640 の**コマンド口**なので使わない。
MIDI_TX_BYTE = 0x00043782   # (R4 = 1 バイト) 待たずに積んで IPRA を立てる
# 0x000437A0 は同じ処理の「空きが出るまで待つ」版。割り込み文脈では使えない
# （満杯だと、リングを減らす送信割り込みが同じ優先度 12 で走れず永久に固まる）
TX_RD = 0x00409416          # 読み位置（word）
TX_WR = 0x00409418          # 書き位置（word）
RING_MASK = 0x3FF           # リングは 0x0040941A の 1024 バイト
MARGIN = 128                # 満杯まで詰めない余裕
HOOKED = 0x00041E50
USB_RX_ISR = 0x00043630    # 純正の受信割り込みハンドラ（IRQ3）

SCRATCH = 0xFFFFF000       # 内蔵 RAM。純正が使っていなさそうな低い側
MAGIC = 0x4D555744         # 'MUWD'
OFF_MAGIC, OFF_BLOCK, OFF_CALLS, OFF_BUF = 0x00, 0x04, 0x08, 0x10

WORDS = 16                 # 1 メッセージのワード数。94 バイトに収まる
ROM_WORDS = 0x800000

# メインループのリテラル（呼び水）と、IRQ2 のハンドラ番地（本番）の両方を差し替える。
# メインループのリテラル 0x040CE0 は使わない（起動直後に走って初期化と競合する）。
# **送信割り込み（0x0400D8）は絶対にフックしないこと。**
# 積む -> IPRA が立つ -> 送信割り込み -> また積む、で永久に回り、割り込み優先度のまま
# CPU を占有してメインのタスクが動けなくなる（実機で起動バナーの後に沈黙した）。
# 受信割り込みだけにすれば、PC が 1 バイト送ったとき 1 ブロック返す受動的な動きになる。
HEADER_PATCH = {0x000400DC: "rxisr"}
LAST_LABELS = {}


def build(words=WORDS, rom_words=ROM_WORDS, count_calls=False, kickstart=False):
    a = Asm(ENTRY)
    msg_len = 7 + 4 + words * 5 + 2 + 1
    if msg_len > RING_MASK - MARGIN:
        raise ValueError(f"1 メッセージ {msg_len} バイトは送信リング"
                         f"({RING_MASK + 1} バイト, 余裕 {MARGIN}) に収まらない")
    # 作業領域は 0xFFFFF000-0xFFFFF16F しか空いていない（0xFFFFF170 から先は純正が使う）
    if SCRATCH + OFF_BUF + msg_len > 0xFFFFF170:
        raise ValueError(f"バッファ {msg_len} バイトが作業領域に収まらない"
                         f"（{SCRATCH + OFF_BUF:#x} から 0xFFFFF170 まで）")

    def loadi(v, rn):
        """8bit 即値に入らない値も積めるようにする。"""
        if -128 <= v <= 127:
            a.movi(v, rn)
        else:
            a.movl_imm(v, rn)

    def call(addr):
        a.movl_imm(addr, "r3")
        a.rn("jsr@", "r3"); a.nop()

    def putb_imm(v):
        """即値 1 バイトをバッファ（r6）へ。"""
        a.movi(v & 0xFF if v < 0x80 else v - 0x100, "r0")
        a.rr("mov.br@", "r0", "r6"); a.addi(1, "r6")

    def putb_r0_sum():
        """r0 の下位 7bit をバッファへ入れ、加算和(r5)に足す。"""
        a.imm_r0("and", 0x7F)
        a.rr("mov.br@", "r0", "r6"); a.addi(1, "r6")
        a.rr("add", "r0", "r5")

    def shr(reg, n):
        for amt, op in ((16, "shlr16"), (8, "shlr8"), (2, "shlr2"), (1, "shlr")):
            while n >= amt:
                a.rn(op, reg); n -= amt

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

    # ================= 検証用の呼び水（MAME 専用。実機には焼かない） =================
    # MAME は M37640 をエミュレートしないので割り込みが一度も上がらず、fill が動かない。
    # メッセージの中身を確かめたいときだけ、メインループのリテラルから 1 回呼ばせる。
    # **実機ではこれを有効にしないこと。** 起動直後の制御できないタイミングで走る。
    if kickstart:
        a.label("hook")
        a.sts_pr_push()
        a.br("bsr", "fill"); a.nop()
        a.lds_pr_pop()
        a.movl_imm(0x00041E50, "r3")
        a.rn("jmp@", "r3"); a.nop()
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

    # ================= 1 ブロック読んで積む =================
    # BSR で呼ばれる。PR を退避してから 0x433A0 を JSR する。R0-R7 だけ使う。
    a.label("fill")
    a.sts_pr_push()

    # --- 呼ばれた回数を数える（MAME での診断用。実機では読めない）
    if count_calls:
        a.movl_imm(SCRATCH + OFF_CALLS, "r1")
        a.rr("mov.l@r", "r1", "r2"); a.addi(1, "r2"); a.rr("mov.lr@", "r2", "r1")

    # --- 送信リングの空きが 1 メッセージ分なければ何もしない
    # 0x437A0 は空きが出るまでスピンするので、満杯のまま割り込み文脈で呼ぶと固まる。
    a.movl_imm(TX_RD, "r1"); a.rr("mov.w@r", "r1", "r2"); a.rr("extu.w", "r2", "r2")
    a.movl_imm(TX_WR, "r1"); a.rr("mov.w@r", "r1", "r3"); a.rr("extu.w", "r3", "r3")
    a.rr("sub", "r2", "r3")            # r3 = 書き位置 - 読み位置
    a.movl_imm(RING_MASK, "r1")
    a.rr("mov", "r3", "r0"); a.rr("and", "r1", "r0")    # r0 = 溜まっているバイト数
    a.movl_imm(RING_MASK - msg_len - MARGIN, "r2")
    a.rr("cmp/hi", "r2", "r0")         # 溜まりすぎ = 空きが足りない
    a.long_br("bt", "done")            # done は遠いので bra を挟む

    # --- 作業領域の合言葉を確認する
    a.movl_imm(SCRATCH, "r1")
    a.rr("mov.l@r", "r1", "r2")
    a.movl_imm(MAGIC, "r3")
    a.rr("cmp/eq", "r3", "r2")
    a.br("bt", "haveblk")
    a.rr("mov.lr@", "r3", "r1")                       # 合言葉を書く
    a.movi(0, "r0")
    a.rr("mov", "r1", "r2"); a.addi(OFF_BLOCK, "r2"); a.rr("mov.lr@", "r0", "r2")
    a.label("haveblk")
    a.rr("mov", "r1", "r2"); a.addi(OFF_BLOCK, "r2")
    a.rr("mov.l@r", "r2", "r7")                       # r7 = ブロック番号

    # --- ヘッダを組み立てる
    a.rr("mov", "r1", "r6"); a.addi(OFF_BUF, "r6")    # r6 = バッファ
    for b in (0xF0, 0x43, 0x7D, 0x4D, 0x55, 0x57, 0x44):
        putb_imm(b)
    a.movi(0, "r5")                                   # 加算和
    for sh in (21, 14, 7, 0):
        a.rr("mov", "r7", "r0"); shr("r0", sh); putb_r0_sum()

    # --- ワード番地 = ブロック番号 * words
    a.rr("mov", "r7", "r0")
    n = words.bit_length() - 1
    for amt, op in ((16, "shll16"), (8, "shll8"), (2, "shll2"), (1, "shll")):
        while n >= amt:
            a.rn(op, "r0"); n -= amt
    a.rr("mov", "r0", "r7")                           # r7 = 先頭ワード番地
    loadi(words, "r4")                                # r4 = 残りワード数
    a.br("bra", "wordloop"); a.nop()
    a.flush_pool()

    # --- 1 ワード読んで 5 個の 7bit に分ける
    a.label("wordloop")
    a.movl_imm(M.SWP_WAVE_ADR_H, "r1")
    a.rr("mov", "r7", "r0"); a.rn("shlr16", "r0"); a.rr("mov.wr@", "r0", "r1")
    a.rr("mov", "r1", "r2"); a.addi(2, "r2")
    a.rr("mov", "r7", "r0"); a.rr("mov.wr@", "r0", "r2")
    a.movl_imm(M.SWP_WAVE_SIZE_H, "r1")
    a.movi(0, "r0"); a.rr("mov.wr@", "r0", "r1")
    a.rr("mov", "r1", "r2"); a.addi(2, "r2")
    a.movi(1, "r0"); a.rr("mov.wr@", "r0", "r2")
    a.movl_imm(M.SWP_WAVE_TRIG, "r1")
    a.movi(-0x80, "r0"); a.rn("shll8", "r0"); a.rr("mov.wr@", "r0", "r1")
    # 検証済みの MIDI 版と同じ固定待ち。トリガ直後はステータスがまだ立っておらず、
    # これが無いと poll が即抜けて古いデータを読む
    a.movi(40, "r0")
    a.label("trigwait"); a.rn("dt", "r0"); a.br("bf", "trigwait")
    a.rr("mov", "r1", "r2"); a.addi(2, "r2")          # ステータス
    a.movl_imm(400, "r3")
    a.label("poll")
    a.rr("mov.w@r", "r2", "r0"); a.rr("extu.w", "r0", "r0")
    a.rr("tst", "r0", "r0"); a.br("bf", "polldone")
    a.rn("dt", "r3"); a.br("bf", "poll")
    a.label("polldone")
    a.movl_imm(M.SWP_WAVE_DAT_H, "r1")
    a.rr("mov.w@r", "r1", "r2"); a.rr("extu.w", "r2", "r2")
    a.rr("mov", "r1", "r0"); a.addi(2, "r0")
    a.rr("mov.w@r", "r0", "r3"); a.rr("extu.w", "r3", "r3")
    a.rn("shll16", "r2"); a.rr("or", "r3", "r2")      # r2 = 32bit データ
    for sh in (28, 21, 14, 7, 0):
        a.rr("mov", "r2", "r0"); shr("r0", sh); putb_r0_sum()
    a.addi(1, "r7")
    a.rn("dt", "r4"); a.long_br("bf", "wordloop")

    # --- 加算和と F7
    a.rr("mov", "r5", "r2")
    a.rr("mov", "r2", "r0"); shr("r0", 7); a.imm_r0("and", 0x7F)
    a.rr("mov.br@", "r0", "r6"); a.addi(1, "r6")
    a.rr("mov", "r2", "r0"); a.imm_r0("and", 0x7F)
    a.rr("mov.br@", "r0", "r6"); a.addi(1, "r6")
    putb_imm(0xF7)

    # --- ブロック番号を進める
    a.movl_imm(SCRATCH + OFF_BLOCK, "r1")
    a.rr("mov.l@r", "r1", "r2"); a.addi(1, "r2")
    a.movl_imm(rom_words // words, "r3")
    a.rr("cmp/hs", "r3", "r2")
    a.br("bf", "nowrap")
    a.movi(0, "r2")
    a.label("nowrap")
    a.rr("mov.lr@", "r2", "r1")

    # --- 1 バイトずつ純正の API に渡す。0x437A0 は R0-R7 を壊すので r6/r7 は退避する
    a.movl_imm(SCRATCH + OFF_BUF, "r6")
    loadi(msg_len, "r7")
    a.label("txbyte")
    a.rr("mov.b@r+", "r6", "r4"); a.rr("extu.b", "r4", "r4")
    a.rr("mov.lr@-", "r6", "r15")
    a.rr("mov.lr@-", "r7", "r15")
    a.movl_imm(MIDI_TX_BYTE, "r3")
    a.rn("jsr@", "r3"); a.nop()
    a.rr("mov.l@r+", "r15", "r7")
    a.rr("mov.l@r+", "r15", "r6")
    a.rn("dt", "r7"); a.br("bf", "txbyte")

    a.label("done")
    a.lds_pr_pop()
    a.rts(); a.nop()
    a.flush_pool()

    code = a.assemble()
    LAST_LABELS.clear(); LAST_LABELS.update(a.labels)
    return code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", type=Path, default=Path("build/usbdump.bin"))
    ap.add_argument("--words", type=int, default=WORDS, help="1 メッセージのワード数")
    ap.add_argument("--rom-words", type=lambda s: int(s, 0), default=ROM_WORDS)
    ap.add_argument("--scratch", type=lambda s: int(s, 0), default=SCRATCH,
                    help="作業領域（内蔵 RAM）の先頭。既定 0xFFFFF000")
    ap.add_argument("--count-calls", action="store_true",
                    help="フックが呼ばれた回数を作業領域 +8 に数える（MAME 診断用）")
    a = ap.parse_args()
    globals()["SCRATCH"] = a.scratch
    code = build(words=a.words, rom_words=a.rom_words, count_calls=a.count_calls)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    n = a.rom_words // a.words
    ml = 7 + 4 + a.words * 5 + 2 + 1
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")
    print(f"1 メッセージ {a.words} ワード = {ml} バイト、全 {n:,} メッセージ")
    print(f"USB へ流す総量 {n * ml / 1e6:.1f} MB（MIDI 版は 42.9 MB）")


if __name__ == "__main__":
    sys.exit(main())
