# Linux

issue [#25](https://github.com/tarboh/S-MU2000/issues/25) から。**画面の要らない道具は Linux で動く**。
確かめたのは Ubuntu（WSL2、x86-64、g++ 15.2）。

```bash
sudo apt install build-essential
make
```

`build-linux/` に出来る（Windows と同じ作業用ディレクトリを共有していても、`.o` が混ざらないように分けてある）。

| 道具 | 中身 |
|---|---|
| `build-linux/verify` | SWP30 のレジスタと乱数の確認（ROM 要らず） |
| `build-linux/boot` | 起動の確認 |
| `build-linux/render` | MIDI ファイルを WAV に書き出す |
| `build-linux/panel` | フロントパネルを文字だけで動かす |
| `build-linux/statetest` | 状態の保存と復元 |
| `build-linux/blocktime` | 1 ブロックの所要時間を測る |

回帰試験も通る（ROM が要る。`python3-numpy` を入れておく）。

```bash
make test
```

2026-09-18 に Ubuntu で回した結果は、Windows と同じく全部「合」。**書き出した WAV は
基準の波形（`tests/`）と同じ**なので、音源そのものは Linux でも同じ音を出している。

## 中で直したところ

* `Makefile` … `uname -s` が `Linux` なら `PLATFORM := linux`。`CXX` は `g++`、`PYTHON` は `python3`、
  `-pthread` を足す。Linux から Windows 用を作るのは今までどおり `make CROSS=windows`
  （前は mingw-w64 が入っているだけでそちらが選ばれていたので、その自動判定より前に置いた）
* `src/compat/paths.h` … macOS 用だったところを `__APPLE__` で囲い、Linux の道を足した

| | Linux | macOS |
|---|---|---|
| 自分の実行ファイルの場所 | `/proc/self/exe` | `_NSGetExecutablePath` |
| 設定の置き場 | `$XDG_DATA_HOME/S-MU2000/`（無ければ `~/.local/share/S-MU2000/`） | `~/Library/Application Support/S-MU2000/` |
| 機械ぜんたいの置き場（ROM を置ける） | `/usr/local/share/S-MU2000/` | `/Library/Application Support/S-MU2000/` |

* `tools/run_tests.py` … 道具の置き場を `build/` 決め打ちから、`SMU_BUILD`（Makefile の `BUILD`）で
  受け取る形にした

## まだ無いもの

| | 要るもの |
|---|---|
| `live`（MIDI を受けてその場で鳴らす） | 音の出口と MIDI の口（ALSA） |
| `gui`（フロントパネルの窓） | 窓と描画（X11 か Wayland）と、`src/compat/gdi.h` の代わり |
| VST3・CLAP プラグイン | 上の音まわりと、Linux 用のバンドルの形 |

`render` で WAV に書き出すだけなら、いまのままで全部できる。

## WSL について

WSL2 でも上のとおり動く（試験も通る）。ただし **WSL2 には音を出す口が無い**ので、
`live` を移したとしても、そのままでは鳴らせない。音を出すなら本物の Linux か、
WSLg の PulseAudio 越しになる。
