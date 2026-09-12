# S-MU2000

Yamaha MU2000 のソフトウェア音源。DAW に挿して使えることを目指す。

**現在の状態: VST3 として DAW に挿して鳴る。実機のフロントパネル風の画面が付いた。**

作りかけを晒しながら進めている。X では `#S_MU2000`。

> ヤマハとは無関係の非公式なプロジェクト。Yamaha・MU2000・XG はヤマハ株式会社の商標。

実機の firmware をそのまま走らせ、MIDI を受けて発音する。2 分半の実曲を
MAME の録音と突き合わせて、発音指示 2851 件すべてが一致、振幅も 88.7% の
区間で 10% 以内に収まっている。実時間再生の CPU 使用率はおよそ 38%。
LCD は firmware が書いたものがそのまま出て、ボタンもダイヤルも触れる。

## これは何か

MU2000 の中身（SH7043 CPU + SWP30 音源チップ ×2）をソフトウェアで動かし、
実機の firmware をそのまま走らせる。エミュレータなので、音色も挙動も実機由来になる。

MAME でも MU2000 は鳴る。だが MAME は自分で時計を持って実時間に追いつこうとする
つくりで、DAW や外部シーケンサと同期させると遅れが溜まって破綻する（実測で
処理能力に 177% の余力がありながら平均速度が 98% 台から上がらない）。
ソフトシンセはホストのオーディオコールバックに駆動されるので、この問題が原理的に起きない。

くわしくは [doc/design.md](doc/design.md)。残っているものは
[doc/todo.md](doc/todo.md) に、直す順で並べてある。

## ROM について

**ROM は同梱しない。** 利用者が自分の MU2000 から吸い出す必要がある。

| ROM | 内容 |
|---|---|
| プログラム ROM | 4MB（本体の firmware） |
| 波形 ROM | 32MB（音色データ） |

**吸い出しの手順と道具は [doc/dump/](doc/dump/) に入っている。**

* プログラム ROM は**吸い出さなくていい**。ヤマハが公開している更新プログラム
  （`mu2r1_uw.zip`）から復元できる。中身は Flash 書き込みの SysEx をそのまま
  収めた MIDI ファイルで、組み直すと MAME 登録の SHA1 に一致する
* 波形 ROM は **USB ケーブル 1 本で約 36 分**。分解も MIDI インターフェースも
  要らない。自作のダンパを本体ファーム領域だけに書き込み、SWP30 の
  wave direct access で読んだものを USB へ流す。ダウンローダには触らないので
  純正アップデータでいつでも戻せる（が、ファーム書き換えなので自己責任で）

MIDI 経由の予備の経路もあり、両方で吸ったものが 1 バイト残らず一致することを
確かめてある。

## 使い方

```
make

build/live.exe   <rom ディレクトリ> [--midi 番号]     MIDI 入力を受けて鳴らす
build/live.exe   --list                              MIDI 入力の一覧
build/render.exe <rom ディレクトリ> <MIDI> <出力 wav>  ファイルを WAV に
build/midisend.exe <MIDI ファイル> [--port 番号]      MIDI 出力へ実時間で流す
build/boot.exe   <rom ディレクトリ> [サイクル数]       起動の確認
build/gui.exe    <rom ディレクトリ> [--midi 番号]      実機パネル風の画面で鳴らす
build/gui.exe    --list                              MIDI の入口と出口の一覧
build/rec.exe    --list                              音声入力の一覧
build/rec.exe    <番号> <wav> <秒> [--send <番号> <MIDI>]  実機の音を録る
build/blocktime.exe <rom> <MIDI> <フレーム数> [秒]  1 ブロックの所要時間を測る
```

**Domino など外のシーケンサから鳴らす手順は
[doc/domino.md](doc/domino.md)**。要るのは仮想 MIDI ケーブル（loopMIDI）
ひとつだけ。`gui.exe` は入口と出口を**動かしたまま画面から選べる**ので、
パネルの `MIDI IN A` のジャックを押すか、窓のどこかを右クリックする。
入口は **A と B の 2 口**（パート 1-16 と 17-32）で、THRU の出口も口ごとに選べる。
選んだものは `%LOCALAPPDATA%\S-MU2000\gui.ini` に覚えておく。

画面の中身は [doc/gui.md](doc/gui.md)。3 面ある。
**パネルの絵は作り直さずに直せる**。位置も色も `panel.txt` という文字
ファイルに追い出してある（[doc/panel-editing.md](doc/panel-editing.md)）。

* **パネル** … 実機のフロントパネル（LCD・ボタン 35 個・大きなダイヤル）
* **エディタ** … SOL2 風。パート別のフィルタ・EG・エフェクト送り・音色
* **エフェクト** … リバーブ／コーラス／バリエーションと、インサーション 2 系統
  （番地は実測で確かめてある。[doc/effects.md](doc/effects.md)）

