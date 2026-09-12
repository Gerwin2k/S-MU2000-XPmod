#!/usr/bin/env python3
# license:BSD-3-Clause
"""実機に書き込むファームウェアイメージを組み立てる。

純正ファームウェア（roms/mu2000_flash.bin）を土台に、
  - 0x040100（ダウンローダが JSR する本体ファームの入口）に 0x041000 へのジャンプ
  - 0x041000 に波形 ROM ダンパ本体
を埋め込む。0x040204 以降の割り込みベクタスタブは温存する。

ダウンローダ（0x000000-0x00C001）は一切変更しないので、失敗しても
[Drum]+[PLAY]+[VALUE+] 起動 → 純正アップデータで復旧できる。
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import make_dumper
import make_rxtest
import make_usbprobe
import make_usbinit
import make_usbhook
import make_usbdump
import make_usbwrap
import make_blink
from sh2asm import Asm

ENTRY = 0x00040100
# 自作コードの置き場所。0x041000 には純正のコードが入っているので使ってはいけない。
# 本体ファーム領域で 0xFF が続いている唯一の空きが 0x3CBBB3-0x3DFFFF（約 83KB）。
# ここに置けば、純正ファームは 0x040100 の 12 byte 以外まったく無傷で残る。
BODY = 0x003CC000


def trampoline(target: int) -> bytes:
    a = Asm(ENTRY)
    a.movl_imm(target, "r0")
    a.rn("jmp@", "r0")
    a.nop()
    a.flush_pool()
    return a.assemble()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", type=Path, default=Path("roms/mu2000_flash.bin"))
    ap.add_argument("-o", "--out", type=Path, default=Path("build/firmware_dumper.bin"))
    ap.add_argument("--rom-words", type=lambda s: int(s, 0), default=make_dumper.ROM_WORDS)
    ap.add_argument("--first-block", type=lambda s: int(s, 0), default=0,
                    help="開始ブロック番号（取りこぼし回収用）")
    ap.add_argument("--body", type=lambda s: int(s, 0), default=BODY,
                    help="自作コードの配置先（既定 0x3CC000。純正コードと重ならない空き領域）")
    ap.add_argument("--module", choices=("dumper", "rxtest", "usbprobe", "usbinit", "usbhook", "usbdump", "usbwrap", "blink"), default="dumper",
                    help="dumper = 波形 ROM ダンパ(MIDI), rxtest = MIDI IN 疎通確認, usbprobe = USB 探査, "
                         "usbinit = 純正初期化を通す, usbhook = USB 送信の疎通/速度測定, "
                         "usbdump = 波形 ROM ダンパ(USB), "
                         "usbwrap = 割り込みを包むだけ(切り分け用), "
                         "blink = LED を光らせるだけ(Hello World)")
    ap.add_argument("--words", type=int, default=None,
                    help="usbdump 専用。1 メッセージのワード数（既定 16）。"
                         "大きいほど 1 引き金あたりの取れ高が増える")
    ap.add_argument("--tail", choices=("blink", "stock"), default="blink",
                    help="usbinit 専用。stock = 純正のメインループへ飛ぶ")
    ap.add_argument("--rx-sci", type=int, choices=(0, 1), default=0,
                    help="再送要求／echo を受ける系統。0 = MIDI IN A（既定）, 1 = MIDI IN B")
    a = ap.parse_args()

    img = bytearray(a.base.read_bytes())
    if len(img) != 0x400000:
        print(f"警告: 土台イメージが 4MB ではない ({len(img):,} byte)")

    tr = trampoline(a.body)
    body = _build_at(a.body, a.rom_words, a.first_block, a.module, a.rx_sci, a.tail, a.words)

    img[ENTRY:ENTRY + len(tr)] = tr
    img[a.body:a.body + len(body)] = body

    # ---- ヘッダの割り込みベクタを自前ハンドラに差し替える
    # ダウンローダは 0x040100 へ飛ぶ直前に、本体ファームのヘッダ 0x0400B4-0x0400DC から
    # 6 個の番地を読んでワーク RAM 0x401030-0x401044 に据える。IRQ2/IRQ3（USB 送受信）の
    # ハンドラはそこ経由で呼ばれるので、ここを書き換えれば自前のハンドラが動く。
    mod = {"dumper": make_dumper, "rxtest": make_rxtest, "usbprobe": make_usbprobe,
           "usbinit": make_usbinit, "usbhook": make_usbhook,
           "usbdump": make_usbdump, "usbwrap": make_usbwrap, "blink": make_blink}[a.module]
    patch = getattr(mod, "HEADER_PATCH", None)
    labels = getattr(mod, "LAST_LABELS", None)
    if patch and labels:
        for off, name in sorted(patch.items()):
            if name not in labels:
                raise SystemExit(f"ラベルが見つからない: {name}")
            addr = labels[name]
            img[off:off + 4] = struct.pack(">I", addr)
            print(f"ヘッダ {off:#08x} = {addr:#08x}  ({name})")

    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(bytes(img))
    print(f"トランポリン {len(tr)} byte @ {ENTRY:#08x} -> {a.body:#08x}")
    print(f"本体コード   {len(body)} byte @ {a.body:#08x}")
    print(f"作成: {a.out} ({len(img):,} byte)")
    return 0


def _build_at(org: int, rom_words: int, first_block: int = 0,
              module: str = "dumper", rx_sci: int = 0, tail: str = "blink",
              words: int = None) -> bytes:
    """build() を任意の番地で組み立てる。"""
    mod = {"dumper": make_dumper, "rxtest": make_rxtest, "usbprobe": make_usbprobe,
            "usbinit": make_usbinit, "usbhook": make_usbhook,
            "usbdump": make_usbdump, "usbwrap": make_usbwrap, "blink": make_blink}[module]
    old = mod.ENTRY
    mod.ENTRY = org
    try:
        if module == "dumper":
            return mod.build(rom_words=rom_words, first_block=first_block, rx_sci=rx_sci)
        if module == "rxtest":
            return mod.build(rx_sci=rx_sci)
        if module == "usbinit":
            return mod.build(tail=tail)
        if module == "usbdump":
            if words:
                return mod.build(rom_words=rom_words, words=words)
            return mod.build(rom_words=rom_words)
        return mod.build()
    finally:
        mod.ENTRY = old


if __name__ == "__main__":
    sys.exit(main())
