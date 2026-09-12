# ROM 吸い出し手順

> **注:** この文書には MAME 側の道具（`run_mame.ps1` など）を指す箇所がある。
> それらはこのリポジトリには入っていない。

対象: プログラム Flash 2 個（IC24, IC25）と波形マスク ROM 4 個（IC49, IC50, IC53, IC54）。
いずれも表面実装（SOP / TSOP）で、MU2000 の標準ファームウェアにはメモリ内容を
外部へ出力する機能がないため、**チップを基板から外してプログラマで読む**のが基本。

## 0. 事前確認（分解して写真を撮る）

1. AC アダプタとバックアップ電池の状態を確認。電池を外すとユーザーデータ（SRAM）が消えるので、
   必要なら先に MIDI バルクダンプで退避する。
2. 天板を外し、メイン基板（DM）の IC24 / IC25 / IC49 / IC50 / IC53 / IC54 の**刻印を撮影**する。
3. 刻印からデータシートを探し、ピン配置・電源電圧（3.3V/5V）・パッケージ寸法を確定する。
   → `doc/dump/hardware.md` の「実機で確認した型番」欄に記入。

型番が分かるまで、以下の機材選定は仮。

## 1. 機材

| 用途 | 推奨 | 備考 |
|------|------|------|
| プログラマ | XGecu T56（または T48） | マスク ROM 読み出し対応、TSOP48/SOP44 アダプタが豊富 |
| アダプタ | TSOP48 (12×20mm, Type I) ソケット, SOP44 ソケット | 型番確定後に足ピッチと本体サイズを照合 |
| 取り外し | ホットエアー（300–330℃）, 低温はんだ（Chip Quik 等）, フラックス, 予熱台があると安全 | |
| 再実装 | フラックス, 細いこて先, ドラッグはんだ | 実機を動かし続けたい場合 |
| 代替 | Raspberry Pi Pico + TSOP48 ソケットで自作リーダ | プログラマ非対応の型番のとき |

## 2. プログラム Flash（IC24, IC25）: 吸い出し不要

