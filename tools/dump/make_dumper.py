#!/usr/bin/env python3
# license:BSD-3-Clause
"""MU2000 の波形 ROM を MIDI OUT から吸い出す自作ファームウェアを生成する。

本体ファームウェア領域の先頭（エントリ 0x00040100）に置く SH-2 コード。
ダウンローダ（0x000000-0x00C001）には一切触れないので、失敗しても
[Drum]+[PLAY]+[VALUE+] 起動 → 純正アップデータで復旧できる。

動作:
  1. SH7043 の SCI0 を 31250bps に設定（MIDI OUT = SCI0 TX, MIDI IN A = SCI0 RX）
  2. SWP30 を起動時初期化してから wave direct access で波形 ROM を 32bit ずつ読む
  3. 1 ブロック 128 ワード（512 byte）を SysEx 1 個で送出。読み終わったら先頭から繰り返す

送出 SysEx（MU2000 -> PC）:
  F0 43 7D 'M' 'U' 'W' 'D' <ブロック番号 4x7bit> <128 ワード x 5x7bit> <加算和 2x7bit> F7
  1 ワードは 32bit を 5 個の 7bit に分割（上位から 4,7,7,7,7 bit）
  加算和はブロック番号の 4 バイトとペイロードの 640 バイトを足して下位 14bit を取ったもの。
  受信側は不一致のブロックを捨て、再送を要求する。

再送要求 SysEx（PC -> MU2000）:
  F0 43 7D <ブロック番号 4x7bit> F7
  送出中のブロックを送り終えた時点で、そのブロックへ飛んで続きを送り直す。
  受信は既定で SCI0（MIDI IN A）。--rx-sci 1 にすると SCI1（MIDI IN B）で受ける。
  状態機械が 1 組しかないので両方を同時には見ない（同じ要求が 2 系統から入ると壊れる）。
  取りこぼさないよう、1 バイト送出待ちのスピンループの中で毎回ポーリングしている
  （バイト間隔 320us に対しポーリング間隔は約 3us）。
  受信エラー（ORER/FER/PER）は RDRF が立っていなくても毎回落とす。落とさないと
  SCI の受信が止まったままになる。

ピン機能（PFC）と PA21（MIDI A の前面/背面切替）はダウンローダが設定済みなので、
こちらは SCR に TE/RE を立てるだけでよい。念のため PA21 はダウンローダと同じ値を書き直す。

レジスタ割り当て:
  r0  作業        r1  SCI0 SSR      r2  波形アドレス上位  r3  データワード
  r4  加算和      r5  ワード番地    r6  ブロック番号      r7  ワード数カウンタ
  r8  トリガ      r9  受信状態      r10 サイズ上位        r11 データ上位
  r12 受信中のブロック番号         r13 送出バイト/作業    r14 作業
  r15 保留中の再送要求（0 = なし、ブロック番号+1）
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sh2asm import Asm

ENTRY = 0x00040100
SCI0 = 0xFFFF81A0          # SMR, +1 BRR, +2 SCR, +3 TDR, +4 SSR, +5 RDR
SCI1 = 0xFFFF81B0          # MIDI IN B
BRR_31250 = 27             # 28MHz, CKS=0 -> 28e6/(32*31250)-1
PADR_H = 0xFFFF8380        # ポート A データ 上位 16bit（PA31-PA16）
PAIOR_H = 0xFFFF8384       # ポート A 入出力 上位 16bit
PA21 = 0x0020              # PA21 = MIDI A の前面/背面切替。ダウンローダは出力・1 にする
SWP_WAVE_ADR_H = 0x80011C  # reg 2 ch14 : ((2<<6)|14)*2 + 0x800000
SWP_WAVE_TRIG = 0x80021C   # reg 4 ch14（+2 が ch15 = ステータス）
SWP_WAVE_DAT_H = 0x80029C  # reg 5 ch14
SWP_CTRL0 = 0x80001C       # reg0 ch14 : 起動時に 0x9100、ch15 に 0xC002 -> 0xC003
SWP_CTRL_D = 0x80069C      # reg0x0D ch14 : 起動時に 0x1100 -> 0x0040
SWP_WAVE_SIZE_H = 0x80019C # reg 3 ch14
WORDS_PER_BLOCK = 128      # SysEx 1 個 = 654 byte（Windows MIDI の 1024 byte 制限内）
ROM_WORDS = 0x800000       # 32MB / 4

RX_IDLE = 0                # 受信状態機械。1='43' 待ち, 2='7D' 待ち, 3-6=7bit 積み, 7=F7 待ち
                           # 成立した要求は r15 に置く。状態機械とは切り離す（次の F0 で消えないように）


def build(rom_words=ROM_WORDS, words_per_block=WORDS_PER_BLOCK, first_block=0, rx_sci=0):
    if words_per_block & (words_per_block - 1):
        raise ValueError("1 ブロックのワード数は 2 の冪にすること")
    log2_wpb = words_per_block.bit_length() - 1
    a = Asm(ENTRY)

    def shl(rn, n):
        for amt, op in ((16, "shll16"), (8, "shll8"), (2, "shll2"), (1, "shll")):
            while n >= amt:
                a.rn(op, rn); n -= amt

    def septet(src, shift):
        a.rr("mov", src, "r0")
        for n, op in ((16, "shlr16"), (8, "shlr8"), (2, "shlr2"), (1, "shlr")):
            while shift >= n:
                a.rn(op, "r0"); shift -= n
        a.imm_r0("and", 0x7F)
        a.br("bsr", "send"); a.nop()

    def send_byte(v):
        a.movi(v & 0xFF if v < 0x80 else v - 0x100, "r0")
        a.br("bsr", "send"); a.nop()

    def report16(reg):
        """reg が指すアドレスの 16bit レジスタを読み、3 個の 7bit に分けて送る。"""
        a.rr("mov.w@r", reg, "r3"); a.rr("extu.w", "r3", "r3")
        for sh in (14, 7, 0):
            septet("r3", sh)

    def status_addr(reg):
        """波形ステータス（トリガ +2）のアドレスを reg に作る。"""
        a.rr("mov", "r8", reg); a.addi(2, reg)

    def trigger():
        a.movi(-0x80, "r0"); a.rn("shll8", "r0")     # 0xFFFF8000 -> 下位 16bit = 0x8000
        a.rr("mov.wr@", "r0", "r8")

    def w16(addr_reg, value_reg):
        a.rr("mov.wr@", value_reg, addr_reg)

    def delay(n, label):
        if -128 <= n <= 127:
            a.movi(n, "r14")
        else:
            a.movl_imm(n, "r14")
        a.label(label); a.rn("dt", "r14"); a.br("bf", label)

    def rxpoll(tag, offset):
        """SCI の受信を 1 バイト分だけ処理する。r0, r14 を破壊し、r9/r12/r15 を更新する。

        r9 = 状態, r12 = 積み上げ中のブロック番号, r15 = 成立した要求（ブロック+1）。
        """
        def L(s):
            return f"rx{tag}_{s}"

        a.rr("mov", "r1", "r14")
        if offset:
            a.addi(offset, "r14")
        a.rr("mov.b@r", "r14", "r0")          # SSR
        a.imm_r0("and", 0x78)                 # RDRF|ORER|FER|PER
        a.rr("tst", "r0", "r0")
        a.br("bt", L("end"))                  # 何も起きていない
        a.imm_r0("and", 0x40)                 # RDRF だけ残す
        a.rr("tst", "r0", "r0")
        a.br("bt", L("clr"))                  # エラーだけ -> 落として復帰させる
        a.rr("mov.b@r", "r14", "r0")          # SSR を読み直して
        a.imm_r0("and", 0x87)                 # RDRF/ORER/FER/PER を落とす（TDRE は保つ）
        a.rr("mov.br@", "r0", "r14")
        a.movb_disp_r0(1, "r14")              # RDR（受信バイト、符号拡張）
        a.imm_r0("tst", 0x80)                 # T=1 ならデータバイト
        a.br("bf", L("status"))

        # --- データバイト（0x00-0x7F）
        a.rr("tst", "r9", "r9"); a.br("bt", L("end"))          # 待機中でなければ無視
        a.movi(1, "r14"); a.rr("cmp/eq", "r14", "r9")
        a.br("bf", L("d2"))
        a.imm_r0("cmp/eq", 0x43); a.br("bf", L("reset"))       # Yamaha ID
        a.movi(2, "r9"); a.br("bra", L("end")); a.nop()
        a.label(L("d2"))
        a.movi(2, "r14"); a.rr("cmp/eq", "r14", "r9")
        a.br("bf", L("d3"))
        a.imm_r0("cmp/eq", 0x7D); a.br("bf", L("reset"))       # デバイス番号
        a.movi(3, "r9"); a.br("bra", L("end")); a.nop()
        a.label(L("d3"))
        a.movi(7, "r14"); a.rr("cmp/hs", "r14", "r9")
        a.br("bt", L("reset"))                                 # 7bit を 4 個より多い -> 異常
        a.rn("shll8", "r12"); a.rn("shlr", "r12")              # r12 <<= 7
        a.rr("or", "r0", "r12")
        a.addi(1, "r9")
        a.br("bra", L("end")); a.nop()

        # --- ステータスバイト（0x80-0xFF）
        a.label(L("status"))
        a.imm_r0("cmp/eq", 0xF0); a.br("bf", L("f7"))
        a.movi(1, "r9"); a.movi(0, "r12")                      # SysEx 開始
        a.br("bra", L("end")); a.nop()
        a.label(L("f7"))
        a.imm_r0("cmp/eq", 0xF7); a.br("bf", L("reset"))
        a.movi(7, "r14"); a.rr("cmp/eq", "r14", "r9")
        a.br("bf", L("reset"))
        a.rr("mov", "r12", "r15"); a.addi(1, "r15")             # 要求成立。r15 = ブロック+1
        a.movi(RX_IDLE, "r9")
        a.br("bra", L("end")); a.nop()

        a.label(L("reset"))
        a.movi(RX_IDLE, "r9")
        a.br("bra", L("end")); a.nop()

        # エラーだけが立っている場合。落としておかないと受信が復帰しない
        a.label(L("clr"))
        a.rr("mov.b@r", "r14", "r0")
        a.imm_r0("and", 0x87)
        a.rr("mov.br@", "r0", "r14")
        a.label(L("end"))

    # ---- 割り込みを全マスク
    a.stc_sr("r0"); a.imm_r0("or", 0xF0); a.rn("ldc.sr", "r0")
    a.movi(0, "r4"); a.movi(RX_IDLE, "r9"); a.movi(0, "r12"); a.movi(0, "r15")

    # ---- SCI0 初期化（MIDI OUT / MIDI IN A, 31250bps, 送受信とも有効）
    a.movl_imm(SCI0, "r13")
    a.movi(0, "r0")
    a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")   # SCR = 0
    a.rr("mov.br@", "r0", "r13")                                                # SMR = 0
    a.movi(BRR_31250, "r0")
    a.rr("mov", "r13", "r14"); a.addi(1, "r14"); a.rr("mov.br@", "r0", "r14")   # BRR
    delay(20000, "initwait")
    a.movi(0x30, "r0")
    a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")   # SCR = TE|RE

    if rx_sci == 1:
        # ---- SCI1 初期化（MIDI IN B, 受信のみ）
        a.movl_imm(SCI1, "r13")
        a.movi(0, "r0")
        a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")   # SCR = 0
        a.rr("mov.br@", "r0", "r13")                                                # SMR = 0
        a.movi(BRR_31250, "r0")
        a.rr("mov", "r13", "r14"); a.addi(1, "r14"); a.rr("mov.br@", "r0", "r14")   # BRR
        delay(20000, "initwait1")
        a.movi(0x10, "r0")
        a.rr("mov", "r13", "r14"); a.addi(2, "r14"); a.rr("mov.br@", "r0", "r14")   # SCR = RE

    # ---- PA21 を出力・1 にする（背面 MIDI IN A を RXD0 へ。ダウンローダと同じ設定）
    # 他のビットを巻き添えにしないよう読み変更書きにする。上位ポート A には
    # LCD や SmartMedia の制御線も同居していて、丸ごと上書きすると壊れる
    for adr in (PAIOR_H, PADR_H):
        a.movl_imm(adr, "r13")
        a.rr("mov.w@r", "r13", "r0"); a.rr("extu.w", "r0", "r0")
        a.imm_r0("or", PA21)
        w16("r13", "r0")

    # ---- 定数
    a.movl_imm(SCI0 + 4, "r1")            # SSR
    a.movl_imm(SWP_WAVE_ADR_H, "r2")      # 波形アドレス上位（+2 が下位）
    a.movl_imm(SWP_WAVE_TRIG, "r8")       # トリガ（+2 が ch15 = ステータス）
    a.movl_imm(SWP_WAVE_SIZE_H, "r10")    # サイズ上位（+2 が下位）
    a.movl_imm(SWP_WAVE_DAT_H, "r11")     # データ上位（+2 が下位）
    a.br("bra", "swpinit"); a.nop()
    a.flush_pool()

    # ---- SWP30 の起動時初期化
    # 本体ファームを置き換えているので、SWP30 は電源投入時のまま初期化されていない。
    # これをやらないと wave direct access が動かず、読み出し値が固定値になる。
    a.label("swpinit")
    a.movl_imm(SWP_CTRL0, "r13"); a.movl_imm(0x9100, "r0"); w16("r13", "r0")
    a.addi(2, "r13"); a.movl_imm(0xC002, "r0"); w16("r13", "r0")
    a.movl_imm(0xC003, "r0"); w16("r13", "r0")
    a.movl_imm(SWP_CTRL_D, "r13"); a.movl_imm(0x1100, "r0"); w16("r13", "r0")
    a.movi(0x40, "r0"); w16("r13", "r0")
    delay(20000, "swpwait")

    # ---- 診断メッセージ（アドレス 1 を 1 回読んで、各レジスタの様子を報告する）
    a.movi(0, "r0"); w16("r2", "r0")                                   # adr 上位 = 0
    a.rr("mov", "r2", "r13"); a.addi(2, "r13"); a.movi(1, "r0"); w16("r13", "r0")   # adr 下位 = 1
    a.movi(0, "r0"); w16("r10", "r0")                                  # size 上位 = 0
    a.rr("mov", "r10", "r13"); a.addi(2, "r13"); a.movi(1, "r0"); w16("r13", "r0")  # size 下位 = 1
    for b in (0xF0, 0x43, 0x7D, 0x4D, 0x55, 0x44, 0x47):               # F0 43 7D "MUDG"
        send_byte(b)
    report16("r2")
    a.rr("mov", "r2", "r13"); a.addi(2, "r13"); report16("r13")
    report16("r10")
    a.rr("mov", "r10", "r13"); a.addi(2, "r13"); report16("r13")
    status_addr("r13"); report16("r13")                                 # トリガ前のステータス
    trigger()
    status_addr("r13"); report16("r13")                                 # 直後のステータス
    delay(2000, "diagwait")
    status_addr("r13"); report16("r13")                                 # 待った後のステータス
    report16("r11")
    a.rr("mov", "r11", "r13"); a.addi(2, "r13"); report16("r13")        # データ
    report16("r2")
    a.rr("mov", "r2", "r13"); a.addi(2, "r13"); report16("r13")         # アドレスの自動増加を確認
    report16("r10")
    a.rr("mov", "r10", "r13"); a.addi(2, "r13"); report16("r13")        # サイズの減算を確認
    send_byte(0xF7)
    a.br("bra", "outer"); a.nop()
    a.flush_pool()

    # ---- 本編
    a.label("outer")
    if first_block:
        a.movl_imm(first_block * words_per_block, "r5")
        a.movl_imm(first_block, "r6")
    else:
        a.movi(0, "r5"); a.movi(0, "r6")

    a.label("block")
    for b in (0xF0, 0x43, 0x7D, 0x4D, 0x55, 0x57, 0x44):               # F0 43 7D "MUWD"
        send_byte(b)
    a.movi(0, "r4")                                                    # ここから加算和を取る
    for sh in (21, 14, 7, 0):
        septet("r6", sh)
    a.movl_imm(words_per_block, "r7")

    a.label("wordloop")
    a.rr("mov", "r5", "r0"); a.rn("shlr16", "r0"); w16("r2", "r0")                  # アドレス上位
    a.rr("mov", "r2", "r13"); a.addi(2, "r13"); a.rr("mov", "r5", "r0"); w16("r13", "r0")
    a.movi(0, "r0"); w16("r10", "r0")                                               # サイズ = 1
    a.rr("mov", "r10", "r13"); a.addi(2, "r13"); a.movi(1, "r0"); w16("r13", "r0")
    trigger()
    delay(40, "trigwait")                                              # 読み出し完了を待つ固定待ち
    status_addr("r13")
    a.movl_imm(400, "r14")                                             # 続けてステータスを監視
    a.label("poll")
    a.rr("mov.w@r", "r13", "r0"); a.rr("extu.w", "r0", "r0")
    a.rr("tst", "r0", "r0"); a.br("bf", "polldone")
    a.rn("dt", "r14"); a.br("bf", "poll")
    a.label("polldone")
    a.rr("mov.w@r", "r11", "r3"); a.rr("extu.w", "r3", "r3")
    a.rr("mov", "r11", "r13"); a.addi(2, "r13")
    a.rr("mov.w@r", "r13", "r14"); a.rr("extu.w", "r14", "r14")
    a.rn("shll16", "r3"); a.rr("or", "r14", "r3")
    for sh in (28, 21, 14, 7, 0):
        septet("r3", sh)
    a.addi(1, "r5")
    a.rn("dt", "r7"); a.br("bf", "wordloop")

    # ---- 加算和 14bit（ブロック番号 4 バイト + ペイロード 640 バイト）
    a.rr("mov", "r4", "r3")
    septet("r3", 7)
    septet("r3", 0)
    send_byte(0xF7)

    # ---- 再送要求が届いていればそのブロックへ飛ぶ
    a.rr("tst", "r15", "r15")
    a.br("bt", "noreq")
    a.rr("mov", "r15", "r14"); a.addi(-1, "r14")                       # 要求ブロック
    a.movi(0, "r15")                                                   # 要求を消費
    a.rr("mov", "r14", "r0"); shl("r0", log2_wpb)                      # 先頭ワード番地
    a.movl_imm(rom_words, "r13")
    a.rr("cmp/hs", "r13", "r0")
    a.br("bt", "noreq")                                                # 範囲外の要求は無視
    a.rr("mov", "r14", "r6"); a.rr("mov", "r0", "r5")
    a.br("bra", "block"); a.nop()

    a.label("noreq")
    a.addi(1, "r6")
    a.movl_imm(rom_words, "r13")
    a.rr("cmp/hs", "r13", "r5")
    a.br("bf", "nextblock")
    a.br("bra", "outer"); a.nop()
    a.label("nextblock")
    a.br("bra", "block"); a.nop()

    # ---- send: r0 の下位 8bit を SCI0 から送る。r13, r14, r15 を破壊する
    # 送出待ちのスピンループで MIDI IN もポーリングする（受信の取りこぼしを防ぐ）
    a.label("send")
    a.rr("mov", "r0", "r13")
    a.rr("add", "r13", "r4")                                           # 加算和
    a.label("sendwait")
    rxpoll("x", (SCI1 - SCI0) if rx_sci else 0)
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
    ap.add_argument("-o", "--out", type=Path, default=Path("build/dumper.bin"))
    ap.add_argument("--rom-words", type=lambda s: int(s, 0), default=ROM_WORDS,
                    help="読み出す 32bit ワード数（既定 0x800000 = 32MB）")
    ap.add_argument("--first-block", type=lambda s: int(s, 0), default=0,
                    help="開始ブロック番号。取りこぼしたブロックだけ拾い直すときに使う")
    ap.add_argument("--rx-sci", type=int, choices=(0, 1), default=0,
                    help="再送要求を受ける系統。0 = MIDI IN A（既定）, 1 = MIDI IN B")
    a = ap.parse_args()
    code = build(a.rom_words, first_block=a.first_block, rx_sci=a.rx_sci)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(code)
    print(f"エントリ {ENTRY:#08x}, {len(code)} byte -> {a.out}")
    n = a.rom_words // WORDS_PER_BLOCK - a.first_block
    print(f"1 ブロック {WORDS_PER_BLOCK} ワード, ブロック {a.first_block} 〜 {a.rom_words // WORDS_PER_BLOCK - 1}（{n} 個）を繰り返し送出")
    total = n * WORDS_PER_BLOCK * 5 + n * 15
    print(f"送出 MIDI バイト数 {total:,} -> 31250bps で約 {total / 3125 / 3600:.1f} 時間")


if __name__ == "__main__":
    main()
