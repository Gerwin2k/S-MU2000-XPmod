#!/usr/bin/env python3
# license:BSD-3-Clause
"""MAME の sh7042_device::map() から、内蔵周辺のレジスタ振り分けを生成する。

MAME は address_map にレジスタを並べ、メモリ機構が幅とマスクを面倒みてくれる。
こちらにはその機構がないので、同じことを素の switch に展開する。

手で書き写すとレジスタ 1 つの取り違えが「音が違う」としてしか現れず気づけないので、
機械で起こして差分を見られるようにしてある。

  入力: MAME の sh7042.cpp（map の並び）と、こちらの sh*.h（ハンドラの署名）
  出力: src/mame/cpu/sh7042_map.hxx

幅はハンドラの戻り値から取る。範囲がそれより広いものは配列（TGR など）で、
ハンドラが offs_t を受けるかどうかで見分ける。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAME = Path(sys.argv[1] if len(sys.argv) > 1
            else r"C:/Users/gugug/GitHub/MU2000/mame-src/src/devices/cpu/sh/sh7042.cpp")

WIDTH = {"u8": 1, "u16": 2, "u32": 4}


def load_signatures():
    """こちらのヘッダから、レジスタハンドラの戻り値と引数を拾う。"""
    sig = {}
    for h in sorted((ROOT / "src/mame/cpu").glob("sh*.h")):
        cls = None
        for line in h.read_text(encoding="utf-8", errors="replace").split("\n"):
            m = re.match(r"class (sh\w*_device)", line)
            if m:
                cls = m.group(1)
            m = re.match(r"\s*(u8|u16|u32|void)\s+(\w+_[rw])\((.*?)\);", line)
            if m and cls:
                sig[(cls, m.group(2))] = (m.group(1), m.group(3).strip())
    return sig


def load_map():
    """MAME の map() を (start, end, 対象, 読み, 書き) に開く。"""
    src = MAME.read_text(encoding="utf-8")
    body = src[src.index("void sh7042_device::map("):]
    body = body[:body.index("\n}\n")]

    entries, ram, skipped = [], [], []
    line_re = re.compile(
        r"map\(0x([0-9a-f]+),\s*0x([0-9a-f]+)\)\s*\.\s*(rw|r|w)\s*\((.*)\);")
    for line in body.split("\n"):
        line = line.strip()
        if not line.startswith("map(0x"):
            continue
        if ".ram()" in line:
            m = re.match(r"map\(0x([0-9a-f]+),\s*0x([0-9a-f]+)\)", line)
            ram.append((int(m.group(1), 16), int(m.group(2), 16)))
            continue
        m = line_re.match(line)
        if not m:
            skipped.append(line)
            continue
        start, end, kind, args = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
        # 対象デバイス。省略されていれば sh7042 自身のハンドラ
        dev = "this"
        d = re.match(r"(m_\w+(?:\[\d\])?)\s*,", args)
        if d:
            dev = d.group(1)
        funcs = re.findall(r"FUNC\((\w+)::(\w+)\)", args)
        rd = wr = None
        if kind == "rw":
            rd, wr = funcs[0], funcs[1]
        elif kind == "r":
            rd = funcs[0]
        else:
            wr = funcs[0]
        entries.append((start, end, dev, rd, wr))
    return entries, ram, skipped


SIG = load_signatures()


def call(dev, func, arglist):
    who = "" if dev == "this" else dev + "->"
    return who + func[1] + "(" + ", ".join(arglist) + ")"


def build_table(entries):
    """バイト番地 -> (レジスタ先頭番地, 幅, 配列添字, 対象, 読み, 書き) の表。"""
    table = {}
    for start, end, dev, rd, wr in entries:
        probe = rd if rd is not None else wr
        ret, args = SIG[probe]
        if rd is None:
            m = re.search(r"(u8|u16|u32) data", SIG[wr][1])
            ret = m.group(1) if m else "u16"
        w = WIDTH[ret]

        span = end - start + 1
        # ハンドラが offs_t を受けるなら配列。受けないなら 1 本のレジスタで、
        # 範囲が広くてもどのバイトでも同じものを叩く（MTU の TIOR がこれ）
        indexed = bool(re.search(r"offs_t(\s+\w+)?\s*[,)]", args)) or "offs_t" in args
        count = span // w if indexed else 1

        for i in range(count):
            base = start + i * w
            width_here = w if indexed else span
            for b in range(width_here):
                table[base + b] = (base, w, i if indexed else None, dev, rd, wr)
    return table


def read_expr(table, a):
    """番地 a を担当する読みハンドラの呼び出し式。読めない番地なら None。"""
    base, w, idx, dev, rd, wr = table[a]
    if rd is None:
        return None
    args = [] if idx is None else [str(idx)]
    return base, w, call(dev, rd, args)


def write_expr(table, a, data, mask):
    base, w, idx, dev, rd, wr = table[a]
    if wr is None:
        return None
    args_str = SIG[wr][1]
    arglist = []
    if "offs_t" in args_str:
        arglist.append(str(idx if idx is not None else 0))
    arglist.append(data)
    if "mem_mask" in args_str:
        arglist.append(mask)
    return call(dev, wr, arglist)


def emit(table, ram):
    addr_lo, addr_hi = min(table), max(table)
    o = []
    o.append("// license:BSD-3-Clause")
    o.append("// copyright-holders:Olivier Galibert")
    o.append("//")
    o.append("// このファイルは tools/gen_sh7042_map.py が生成した。手で直さないこと。")
    o.append("// 元は MAME の sh7042_device::map()。")
    o.append("//")
    o.append("// MAME はアドレス空間の機構が幅とマスクを合わせてくれる。ここでは")
    o.append("// 同じ結果になるよう、バイト番地の switch に開いてある。")
    o.append("// 8bit レジスタをワードで読めば 2 本つなげて返し、16bit レジスタを")
    o.append("// バイトで書けば mem_mask を立てて片側だけ渡す。")
    o.append("")

    # ---- 8bit 読み
    o.append("u8 sh7042_device::internal_r8(offs_t a)")
    o.append("{")
    o.append("\tswitch(a) {")
    for a in sorted(table):
        e = read_expr(table, a)
        if e is None:
            continue
        base, w, expr = e
        if w == 1:
            o.append("\tcase 0x%08x: return %s;" % (a, expr))
        else:
            shift = (base + w - 1 - a) * 8      # ビッグエンディアン
            o.append("\tcase 0x%08x: return u8(%s >> %d);" % (a, expr, shift))
    o.append("\t}")
    o.append('\tlogerror("sh7042: 未対応の 8bit 読み %08x\\n", a);')
    o.append("\treturn 0;")
    o.append("}")
    o.append("")

    # ---- 16bit 読み
    o.append("u16 sh7042_device::internal_r16(offs_t a)")
    o.append("{")
    o.append("\tswitch(a) {")
    for a in sorted(x for x in table if not (x & 1)):
        e = read_expr(table, a)
        if e is None:
            continue
        base, w, expr = e
        if w == 2 and base == a:
            o.append("\tcase 0x%08x: return %s;" % (a, expr))
        elif w == 4:
            shift = (base + 2 - a) * 8
            o.append("\tcase 0x%08x: return u16(%s >> %d);" % (a, expr, shift))
        elif w == 1:
            second = read_expr(table, a + 1) if (a + 1) in table else None
            if second is not None:
                o.append("\tcase 0x%08x: return u16((%s << 8) | %s);" % (a, expr, second[2]))
            else:
                o.append("\tcase 0x%08x: return u16(%s << 8);" % (a, expr))
    o.append("\t}")
    o.append('\tlogerror("sh7042: 未対応の 16bit 読み %08x\\n", a);')
    o.append("\treturn 0;")
    o.append("}")
    o.append("")

    # ---- 32bit 読み
    o.append("u32 sh7042_device::internal_r32(offs_t a)")
    o.append("{")
    o.append("\tswitch(a) {")
    for a in sorted(x for x in table if not (x & 3)):
        e = read_expr(table, a)
        if e is None:
            continue
        base, w, expr = e
        if w == 4 and base == a:
            o.append("\tcase 0x%08x: return %s;" % (a, expr))
    o.append("\t}")
    o.append("\t// 32bit レジスタでないところは 16bit を 2 回")
    o.append("\treturn (u32(internal_r16(a)) << 16) | internal_r16(a + 2);")
    o.append("}")
    o.append("")

    # ---- 8bit 書き
    o.append("void sh7042_device::internal_w8(offs_t a, u8 v)")
    o.append("{")
    o.append("\tswitch(a) {")
    for a in sorted(table):
        base, w, idx, dev, rd, wr = table[a]
        if wr is None:
            continue
        if w == 1:
            c = write_expr(table, a, "v", "0xff")
        else:
            t = "u%d" % (w * 8)
            shift = (base + w - 1 - a) * 8
            c = write_expr(table, a, "%s(v) << %d" % (t, shift), "%s(0xff) << %d" % (t, shift))
        o.append("\tcase 0x%08x: %s; return;" % (a, c))
    o.append("\t}")
    o.append('\tlogerror("sh7042: 未対応の 8bit 書き %08x, %02x\\n", a, v);')
    o.append("}")
    o.append("")

    # ---- 16bit 書き
    o.append("void sh7042_device::internal_w16(offs_t a, u16 v)")
    o.append("{")
    o.append("\tswitch(a) {")
    for a in sorted(x for x in table if not (x & 1)):
        base, w, idx, dev, rd, wr = table[a]
        if wr is None:
            continue
        if w == 2 and base == a:
            o.append("\tcase 0x%08x: %s; return;" % (a, write_expr(table, a, "v", "0xffff")))
        elif w == 4:
            shift = (base + 2 - a) * 8
            c = write_expr(table, a, "u32(v) << %d" % shift, "u32(0xffff) << %d" % shift)
            o.append("\tcase 0x%08x: %s; return;" % (a, c))
        elif w == 1:
            hi = write_expr(table, a, "u8(v >> 8)", "0xff")
            lo = write_expr(table, a + 1, "u8(v)", "0xff") if (a + 1) in table else None
            line = "\tcase 0x%08x: %s;" % (a, hi)
            if lo is not None:
                line += " %s;" % lo
            o.append(line + " return;")
    o.append("\t}")
    o.append('\tlogerror("sh7042: 未対応の 16bit 書き %08x, %04x\\n", a, v);')
    o.append("}")
    o.append("")

    # ---- 32bit 書き
    o.append("void sh7042_device::internal_w32(offs_t a, u32 v)")
    o.append("{")
    o.append("\tswitch(a) {")
    for a in sorted(x for x in table if not (x & 3)):
        base, w, idx, dev, rd, wr = table[a]
        if wr is None or w != 4 or base != a:
            continue
        o.append("\tcase 0x%08x: %s; return;" % (a, write_expr(table, a, "v", "0xffffffff")))
    o.append("\t}")
    o.append("\t// 32bit レジスタでないところは 16bit を 2 回")
    o.append("\tinternal_w16(a, u16(v >> 16));")
    o.append("\tinternal_w16(a + 2, u16(v));")
    o.append("}")
    o.append("")
    o.append("// 内蔵レジスタの範囲: 0x%08x - 0x%08x" % (addr_lo, addr_hi))
    for s, e in ram:
        o.append("// 内蔵 RAM: 0x%08x - 0x%08x（組み立て側が領域として貼る）" % (s, e))
    return "\n".join(o) + "\n"


def main():
    entries, ram, skipped = load_map()
    if skipped:
        print("解釈できなかった map 行:", file=sys.stderr)
        for s in skipped:
            print("  ", s, file=sys.stderr)
        return 1
    table = build_table(entries)
    dst = ROOT / "src/mame/cpu/sh7042_map.hxx"
    dst.write_text(emit(table, ram), encoding="utf-8")
    print("map %d 行、担当する番地 %d 個を %s に生成" % (len(entries), len(table), dst.name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
