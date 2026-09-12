# MU2000 で自分のコードを走らせる — 最小の例

**LED を光らせるだけのファームウェア。** 音も MIDI も USB も動かない。
前面パネルの LED 10 個を、光が往復するだけ。

波形 ROM を USB 経由で吸う一連の作業（[usb.md](usb.md)）の副産物。
そこで分かった「どこを書き換えれば自分のコードが動くか」「どこを触っては
いけないか」を、いちばん小さい形に切り出したもの。**純正ファームを 1 行も呼ばない。**

```bash
python tools/dump/build_firmware.py --module blink -o build/firmware_blink.bin
python tools/dump/make_ydl.py --image build/firmware_blink.bin -o build/blink.ydl
python tools/dump/stage_ydl.py build/blink.ydl
build/upgrade_dumper/Upgrade.exe          # 約 12 分
```

生成物は `tools/dump/make_blink.py`。`--mode blink` で単純な全点滅、`--delay` で速さが変わる。

## 純正との差分は 110 バイトだけ

| 番地 | 大きさ | 内容 |
|---|---|---|
| 0x040100 | 12 byte | 0x3CC000 へ飛ぶトランポリン |
| 0x3CC000 | 102 byte | 本体（LED を往復させるループ） |

残りの 4MB は純正のまま。**ダウンローダ領域 0x000000-0x00C001 には 1 バイトも触らない。**

## 3 つの約束ごと

### 1. 入口は 0x040100

ダウンローダは電源投入時にバス・DRAM・スタックを整えてから、本体ファームの入口
`0x040100` を JSR で呼ぶ。ここを乗っ取れば自分のコードが走る。

スタックまで用意されている（R15 = 0xFFFFFFFC、SH7043 内蔵 RAM の末尾）ので、
こちらは何も初期化しなくていい。いきなり書き始められる。

### 2. コードは 0x3CC000 に置く

本体ファーム領域 0x040000-0x3DFFFF で 0xFF が続く空きは
**0x3CBBB3-0x3DFFFF の 83KB しか無い**。0x041000 などには純正のコードが入っていて、
そこに置くと純正を自分で壊すことになる（実際に長らくこれで嵌まった）。

### 3. LED は 2 個のラッチにぶら下がっている

書き込み専用。`MOV.B` で 1 バイト書くだけ。ポートの設定もクロックの設定も要らない。

| 番地 | 内容 |
|------|------|
| 0xE00000 | LED 8 個（bit0-7） |
| 0xC80000 | bit6-7 = LED 2 個（MU / PLG-1）、**bit0-5 はスイッチ走査の列選択** |

**0xC80000 の bit0-5 は 0 のままにすること。** あそこはパネルのスイッチを読むための
列選択で、勝手な値を書くと押していないキーを押したことにできてしまう。

## 壊しても必ず戻せる

ダウンローダに触らない限り、どんなに壊れても
**[Drum] + [PLAY] + [VALUE+] を押しながら電源投入**すればダウンロードモードに入れる。

```bash
python tools/dump/stage_ydl.py --stock
build/upgrade_dumper/Upgrade.exe
```

実際、この作業中に 3 回ほど「LCD が真っ白で無反応」の状態にしたが、毎回これで戻せた。

## MAME で確かめる

実機に焼く前に、LED の出力を MAME で見られる。フラッシュを差し替えた
`mu2000.zip` を作り、`-autoboot_script` で Lua を流して 0xE00000 / 0xC80000 への
書き込みを拾う。フラッシュ 4MB と zip 内 2 ファイルの対応は

```
h[2k] = flash[4k+1], h[2k+1] = flash[4k]      （上位 16bit を下位バイト先で）
l[2k] = flash[4k+3], l[2k+1] = flash[4k+2]    （下位 16bit を下位バイト先で）
```

`install_write_tap` が返す値は 32bit バス上のものなので、`MOV.B` で書いた 1 バイトは
マスクを見てレーンから取り出す必要がある。実際に取れた並び:

```
.........*  ........*.  .......*..  ......*...  .....*....
....*.....  ...*......  ..*.......  .*........  *.........   ← 左端
.*........  ..*.......  ...*......  ...                      ← 折り返し
```

## 次にやるなら

- **LCD に文字を出す。** HD44780 をポート E のビットバンギングで駆動する
  （PEDR 0xFFFF83B0、bit4 = E, bit0 = R/W, bit2 = RS, 上位 8bit = データ）。
  [hardware.md](hardware.md) 参照
- **MIDI OUT に何か流す。** SH7043 内蔵 SCI を 31250bps に設定する
  （SMR=0, BRR=27, SCR=TE）。`tools/dump/make_dumper.py` が実例
- **音を出す。** SWP30 の初期化が要る。`tools/dump/make_dumper.py` の `swpinit` が
  波形読み出しのための最小の初期化をやっている

## 関連

- [usb.md](usb.md) — USB 経由の波形 ROM ダンプ（完成）。この玩具の元になった作業
- [softdump.md](softdump.md) — MIDI 経由のダンプ
- [hardware.md](hardware.md) — 基板構成とメモリマップ
- [updater-protocol.md](updater-protocol.md) — `.ydl` とダウンローダのプロトコル
