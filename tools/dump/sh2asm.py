#!/usr/bin/env python3
# license:BSD-3-Clause
"""MU2000 用の最小 SH-2 アセンブラ。

自作ファームウェア（波形 ROM ダンパ）を書くために必要な命令だけを実装する。
ラベル、自動リテラルプール（`mov.l #imm,Rn`）、`.org` / `.long` / `.word` / `.byte` / `.ascii` / `.pool` に対応。
big-endian。出力は bytes。

検証は `mame/unidasm.exe -arch sh2` に通して目視/自動比較する（tools/test_sh2asm.py）。
"""
import re
import struct

R = re.compile(r"^r(\d{1,2})$", re.I)


def reg(t):
    m = R.match(t.strip())
    if not m:
        raise ValueError(f"レジスタではない: {t}")
    v = int(m.group(1))
    if not 0 <= v <= 15:
        raise ValueError(f"レジスタ番号が範囲外: {t}")
    return v


def imm(t, bits=8, signed=True):
    if isinstance(t, int):
        v = t
    else:
        t = t.strip()
        v = int(t[1:], 0) if t.startswith("#") else int(t, 0)
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    if not lo <= v <= hi and not (0 <= v < (1 << bits)):
        raise ValueError(f"即値が範囲外: {t}")
    return v & ((1 << bits) - 1)


# ---- 2 オペランド (Rm,Rn) 系: opcode nibble0, nibble3
RM_RN = {
    "mov":     (0x6, 0x3), "mov.b@r": (0x6, 0x0), "mov.w@r": (0x6, 0x1), "mov.l@r": (0x6, 0x2),
    "mov.br@": (0x2, 0x0), "mov.wr@": (0x2, 0x1), "mov.lr@": (0x2, 0x2),
    # 後置インクリメント / 前置デクリメント（スタックの退避・復帰に使う）
    "mov.b@r+": (0x6, 0x4), "mov.w@r+": (0x6, 0x5), "mov.l@r+": (0x6, 0x6),
    "mov.br@-": (0x2, 0x4), "mov.wr@-": (0x2, 0x5), "mov.lr@-": (0x2, 0x6),
    "add":     (0x3, 0xC), "sub": (0x3, 0x8), "and": (0x2, 0x9), "or": (0x2, 0xB),
    "xor":     (0x2, 0xA), "tst": (0x2, 0x8),
    "cmp/eq":  (0x3, 0x0), "cmp/hs": (0x3, 0x2), "cmp/ge": (0x3, 0x3),
    "cmp/hi":  (0x3, 0x6), "cmp/gt": (0x3, 0x7),
    "extu.b":  (0x6, 0xC), "extu.w": (0x6, 0xD), "swap.b": (0x6, 0x8), "swap.w": (0x6, 0x9),
    "exts.b":  (0x6, 0xE), "exts.w": (0x6, 0xF),
}
# ---- 1 オペランド (Rn): 0x4n?? 形式の下位 2 nibble
RN_4 = {
    "shll": 0x00, "shlr": 0x01, "shll2": 0x08, "shlr2": 0x09,
    "shll8": 0x18, "shlr8": 0x19, "shll16": 0x28, "shlr16": 0x29,
    "dt": 0x10, "jmp@": 0x2B, "jsr@": 0x0B, "ldc.sr": 0x0E, "rotl": 0x04, "rotr": 0x05,
}
BRANCH8 = {"bt": 0x89, "bf": 0x8B, "bt/s": 0x8D, "bf/s": 0x8F}
BRANCH12 = {"bra": 0xA, "bsr": 0xB}
IMM_R0 = {"cmp/eq": 0x88, "and": 0xC9, "or": 0xCB, "tst": 0xC8, "xor": 0xCA}