VST3 の画面も同じもの。

DAW に挿すなら VST3。作り方と ROM の置き場は [doc/vst3.md](doc/vst3.md)。
置き場は `%LOCALAPPDATA%\Programs\Common\VST3`（利用者ごと）か
`C:\Program Files\Common Files\VST3`（全員）。ROM は同梱できないので、
バンドルの `Contents/Resources/roms.txt` に置き場所を 1 行書く。

```
make vst3           build/S-MU2000.vst3/ にバンドルができる
make install-vst3   VST3 の置き場へ複製する
make probe          DAW 無しで読み込みと発音を確かめる
```

`live` は音声デバイスが要求した分だけ音源を進める。自分で時計を持たないので、
外部と同期させてもずれない（MAME が破綻したのはここ）。WASAPI の共有モードを
使い、待ち時間は既定で 30ms（`--latency` で調整）。CPU 使用率はおよそ 38%。

**出力はデバイスが言ってくる形式のまま開く。** 48000Hz を言ってくる機械では
44100 からの変換を自前の sinc でやる（Windows の変換器を通さない）。溜めを
30ms より詰めると、重い曲で音源の最悪値（周期 10ms に対して 9.2ms）と
並んでしまうので、いまはここが下限。

rom ディレクトリには次を置く。

| ファイル | 中身 |
|---|---|
| `mu2000_flash.bin` | プログラム ROM 4MB（CPU から見えるまま） |
| `dump/xv364a0.ic49` ほか 3 つ | 波形 ROM 8MB × 4 |
| `standin/sin-table.bin` | MEG が使う sin 表 64KB |

## ビルドについて

MSYS2 / MinGW-w64 の g++ を想定している。C++20 が要る。
`make test` で回帰試験が回る（[doc/testing.md](doc/testing.md)）。ROM が無い
機械でも、ROM の要らない分だけは走る。
出来た exe は **MSYS2 の DLL に依存しない**ように静的リンクしてある
（動的リンクのままだと、素の PowerShell から起動しても何も言わずに終わる）。

## 由来とライセンス

中核となるチップの実装は **MAME から取り込んでいる**。MAME 全体は GPL だが、
必要な個々のデバイス実装はすべて **BSD-3-Clause** で、流用が認められている。

| 取り込み元 | 著作権 |
|---|---|
| `src/mame/sound/swp30.*` | MAME `src/devices/sound/swp30.*` — Olivier Galibert |
| `src/mame/cpu/sh*` | MAME `src/devices/cpu/sh/` |
| `src/mame/machine/sci4.*` | MAME `src/devices/machine/sci4.*` |
| `src/mame/video/hd44780.*` | MAME `src/devices/video/hd44780.*` — Sandro Ronco |
| `src/mame/ymmu2000.cpp` | MAME `src/mame/yamaha/ymmu2000.cpp` |

`src/mame/cpu/sh*` は Olivier Galibert と David Haywood、パネルの絵
（`art/mame/`）は hap と Felipe Sanches（CC0-1.0）。取り込んだ側の改変には
`S-MU2000:` の印を付けてある。

取り込み元は MAME 0.289 相当（master 2026-09-06、コミット `1fb001f9`）。
MAME 本体はリンクしない（GPL のため）。VST3 は口の定義（MIT）だけを使い、
GPLv3 と Steinberg 独自ライセンスの二択になる SDK 本体は使っていない。

配るときに添えるものは [NOTICE.txt](NOTICE.txt) にまとめてある。

VST3 のインターフェース定義（`third_party/vst3/pluginterfaces`）は Steinberg の
ものだが **MIT** で配られている。GPLv3 の `public.sdk` は使っていないので、
プラグインの土台は全部このリポジトリの中にある。
くわしくは [third_party/vst3/README.md](third_party/vst3/README.md)。

このリポジトリ独自のコードは BSD-3-Clause とする。

## 上流への還元

MU2000 を鳴らす過程で MAME の SWP30 に 2 つのバグを見つけ、実機の測定値をもとに
修正した。[mamedev/mame#16075](https://github.com/mamedev/mame/pull/16075) として
取り込まれている。

| 症状 | 原因 |
|---|---|
| ロングトーンで音色が次々に変わる | ループ長のマスクが 26bit（正しくは 24bit）で、ファインチューンの下位 2bit が混入。ループ長が 5000 万サンプルに化けてループしない |
| 発音 150ms 後にピッチが跳ぶ | ピッチレジスタの bit14 を clamp が誤爆し、最大ピッチに張り付く |
