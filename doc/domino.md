# Domino から鳴らす

Domino（や他の MIDI シーケンサ）から、単体版の `gui.exe` を実機と同じように
鳴らす手順。

## 仕組み

Windows には「MIDI OUT を MIDI IN に折り返す」仕掛けが**入っていない**。
Domino が出すのは MIDI OUT、`gui.exe` が受けるのは MIDI IN なので、
あいだを繋ぐ**仮想の MIDI ケーブル**が要る。

```
   Domino  ──MIDI OUT──▶  loopMIDI Port  ──MIDI IN──▶  gui.exe  ──▶ スピーカー
```

## 1. 仮想 MIDI ケーブルを入れる

[loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html)（Tobias
Erichsen、無料）を入れて起動し、`+` を押して口をひとつ作る。
既定の名前は `loopMIDI Port`。**loopMIDI は出しっぱなしにしておく**
（閉じると口が消える）。

LoopBe1 でもよいが、LoopBe1 は繋ぎっぱなしの音を検出すると勝手に黙るので、
loopMIDI のほうが向いている。

このパソコンには既に loopMIDI が入っていて、`loopMIDI Port` が見えている。

## 2. gui.exe を起動する

```bash
build/gui.exe C:\Users\gugug\GitHub\MU2000\roms
```

起動して 10 秒ほどで LCD に音色名が出る。

## 3. 口を選ぶ

**パネルに描いてある `MIDI IN A` のジャックを左クリック**（窓のどこでも
右クリックでも同じ）。品書きが出るので

* `MIDI IN` → `loopMIDI Port`
* `MIDI OUT` → 使わない（実機の THRU が要るときだけ選ぶ）

選んだものは `%LOCALAPPDATA%\S-MU2000\gui.ini` に覚えるので、
次からは何もしなくてよい。窓の下の行に、いま繋がっている口が出ている。

最初から番号で指定したいときは

```bash
build/gui.exe C:\Users\gugug\GitHub\MU2000\roms --midi 1
```

番号は `build/gui.exe --list` で分かる。

## 4. Domino 側

1. `ファイル` → `環境設定` → `MIDI-OUT`
2. `ポート A` の出力先を **`loopMIDI Port`** にする
3. 音源定義ファイルは **XG** 系（`XG(MU100/MU128 相当).xml` など）を選ぶ。
   MU2000 用のものがあればそれがいちばんよい
4. `OK` して、適当に打ち込んで再生

音が出れば繋がっている。

### 音源初期化を送る

Domino の `トラック` → `音源初期化` を送るか、XG システムオンを直接送る。

```
F0 43 10 4C 00 00 7E 00 F7
```

画面の「エフェクト」の面にある `XG リセット` を押しても同じものが出る。

## 音が出ないとき

| 様子 | 見るところ |
|---|---|
| 窓の下に `IN: なし` と出る | 口を選び直す。loopMIDI が動いているか |
| Domino が「ポートを開けない」と言う | `gui.exe` を先に閉じる。**同じ口を 2 つのソフトで同時には開けない**（loopMIDI の口は 1 対 1） |
| 受けているのに鳴らない | 音源初期化を送る。パートの音量が 0 かもしれない |
| 音が途切れる | `--latency 60` のように待ち時間を延ばす。窓の下の「枯渇」が増えていたらこれ |
| 音が遅れる | `--latency 15` まで詰められる。既定は 30 ミリ秒 |

## 実機も一緒に鳴らしたいとき

`MIDI OUT` に実機（`Yamaha MU2000-1` など）を選ぶと、`gui.exe` が受けた
ものをそのまま外へ流す（実機の THRU と同じ）。画面のつまみを回して出た
コントロールチェンジや SysEx も一緒に出るので、**同じ操作を実機とソフトの
両方に掛けて聴き比べる**のに使える。
