S-MU2000 — Yamaha MU2000 のソフトウェア音源（VST3）

ROM は同梱していない。自分の MU2000 から吸い出したものを用意して、
この Resources フォルダに roms.txt を作り、1 行目に置き場所を書く。

  例）  C:\Users\あなた\GitHub\MU2000\roms

その中には次が要る。

  mu2000_flash.bin        プログラム ROM 4MB
  dump\xv364a0.ic49 ほか  波形 ROM 8MB × 4
  standin\sin-table.bin   MEG が使う sin 表 64KB

環境変数 S_MU2000_ROMS でも指定できる。

読み込みの様子は次に残る。鳴らないときはここを見る。

  %LOCALAPPDATA%\S-MU2000\log.txt

挿してから音が出るまで数秒かかる。実機の電源投入と同じで、
MU2000 の firmware が起動を終えるまで待っている。

くわしくは https://github.com/tarboh/S-MU2000 の doc/vst3.md。