Yamaha 公式アップデータ `mu2r1_uw.zip`（[配布ページ](https://jp.yamaha.com/support/updates/mu2r1_uw.html)）の
`part1/images/v200U12k.ydl` と `part2/images/v200u22k.ydl` は、先頭 4 バイトを `NZem` に変えた
Standard MIDI File で、中身は Yamaha SysEx（`F0 43 xx 59 ...`、モデル ID 0x59 = "MU128 DL" ダウンロードモード）
による Flash 書き込みデータそのもの。

```bash
python tools/dump/ydl_extract.py roms/updater/x/mu2r1_uw/part1/images/v200U12k.ydl                             roms/updater/x/mu2r1_uw/part2/images/v200u22k.ydl                             --mame-dir roms/dump -o roms/mu2000_flash.bin
```

- part1 が 0x000000–0x00BFFF（48KB、ダウンローダ側）、part2 が 0x040000–0x3DFFFF（29 セクタ × 128KB）を書く。
  part2 は 0x060000 から順に書き、最後に 0x040000 のセクタを書く（途中失敗時にブート側を壊さない配慮と思われる）。
- 0x00C000–0x03FFFF と 0x3E0000–0x3FFFFF はアップデータが触らない領域。0xFF 埋めにすると MAME 登録の SHA1 と一致する。
- 復元手順の詳細は `tools/dump/ydl_extract.py` 冒頭のコメント参照。

### 参考: それでも Flash チップから読みたい場合（16Mbit ×16bit, SOP44 想定）

- 候補品種: MBM29F160 / AM29F160 / LH28F160 / TC58FVx160 系。
- T56 で該当型番（またはピン互換品）を選び **Read** → `ic25.bin`, `ic24.bin` として保存（各 2,097,152 byte）。
- 各チップ 2 回読んで一致を確認（`fc /b` または `tools/dump/verify_roms.py`）。
- 実機が EX 化済みなら内容は v2.01 相当、未アップグレードなら v1.01。どちらも MAME に登録がある。
  一致すれば `tools/dump/verify_roms.py` が MAME 用ファイル名にリネームしてくれる。
- ハッシュが一致しない場合の確認順:
  1. バイトスワップ（16bit チップをバイト単位で逆に読んでいる）→ スクリプトが自動判定
  2. IC24/IC25 の取り違え → スクリプトが自動判定
  3. Flash にアップデータ以外の領域（ブートローダ等）がある → ダンプは正しいので、そのまま使い MAME 側の定義を追加する

### 基板から外さない方法（非推奨・未検証）

SOP44 テストクリップで CPU をリセット状態（/RES=L）に固定して読む方法もあるが、
SH7043 のリセット中のバス状態が不確かでバス衝突の恐れがある。基本は取り外し。

## 3. 波形マスク ROM（IC49, IC50, IC53, IC54: 64Mbit ×16bit, TSOP48 想定）

入手ルートの選択肢（詳細は README の Phase 1c）:

1. **ソフトダンプ**: アップデータの SysEx プロトコル（`tools/dump/ydl_extract.py` に記載）で自作コードを実機に送り、
   SWP30 の wave direct access レジスタで 32MB を読んで MIDI OUT / SmartMedia に書き出す。チップ脱着不要だが
   ファーム解析と文鎮化リスクあり。MAME 上でプログラム ROM だけ動かして解析できる。
2. **ジャンク基板**: MU500 / MU1000 / MU2000 は同じ 4 個の波形 ROM。ジャンク品を犠牲にする。
3. **チップ脱着 + プログラマ**（以下の手順）。
4. TSOP48 クリップで基板上から読む（SWP30 とバス競合するため非推奨）。

- 候補品種: MX23L6410 (Macronix) / KM23C64100 (Samsung) / LH53V64xx (Sharp) / TC5364105 (東芝) 等。3.3V 品が多い。
- マスク ROM はプログラマの品種リストに無いことが多い。その場合は
  **ピン互換の 64Mbit ×16 NOR Flash（例: MX29LV640 / S29GL064 の ×16 モード）を選んで読み出しのみ行う**。
  データシート同士でピン配置（A0–A21, DQ0–DQ15, /CE, /OE, VCC, GND, BYTE）を必ず突き合わせる。
- 各チップ 8,388,608 byte。2 回読んで一致確認。
- 4 個とも MAME に SHA1 登録があるので、`tools/dump/verify_roms.py` で照合できる。
  マスク ROM は全個体同一内容なので、**一致しなければ読み出し設定の誤り**（バイトスワップ、ピン配置、接触不良）。

## 4. 検証と MAME 用セット作成

```bash
python tools/dump/verify_roms.py roms/dump
python tools/dump/verify_roms.py roms/dump --dummy-wave   # 波形 ROM がまだ無いとき（無音起動テスト用）
```

- 各ファイルの CRC32/SHA1 を MAME の定義と照合し、バイトスワップも自動検出。
- 全部揃うと `roms/mu2000.zip`（MAME 用ファイル名）を生成する。

## 5. 任意: USB マイコン用 Flash（UD 基板 IC5, 4Mbit）

MAME は現状 USB を実装していないので不要。将来の完全再現用に、余裕があれば同時に吸う。
M37640（IC3）内蔵マスク ROM は特殊モードでの読み出しが必要なため当面対象外。

## 6. MAME のデバイス ROM（代替品を自作）

`mu2000` は本体 ROM の他に、MAME 内部デバイスのファイルを 3 つ要求する。いずれも MAME 開発者が作成したデータで
ソースツリーには含まれず、正規の入手先が無いため `tools/dump/make_standins.py` で代替品を生成する。

| ファイル | 用途 | 代替品の作り方 |
|---------|------|---------------|
| `mulcd.svg`（mulcd.zip） | LCD 描画用 SVG。`<title>` が出力名 `%03x.%d.%d`（セル・行・列）の矩形を点灯させる | 2 行 × 24 桁のドットを自前で生成。サイズ 525,261 byte に合わせてコメントで埋める |
| `hd44780u_b04.bin`（mulcd.zip） | HD44780 の文字 ROM（Yamaha 独自マスク B04） | Adafruit GFX の 5×7 フォント（BSD）から生成。0x80 以上の独自図形文字は再現できない |
| `sin-table.bin`（swp30.zip） | SWP30 の LFO 用 1/4 波 sin テーブル 32768 × 16bit | 数式で近似。単純な sin では CRC が一致しないので実チップは何らか加工されたテーブルらしい |

ハッシュ不一致の警告は出るが動作する。

## 7. MAME で動作確認

```powershell
tools\run_mame.ps1 -ListMidi
tools\run_mame.ps1 -MidiIn "loopMIDI Port"
```

詳細は `tools/run_mame.ps1` のヘッダを参照。