class Asm:
    def __init__(self, org=0):
        self.org = org
        self.items = []          # (kind, ...) 未解決を含む
        self.labels = {}
        self.pc = org
        self.pool = []           # 保留中のリテラル [(value, [item_index...])]

    # --- 出力ヘルパ
    def _emit(self, word):
        self.items.append(("w", word))
        self.pc += 2

    def _emit_fixup(self, kind, *args):
        self.items.append((kind, self.pc, *args))
        self.pc += 2

    # --- ディレクティブ
    def label(self, name):
        if name in self.labels:
            raise ValueError(f"ラベル重複: {name}")
        self.labels[name] = self.pc

    def long(self, v):
        if isinstance(v, str):
            self.items.append(("L", self.pc, v))
        else:
            self.items.append(("w", (v >> 16) & 0xFFFF))
            self.items.append(("w", v & 0xFFFF))
        self.pc += 4

    def word(self, v):
        self._emit(v & 0xFFFF)

    def data(self, b: bytes):
        if len(b) % 2:
            b += b"\x00"
        for i in range(0, len(b), 2):
            self._emit((b[i] << 8) | b[i + 1])

    def align4(self):
        if self.pc % 4:
            self._emit(0x0009)   # NOP

    def _falls_through(self):
        """直前が無条件分岐 / rts / jmp でなければ True（= ここに来ると実行が続く）。"""
        if len(self.items) < 2:
            return True
        prev = self.items[-2]          # 遅延スロットの 1 つ前
        if prev[0] == "B12" and prev[2] == BRANCH12["bra"]:
            return False
        if prev[0] == "w" and prev[1] in (0x000B, 0x002B):        # RTS / RTE
            return False
        if prev[0] == "w" and (prev[1] & 0xF0FF) == 0x402B:       # JMP @Rn
            return False
        return True

    def flush_pool(self, allow_fallthrough=False):
        """保留中のリテラルをここに配置する。

        リテラルはデータなので、実行が流れ込む場所に置いてはいけない。
        直前が bra / rts / jmp でなければ拒否する（暴走の原因になる）。
        意図的に置きたい場合だけ allow_fallthrough=True を渡すこと。
        """
        if not self.pool:
            return
        if not allow_fallthrough and self._falls_through():
            raise ValueError(
                f"リテラルプールを実行が流れ込む位置に置こうとしている @{self.pc:#08x}。"
                " 直前に bra で飛び越すか、rts の後ろに置くこと")
        self.align4()
        for value, refs in self.pool:
            addr = self.pc
            for idx in refs:
                k = self.items[idx]
                self.items[idx] = (k[0], k[1], k[2], addr)
            self.long(value)
        self.pool = []

    # --- 命令
    def movi(self, v, rn):          # MOV #imm,Rn
        self._emit(0xE000 | (reg(rn) << 8) | imm(v))

    def movl_imm(self, value, rn):
        """疑似: MOV.L #imm32,Rn（リテラルプール経由）。

        value にラベル名（str）を渡すと、そのラベルの番地を読み込む。
        """
        idx = len(self.items)
        self._emit_fixup("P", reg(rn), None)
        for v, refs in self.pool:
            if v == value:
                refs.append(idx)
                return
        self.pool.append((value, [idx]))

    def rr(self, op, rm, rn):
        hi, lo = RM_RN[op]
        self._emit((hi << 12) | (reg(rn) << 8) | (reg(rm) << 4) | lo)

    def rn(self, op, r):
        self._emit(0x4000 | (reg(r) << 8) | RN_4[op])

    def imm_r0(self, op, v):
        self._emit((IMM_R0[op] << 8) | imm(v, 8, False))

    def addi(self, v, r):
        self._emit(0x7000 | (reg(r) << 8) | imm(v))

    def br(self, op, label):
        if op in BRANCH12:
            self._emit_fixup("B12", BRANCH12[op], label)
        else:
            self._emit_fixup("B8", BRANCH8[op], label)

    def nop(self):
        self._emit(0x0009)

    def rts(self):
        self._emit(0x000B)

    def long_br(self, cond, label):
        """bt/bf の届く範囲を超える場合に、条件を反転して bra で飛ぶ。"""
        inv = {"bt": "bf", "bf": "bt"}[cond]
        skip = f"__lb{len(self.items)}"
        self.br(inv, skip)
        self.br("bra", label); self.nop()
        self.label(skip)

    def movb_disp_r0(self, disp, rm):
        """MOV.B @(disp,Rm),R0 — disp は 0-15。読み出しは符号拡張。"""
        if not 0 <= disp <= 15:
            raise ValueError(f"disp が範囲外: {disp}")
        self._emit(0x8400 | (reg(rm) << 4) | disp)

    def sts_pr_push(self):
        """STS.L PR,@-R15 — PR をスタックへ退避する。"""
        self._emit(0x4F22)

    def lds_pr_pop(self):
        """LDS.L @R15+,PR — PR をスタックから戻す。"""
        self._emit(0x4F26)

    def stc_sr(self, rn):
        self._emit(0x0002 | (reg(rn) << 8))

    # --- 組み立て
    def assemble(self):
        self.flush_pool()
        # 位置確定（items は生成順 = アドレス順）
        addr = self.org
        positions = []
        for it in self.items:
            positions.append(addr)
            addr += 4 if it[0] == "L" else 2
        out = bytearray()
        for i, it in enumerate(self.items):
            a = positions[i]
            if it[0] == "w":
                out += struct.pack(">H", it[1])
            elif it[0] == "L":
                out += struct.pack(">I", self.labels[it[2]])
            elif it[0] == "P":
                _, _, rn, target = it
                disp = (target - ((a + 4) & ~3)) // 4
                if not 0 <= disp <= 255:
                    raise ValueError(f"リテラルプールが遠すぎる @{a:06x} disp={disp}")
                out += struct.pack(">H", 0xD000 | (rn << 8) | disp)
            elif it[0] == "B12":
                _, _, hi, lab = it
                disp = (self.labels[lab] - (a + 4)) // 2
                if not -2048 <= disp <= 2047:
                    raise ValueError(f"分岐が遠すぎる @{a:06x} -> {lab}")
                out += struct.pack(">H", (hi << 12) | (disp & 0xFFF))
            elif it[0] == "B8":
                _, _, hi, lab = it
                disp = (self.labels[lab] - (a + 4)) // 2
                if not -128 <= disp <= 127:
                    raise ValueError(f"条件分岐が遠すぎる @{a:06x} -> {lab}（bra を挟むこと）")
                out += struct.pack(">H", (hi << 8) | (disp & 0xFF))
            else:
                raise AssertionError(it)
        return bytes(out)
