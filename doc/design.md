# S-MU2000 設計メモ

Yamaha MU2000 のソフトウェア音源。最終目標は **VST3 プラグイン**、最初の目標は
**MIDI を受けて音を出す exe**。

## なぜ MAME ではなく作るのか

MAME でも MU2000 は鳴る（実際に鳴らして、SWP30 のバグを 2 つ直した）。だが
**DAW で使うことはできない**。理由は音の正確さではなく、時計の持ち方にある。

```
MAME:         自分で時計を持ち、実時間に追いつこうとする  → ズレる
ソフトシンセ:  ホストが「N サンプルくれ」と要求する        → ズレようがない
```

実測でも、MAME は処理能力に 177% の余力がありながら**平均速度が 98% 台**から上がらず、
外部シーケンサから流すと遅れが溜まって破綻した。`-nosleep` `-priority` `-refreshspeed`
`-syncrefresh` `-speed` のどれでも 100% には届かない。フレーム単位（17ms）で処理が
落ちる瞬間があり、そこで音が途切れる。**これは MU2000 固有ではなく、MAME を Windows で
外部同期させて使うこと自体が想定外**という話。

オーディオコールバック駆動にすれば、この問題は構造的に消える。

## ライセンス

MAME 全体は GPL だが、**必要な個々のデバイス実装はすべて BSD-3-Clause**。

| ファイル | 由来 | ライセンス |
|---|---|---|
| `src/mame/sound/swp30.*` | MAME `src/devices/sound/` | BSD-3-Clause (Olivier Galibert) |
| `src/mame/cpu/sh*` | MAME `src/devices/cpu/sh/` | BSD-3-Clause |
| `src/mame/machine/sci4.*` | MAME `src/devices/machine/` | BSD-3-Clause |
| `src/mame/ymmu2000.cpp` | MAME `src/mame/yamaha/` | BSD-3-Clause |

**MAME 本体をリンクしてはいけない**（GPL 汚染で VST として配布できなくなる）。
取り込み元は MAME 0.289 相当（master 2026-09-06、コミット 1fb001f9）。

ROM は同梱しない。利用者が自分の実機から吸い出す。吸い出し手順は
[MU2000 リポジトリ](../../MU2000) にある（USB 経由で 32MB を約 36 分）。

## 構成

MU2000 の中身。SWP30 が **2 個** ある点に注意。

```
SH7043A (28MHz)          プログラム ROM 4MB, ワーク RAM 256KB
  ├ 内蔵 SCI ch0/ch1     MIDI IN A/B
  ├ 内蔵 SCI ch2/ch3     未使用
  ├ 0x800000  SWP30 マスタ
  ├ 0x802000  SWP30 スレーブ
  ├ 0xF00000  SCI4       PLG ボード用
  └ 0xC80000/0xE00000    LED ラッチ、パネル
SWP30 ×2                 AWM2 64ch + MEG(エフェクト DSP) + ミキサ
  ├ 波形 ROM 32MB
  └ サンプリング RAM
```

音を出すのに要らないもの: LCD、パネル、SmartMedia、PLG スロット、セーブステート、
デバッガ、画面描画。最初はすべて省く。

## 方針

MAME のソースは**改変して取り込む**。`device_t` や `address_space` を真似た互換層を
作るのではなく、**MAME 依存の配管だけを差し替え、アルゴリズム本体は無改変で残す**。

差し替える対象:

| MAME の仕組み | 置き換え |
|---|---|
| `device_t` / `machine_config` | 素の C++ クラスと直接の生成 |
| `address_space` / `memory_access<>::cache` | フラットな配列へのアクセス関数 |
| `sound_stream` | 呼び出し側が渡すバッファ |
| `required_region_ptr` | ROM を読み込んだ `std::vector` |
| `save_item` / `state_*` | 削除（セーブステートは作らない） |
| `drcumlsh`（動的再コンパイラ） | **使わない**。SWP30 も SH-2 もインタプリタ経路がある |

