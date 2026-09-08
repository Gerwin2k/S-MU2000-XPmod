# 画面

`gui.exe` と VST3 の画面は**同じもの**（`src/ui/panel.*`）。GDI だけで描いていて、
外からは HDC を 1 枚渡すだけ。面が 2 つある。

```
make            gui.exe も一緒に作る
build/gui.exe <rom ディレクトリ> [--midi 番号] [--latency ミリ秒] [--size 1400x360]
build/gui.exe --list                      MIDI 入力の一覧
build/gui.exe <rom> --boot --shot 絵.png   窓を出さずに絵だけ書き出す
```

## パネルの面

実機のフロントパネル。

| もの | 中身 |
|---|---|
| LCD | 本物の HD44780。字の絵は `roms/hd44780u_b04.bin` から引く。2 行 24 桁 |
| LED | 6 個。firmware が点けたとおり |
| ボタン | 35 個。MAME の `mu500` の入力ポートと同じ配線 |
| VALUE ダイヤル | **ホイールで回す**。実機にダイヤルは無く VALUE −/+ のボタンなので、回した分だけ 30ms ずつ叩いている |
| VOLUME | 音源の外で掛ける。VST3 では Output パラメータと同じ値 |

キーボードでも押せる（MAME の割り当てと同じ）。

```
A=PLAY  E=EDIT  U=UTIL  F=EFFECT  S=MUTE/SOLO  [ ]=PART−/+
−  ==VALUE−/+  BackSpace=EXIT  Enter=ENTER  , .=SELECT◀▶
Q=SEQ  Z=AUDITION  X=SELECT  M=SAMPLING/MODE
```

LCD には firmware が書いたものがそのまま出る。起動直後は `Battery Low!`、
EXIT と PLAY を押すと `◀000▶001 GrandP #01` のような演奏画面になる。

## エディタの面

SOL2 の XG エディタに倣った面。パートを 1 つ選び、そのパートのつまみを動かす。

**音源には手を入れていない。MIDI を送っているだけ**。つまみはすべて XG の
コントロールチェンジで決まっているものなので、実機に送るのと同じことをしている。
だからパネルから触った結果とも矛盾しない。

| 段 | つまみ |
|---|---|
| 1 | Volume(7) / Pan(10) / Expression(11) / Reverb(91) / Chorus(93) / Variation(94) |
| 2 | Cutoff(74) / Resonance(71) / Attack(73) / Decay(75) / Release(72) / Vib Rate(76) |
| 3 | Vib Depth(77) / Vib Delay(78) / Porta(5) / Modulation(1) / Bank(0) / Program |

つまみは上下にドラッグ、またはホイール。`XG リセット` は XG システムオン
（`F0 43 10 4C 00 00 7E 00 F7`）を送る。

覚えている値は「この画面から送った値」。音源から読み返す術が無いので、
パネル側で音を変えると表示とずれる。SOL2 のエディタも同じ立て付け。

### 効いていることの確かめ方

エディタで Program を 19 にしてからパネルの面に戻ると、LCD が
`◀000▶019 RockOrg#01` に変わる。画面 → bridge → 音源 → firmware → LCD と
一周しているのが目で見える。

## 中の作り

```
ui::panel     配置・描画・当たり判定。面の切り替えもここ
ui::bridge    画面と音源のあいだ。ボタンは atomic のビット、
              MIDI は輪、LCD の写しは seqlock。錠は使わない
ui::driver    音声スレッド側。ボタンを音源へ、MIDI を音源へ、写しを画面へ
```

音源は音声スレッドが回しているので、画面から直接触ってはいけない。
触れ合うのは bridge だけ。

## 窓を出さずに見た目を直す

`--shot` で PNG に書き出せる。窓を開けない場所（自動での確認、不具合の報告）で使う。

```
build/gui.exe roms --boot --shot panel.png --size 1400x360
```
