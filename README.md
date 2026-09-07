# S-MU2000

Yamaha MU2000 のソフトウェア音源。DAW に挿して使えることを目指す。

**現在の状態: MIDI 入力を受けてその場で鳴る。DAW への組み込み（VST3）はこれから。**

実機の firmware をそのまま走らせ、MIDI を受けて発音する。2 分半の実曲を
MAME の録音と突き合わせて、発音指示 2851 件すべてが一致、振幅も 88.7% の
区間で 10% 以内に収まっている。書き出しは実時間の 1.5 倍速。
まだ exe 一枚で、リアルタイム入力と VST3 はこれから。

## これは何か

MU2000 の中身（SH7043 CPU + SWP30 音源チップ ×2）をソフトウェアで動かし、
実機の firmware をそのまま走らせる。エミュレータなので、音色も挙動も実機由来になる。

MAME でも MU2000 は鳴る。だが MAME は自分で時計を持って実時間に追いつこうとする
つくりで、DAW や外部シーケンサと同期させると遅れが溜まって破綻する（実測で
処理能力に 177% の余力がありながら平均速度が 98% 台から上がらない）。
ソフトシンセはホストのオーディオコールバックに駆動されるので、この問題が原理的に起きない。

くわしくは [doc/design.md](doc/design.md)。

## ROM について

**ROM は同梱しない。** 利用者が自分の MU2000 から吸い出す必要がある。

| ROM | 内容 |
|---|---|
| プログラム ROM | 4MB（本体の firmware） |
| 波形 ROM | 32MB（音色データ） |

吸い出しの手順とツールは [MU2000 リポジトリ](https://github.com/tarboh/MU2000) にある。
USB ケーブル 1 本で 32MB を約 36 分。MIDI インターフェースは不要。

## 使い方

```
make

build/live.exe   <rom ディレクトリ> [--midi 番号]     MIDI 入力を受けて鳴らす
build/live.exe   --list                              MIDI 入力の一覧
build/render.exe <rom ディレクトリ> <MIDI> <出力 wav>  ファイルを WAV に
build/midisend.exe <MIDI ファイル> [--port 番号]      MIDI 出力へ実時間で流す
build/boot.exe   <rom ディレクトリ> [サイクル数]       起動の確認
```

`live` は音声デバイスが要求した分だけ音源を進める。自分で時計を持たないので、
外部と同期させてもずれない（MAME が破綻したのはここ）。待ち時間は既定で
23ms、`--frames` と `--buffers` で詰められる。CPU 使用率はおよそ 60%。

rom ディレクトリには次を置く。

| ファイル | 中身 |
|---|---|
| `mu2000_flash.bin` | プログラム ROM 4MB（CPU から見えるまま） |
| `dump/xv364a0.ic49` ほか 3 つ | 波形 ROM 8MB × 4 |
| `standin/sin-table.bin` | MEG が使う sin 表 64KB |

## 由来とライセンス

中核となるチップの実装は **MAME から取り込んでいる**。MAME 全体は GPL だが、
必要な個々のデバイス実装はすべて **BSD-3-Clause** で、流用が認められている。

| 取り込み元 | 著作権 |
|---|---|
| `src/mame/sound/swp30.*` | MAME `src/devices/sound/swp30.*` — Olivier Galibert |
| `src/mame/cpu/sh*` | MAME `src/devices/cpu/sh/` |
| `src/mame/machine/sci4.*` | MAME `src/devices/machine/sci4.*` |
| `src/mame/ymmu2000.cpp` | MAME `src/mame/yamaha/ymmu2000.cpp` |

取り込み元は MAME 0.289 相当（master 2026-09-06、コミット `1fb001f9`）。
MAME 本体はリンクしない（GPL のため）。

このリポジトリ独自のコードは BSD-3-Clause とする。

## 上流への還元

MU2000 を鳴らす過程で MAME の SWP30 に 2 つのバグを見つけ、実機の測定値をもとに
修正した。上流へ送る予定。

| 症状 | 原因 |
|---|---|
| ロングトーンで音色が次々に変わる | ループ長のマスクが 26bit（正しくは 24bit）で、ファインチューンの下位 2bit が混入。ループ長が 5000 万サンプルに化けてループしない |
| 発音 150ms 後にピッチが跳ぶ | ピッチレジスタの bit14 を clamp が誤爆し、最大ピッチに張り付く |