DRC を切るのは判断が要る点だが、MAME で 177% の余力があったので足りる見込み。
足りなければ後で考える。アルゴリズムを触らないので、正しさには影響しない。

## 検証

移植で一番怖いのは「音が変わったことに気づかない」こと。**基準がある**。

```
修正版 MAME で録音した WAV  ←→  S-MU2000 が出す WAV
```

同じ ROM・同じ MIDI ファイルで、サンプル単位で比較する。MU2000 リポジトリの
`tools/mame_audio_test.py` と `build/audio/*.wav` がそのまま回帰試験になる。

段階ごとの確認手段:

| 段階 | 確認のしかた |
|---|---|
| CPU が動く | 同じ firmware で、SWP30 レジスタへの書き込み列が MAME と一致するか |
| 音が出る | 単音のロングトーンを MAME の録音と突き合わせる |
| 曲が鳴る | 実曲 2 分半を録音して WAV を比較 |
| DAW で使える | 遅延と CPU 使用率を測る |

## 段階

1. **ビルドが通る** — 取り込んだソースが互換層の上でコンパイルできる
2. **CPU が走る** — firmware を読んで起動し、SWP30 レジスタ書き込みが MAME と一致
3. **音が出る** — MIDI ファイルを食わせて WAV に出し、MAME の録音と比較
4. **リアルタイム** — Windows の MIDI 入力を受けて、既定のデバイスから鳴る exe
5. **VST3** — プラグイン化

## 既知の未解決（MAME から引き継ぐ）

- **8bit 圧縮サンプルの展開器**が不正確。MAME 自身が「乗算器はバグっていて負に偏るが
  正確な挙動は不明。スケーリングが 0 か非 0 かで結果が変わり、隠れた状態がある」と
  TODO に明記している。ロングトーンで「ブーン」というノイズが乗る
- `sin-table.bin`（MEG が使う）と HD44780 のフォント ROM が未取得。実機から吸える
  可能性はある（SWP30 に内部レジスタを読む口がある）

ソフトシンセ化しても**音の正確さは上がらない**。上がるのは「DAW で使える」という一点。

## swp30 の依存調査（実測）

`swp30.cpp` 8,000 行のうち、MAME に依存しているのは以下だけだった。
アルゴリズム本体（AWM2 のサンプル読み出し・補間、エンベロープ、フィルタ、LFO、
MEG のインタプリタ、ミキサ）は**一行も触らずに済む**。

| 依存 | 出現 | 対処 |
|---|---|---|
| `save_item(NAME(x))` | 100 | マクロで空にする。セーブステートは作らない |
| `logerror(...)` | 14 | `fprintf(stderr)` へのマクロ。既定では黙る |
| `m_input_stream` / `m_output_stream` | 35 | `sound_buffer` に置換 |
| `state_add(...)` | 5 | マクロで空にする（デバッガ用） |
| `machine()` | 7 | 削除 |
| `space(...)` | 5 | `flat_space` を直接持たせる |
| `m_icount` / `set_icountptr` | 7 | 呼び出し側が回数を管理する |
| `drcuml` 一式 | — | **使わない**。`m_meg_drc_active = false` 固定でインタプリタ経路へ |
| `swp30_disassembler` | — | 削除（デバッガ用） |

実際に使われているメモリ API は 5 つだけ。

```
read_word / read_dword / read_qword / write_word / write_dword
```

音声 API は 2 つだけ。`sound_stream_update` は **1 サンプルにつき 1 回**呼ばれ、
`index` には常に 0 が渡る。出力は DAC 4 本 + 外部シリアル(MELO) 16 本の計 20 本、
入力は MELI 16 本（MU2000 では未使用）。

```
stream.get(channel, 0)                        入力
stream.put_int_clamp(channel, 0, value, scale) 出力
```

### レジスタ対応表

`address_map` は使わず、素の分岐に置き換える。レジスタは 64ch × 64 スロットの
規則的な格子で、ハンドラは `offset >> 6` でチャンネルを取り出している。

