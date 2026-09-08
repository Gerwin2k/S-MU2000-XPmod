# パネルの絵の直しかた

フロントパネルの絵は画像ではなく、**その場で描いている**
（`src/ui/panel.cpp`、GDI だけ）。位置はぜんぶファイル先頭の表に
まとまっているので、数字を書き換えて作り直せば直る。

## まず物差しを出す

```bash
build/gui.exe --shot panel.png --grid --size 1400x560
```

ROM は要らない。**0.1 秒くらいで出る**ので、直しては見て、を繰り返せる。
`--grid` を付けると論理座標の方眼が重なる。50 ごとに細い線、
100 ごとに濃い線と `100,200` のような数字が入る。

直したい部品の位置を方眼から読み取って、下の表を書き換える。

## 座標のきまり

**論理座標 1000 × 400** で置いてある。窓の大きさが変わっても、この
1000 × 400 が窓いっぱいに収まるように一律で拡大縮小されるだけ。
だから**窓の大きさは気にしなくてよい**。

* 縦 0 - 385 が本体。実機の縦横比はおよそ 2.6 : 1
* 縦 385 - 400 は面を切り替える帯で、本体の外

コードの中では

* `scale(x, y, w, h)` … 論理座標の四角 → 実際の窓の `RECT`
* `at(x, y)` … 論理座標の点 → 実際の窓の `POINT`

## どこに何があるか

すべて `src/ui/panel.cpp` の先頭、`namespace { }` の中。

| 表 | 何の位置か |
|---|---|
| `CAT_X[6]` `CAT_Y[3]` `CAT_W` `CAT_H` | 音色カテゴリのボタン 18 個。6 列 × 3 行なので、列と行の座標だけ持っている |
| `CAT_LABEL[18]` | その札の字 |
| `MODES[]` | 右上の丸ボタン 6 個（PLAY / EDIT / UTIL / EFFECT / SAMPLING / SEQ）。`{ ボタン, LED 番号, x, y, 札 }` |
| `NAV[]` | 右端の四角いボタン 9 個。`{ ボタン, x, y, 幅, 高さ, 札, 小さい札 }` |
| `ROUND[]` | SELECT と AUDITION の小さい丸ボタン |
| `DIAL_X` `DIAL_Y` `DIAL_R` | 大きなダイヤルの中心と半径 |
| `COLUMNS[]` | 窓の下に印刷されている札（PART / VOL / EXP …）。位置は LCD の下段の並びから取るので、`at` に並びの名前を書く |
| `LOW_X[]` `LOW_W[]` | LCD の**下段**に並ぶものの位置と幅。単位は上段の点 1 つぶん（[doc/lcd-segments.md](lcd-segments.md)） |

LCD の窓そのものは `panel::resize()` の中。

```cpp
m_lcd = scale(240, 42, 439, 135);      // x, y, 幅, 高さ
```

窓の中身（点の大きさ、目盛りの帯、下段）は、この四角から計算で出している。
**窓を動かしたり大きさを変えれば、中身は全部ついてくる。**

表に入っていないもの（`YAMAHA` の字、`MU2000 TONE GENERATOR`、
A/D INPUT のつまみ、電源、MIDI IN A、PHONES、カードの差し込み口、
`GM2 XG PLG` などの札）は `panel::paint_front()` の中に直接書いてある。
方眼で位置を読んで、そこの数字を直す。

## 色

`src/ui/panel.cpp` の先頭に本体の色。

```cpp
const COLORREF PANEL_FACE = RGB(196, 189, 170);   // 本体の面
const COLORREF PANEL_INK  = RGB(46, 44, 40);      // 印刷の字
const COLORREF KEY_FACE   = RGB(216, 205, 165);   // ボタンの面
const COLORREF KEY_EDGE   = RGB(126, 118, 92);    // ボタンのふち
const COLORREF KEY_DOWN   = RGB(150, 140, 95);    // 押したとき
```

LCD と LED の色は `src/ui/draw.h`。

```cpp
LCD_BACK   窓の地の色
LCD_GHOST  消えている点。実物もうっすら見える
LCD_DOT    点いている点
LED_OFF / LED_ON
```

## 描くための小物

`src/ui/draw.h` にある。GDI を直に触らなくても、たいていはこれで足りる。

| | |
|---|---|
| `fill(dc, rect, 色)` | 塗りつぶす |
| `round_box(dc, rect, 面, ふち, 角の丸み)` | 角を落とした四角。ボタンはこれ |
| `disc(dc, cx, cy, r, 面, ふち, 線の太さ)` | 丸。つまみやジャックはこれ |
| `text_in(dc, rect, 字, 色, 書体, 揃え)` | 字。揃えは `DT_CENTER` など |

## 直したら

```bash
mingw32-make
build/gui.exe --shot panel.png --grid --size 1400x560
```

見て、直して、もう一度。動かして確かめるときは

```bash
build/gui.exe C:\Users\gugug\GitHub\MU2000\roms
```

## 気をつけること

* **押せる場所は表から自動で作られる**（`build_spots()`）。表の数字を
  直せば、絵と当たり判定は勝手に揃う。別々に直す必要はない
* **VST3 の画面も同じ `panel.cpp` を使っている**。直したら
  `mingw32-make` のあと `make install-vst3` で入れ直す
* 窓の縦横比が 1000 : 400 と違うときは、余りが上下か左右に付く。
  本体は必ず真ん中に来る
