# MAME の絵から起こしたパネル

`mu2000-mame.svg` は MAME の `src/mame/layout/mu2000.lay` に埋め込まれて
いた絵をそのまま取り出したもの。

* **license:CC0-1.0**（パブリックドメイン）
* 作った人: hap、Felipe Sanches

CC0 なので条件なしで使ってよい。感謝して使わせてもらっている。

`panel.txt` は `tools/lay2panel.py` が同じ `.lay` から起こしたもので、
MAME の座標（1640 × 680）をこちらの論理座標（1000 × 400）へ移してある。
だから**絵とボタンの位置がぴたりと合う**。

```bash
build/gui.exe <rom ディレクトリ> --layout art/mame/panel.txt
```

作り直すには

```bash
python tools/lay2panel.py <mame>/src/mame/layout/mu2000.lay art/mame
```

ボタン・LED・つまみの絵は `../parts/` を指している。あちらはこちらの
書き起こし（BSD-3-Clause）。