```
レジスタ番地 = チャンネル * 0x40 + スロット   （16bit 単位）
rchan(map, slot) → 全チャンネル分。ハンドラには offset = チャンネル << 6 が渡る
rctrl(map, idx)  → 単発。slot = 0x40*(idx>>1) | 0xe | (idx&1)
```

したがって `write16(addr, data)` / `read16(addr)` を書き、
`slot = addr & 0x3f` で分岐して既存のハンドラをそのまま呼べばよい。
**ハンドラ自体は変更不要。**

## SH-2 側の調査

### DRC の切り離し

`sh.cpp` は UML 命令が 520 箇所あり、行数では DRC が大半を占めていた。だが
**インタプリタ経路は完全に独立して存在する**（`m_isdrc` が false のときの
`execute_run()` → `execute_one()`）。境界も明確だった。

```
sh.cpp   1〜1936 行   インタプリタ（execute_one とその配下）
         1997 行〜    DRC 一色
```

`func_MAC_W` `func_DIV1` `func_ADDV` などは一見インタプリタ用に見えるが、
呼び出しはすべて `UML_CALLC` 経由で **DRC からしか使われていない**。
確認してから消した。

```
sh.cpp   4210 行 → 1898 行
sh2.cpp   803 行 →  510 行
```

### メモリバス

CPU の空間は SWP30 と性質が違う。SWP30 の相手（波形 ROM、リバーブ RAM）は
素の配列だったので `flat_space` で足りたが、**CPU は ROM・RAM・周辺レジスタが
混在する空間**を読み書きするので、番地で振り分ける必要がある。

`src/compat/membus.h` に用意した。MAME の `address_space` は汎用で重いので、
MU2000 に要る形だけにしてある。

| | |
|---|---|
| 連続領域（ROM / RAM） | 配列を直に指す。ROM は書き込み無視 |
| 周辺（SWP30 のレジスタなど） | 関数で受ける。16bit 単位 |
| エンディアン | ビッグ固定（SH-2） |

CPU が実際に使うのは `read/write` の `byte`/`word`/`dword` の **6 つだけ**
だった（`m_program->` と `m_decrypted_program->` の呼び出しを数えて確認）。

ここは**性能に効く場所**なので、後で線形探索が問題になったらページテーブルに
差し替える。いまは正しさを優先して素直に書いてある。

### どの内蔵周辺が要るか（実測で確定）

SH7042 は内蔵周辺を 20 個のサブデバイスとして持つ。全部移植すると重いので、
**MU2000 の firmware が実際に触る番地を ROM から走査して**判定した。
推測ではなく実データ。

リテラルプール（`MOV.L @(disp,PC)` と `MOV.W @(disp,PC)`）を解決して、
0xFFFF8000-0xFFFF9FFF を指すものを数えた結果:

| 周辺 | 番地 | 参照回数 | 判定 |
|---|---|---|---|
| MTU（タイマ） | 0xFFFF8200- | **974** | 必須。最多 |
| PORT | 0xFFFF8380- | **730** | 必須（LCD・LED・スイッチ走査） |
| CMT（タイマ） | 0xFFFF83D0- | 45 | 必須 |
| SCI0（MIDI IN A） | 0xFFFF81A0- | 43 | 必須 |
| SCI1（MIDI IN B） | 0xFFFF81B0- | 43 | 必須 |
| INTC（割り込み） | 0xFFFF8340- | 38 | 必須 |
| BSC（バス制御） | 0xFFFF8600- | 16 | 起動時のみ。空実装で可 |
| DMAC | 0xFFFF86F8 | 1 | 空実装で可 |
| ADC | 0xFFFF83E0- | **0** | **不要** |

ADC が 0 回なのは、HOST SELECT の読み取りをダウンローダ（0x001098）が
行っていて、本体 firmware は触らないため。

分類外の 0xFFFF8000 台は SH-2 のキャッシュ制御など。

したがって移植するのは **SCI, MTU, CMT, INTC, PORT の 5 つ**。
BSC と DMAC は番地だけ受けて何もしない空実装、ADC は配線しない。
