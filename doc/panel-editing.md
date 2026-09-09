# パネルの絵の直しかた

フロントパネルの絵は画像ではなく、その場で描いている。位置・大きさ・色は
**`panel.txt` という文字ファイルに追い出してある**ので、作り直さずに直せる。

## 手順

### 1. いまの配置を書き出す

```bash
build/gui.exe --dump-layout panel.txt
```

組み込みの配置（実機の写真から採寸したもの）がそのまま出てくる。
**これを出発点にする。**

### 2. 物差しを出す

```bash
build/gui.exe --shot panel.png --grid --layout panel.txt --size 1400x560
```

ROM は要らないので**0.1 秒くらいで絵が出る**。`--grid` を付けると
論理座標の方眼が重なる。50 ごとに細い線、100 ごとに濃い線と
`100,200` のような数字。直したい部品の位置を読み取る。

### 3. 直して、見る

`panel.txt` を書き換えて、もう一度 2 を走らせるだけ。**作り直しは要らない。**

動かしながら直すなら、窓を出しておいて

```bash
build/gui.exe <rom ディレクトリ> --layout panel.txt
```

`panel.txt` を保存してから**窓で F5 を押すと読み直す**。

### 4. 置き場

`--layout` を付けないときは、この順に探して最初に見つかったものを読む。

1. いま居るところの `panel.txt`
2. `gui.exe` と同じところの `panel.txt`
3. `%LOCALAPPDATA%\S-MU2000\panel.txt`

どこにも無ければ組み込みの配置を使う。**VST3 は 3 番目だけを見る**ので、
DAW でも同じ絵にしたければそこに置く。

## 書き方

`#` から行末は覚え書き。ただし `#rrggbb` は色なので残る。

### 座標のきまり

**論理座標 1000 × 400**。窓の大きさが変わっても、この 1000 × 400 が
窓いっぱいに収まるように一律で拡大縮小されるだけ。**窓の大きさは
気にしなくてよい**。縦 0 - 385 が本体、385 - 400 は面を切り替える帯。

### 位置

```
body_h 385                # 本体の高さ
lcd    240 42 439 135     # LCD の窓  x y 幅 高さ

cat.x  288 354 419 482 545 607    # 音色カテゴリの列（6 列）
cat.y  219 266 310                # その行（3 行）
cat.size 52 20                    # 押すところの幅と高さ

mode.play 752 66          # 右上の丸ボタン。中心の座標
mode.edit 806 66          # 名前は play edit util effect sampling seq
mode.r 11 5               # ボタンの半径と、中の LED の半径

nav.mute_solo 840 44 48 34    # 右端の四角いボタン。x y 幅 高さ
                              # 名前は mute_solo part- part+ enter
                              # select- select+ exit value- value+

round.select   694 262 22 22  # 小さい丸ボタン。select と audition

dial 893 268 58           # 大きなダイヤル  中心 x y と半径
plg  524 37 341           # MU / PLG-1..3 の表示灯  左端 間隔 y
columns.y 186             # 窓の下の札（PART VOL EXP …）の高さ

low.x 0 12 30 48 55 61 70 78 86 93 0    # LCD 下段の並び
low.w 10 15 15 2 2 8 7 7 7 8 3          # 単位は上段の点 1 つぶん
```

`low.x` `low.w` の並びは左から
`01` / `A01` / 楽器のかたち / VOL / EXP / PAN / REV / CHO / VAR / KEY / モード。
くわしくは [doc/lcd-segments.md](lcd-segments.md)。

### 飾り

ボタンでも LCD でもない、ただ描くだけのもの。**1 つでも書くと、
書いたものだけになる**（組み込みの飾りは消える）。上から順に描く。

```
text x y 幅 高さ 書体 揃え 色 "文字"
disc 中心x 中心y 半径 面の色 ふちの色 線の太さ
box  x y 幅 高さ 角の丸み 面の色 ふちの色
```

* 書体 … `small` `label`
* 揃え … `left` `center` `right` `leftmid` `centermid` `leftwrap` `centerwrap`
* 色 … `ink` `face` `key` `keyedge` `keydown` `jack` `jackedge` `socket`
  `slot` `slotedge` `slotink` `black` `white`、または `#rrggbb`
* 文字の中の `\n` で改行（揃えを `leftwrap` か `centerwrap` に）

例。

```
text 10 6 170 26 label leftmid ink "YAMAHA"
disc 32 74 19 jack jackedge 2
box 57 336 201 21 2 slot slotedge
```

## 変なことを書いたら

その行だけ飛ばして、`何行目: 理由` と教えてくれる。読めた行は反映される
ので、直して F5 を押せばよい。ファイルごと無くしても組み込みの配置に
戻るだけで、動かなくなることは無い。

## 表に無いもの

LCD の中身（点の大きさ、目盛りの帯、下段のセグメント）は `lcd` の四角から
**計算で出している**。窓を動かしたり大きさを変えれば、中身は全部ついてくる。

押せる場所も同じ表から作られる。**絵と当たり判定を別々に直す必要は無い。**

エディタ面とエフェクト面はまだコードの中（`src/ui/editor.cpp`、
`src/ui/effects.cpp`）。

## それでもコードを触るなら

組み込みの配置は `src/ui/layout.cpp` の `layout::layout()`。
描く仕組みは `src/ui/panel.cpp`、描くための小物は `src/ui/draw.h`
（`fill` `round_box` `disc` `text_in`）。
直したら `mingw32-make`、VST3 は `make install-vst3` で入れ直す。
