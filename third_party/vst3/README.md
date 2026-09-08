# VST3 インターフェース定義

Steinberg の VST3 インターフェース定義。**MIT ライセンス**なので、この
リポジトリ（BSD-3-Clause）にそのまま取り込める。ライセンス条文は
LICENSE.txt にある。

出どころ: https://github.com/steinbergmedia/vst3_pluginterfaces

SDK 本体（`public.sdk` の補助クラス群）は GPLv3 と Steinberg の独自
ライセンスの二択なので**使っていない**。ここにあるのは口の定義だけで、
差し込む中身は `src/vst3/` に自前で書いてある。

`base` と `vst` だけを持ってきた。`gui` と `test` は要らない。
ヘッダが `#include "pluginterfaces/base/..."` と書いているので、
`third_party/vst3/pluginterfaces/` の形に置き、`-I third_party/vst3` で通す。

インターフェース番号（iid）の実体はどこかの翻訳単位で作らなければならず、
SDK ではそれが public.sdk 側にある。中身はマクロを並べるだけなので
`src/vst3/iids.cpp` に自前で書いた。
