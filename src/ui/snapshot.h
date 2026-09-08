// license:BSD-3-Clause
//
// 音源から画面へ渡すもの。音声スレッドが作り、GUI スレッドが読む。
// panel.h と bridge.h の両方が要るので、ここだけ分けてある。

#ifndef S_MU2000_UI_SNAPSHOT_H
#define S_MU2000_UI_SNAPSHOT_H

#pragma once

#include "compat/mamecompat.h"

namespace ui {

constexpr int LOGICAL_W = 1000;
constexpr int LOGICAL_H = 400;

// LCD。firmware は 2 行 40 桁で使う。実機の窓に出るのは 24 桁ぶんで、
// そのうち左 20 桁が文字の並ぶところ、残り 4 桁が絵記号のセグメント部
constexpr int LCD_ROWS = 2, LCD_COLS = 24;

// 実機の窓は、DDRAM の桁がそのまま横一列に並んでいるのではない。
//
//   0-16   上の面。メータ 9 桁（1 マス 2 本で 18 本）＋ 文字 8 桁
//   17-19  下の面の左。行 0 が「01」（2 桁）、行 1 が「A01」（3 桁）
//   20-23  下の面。楽器のかたち。両行で 1 枚の絵
//
// 押して確かめた（doc/gui.md）
constexpr int TOP_COLS = 17;

// 下の面に並ぶもの。左から
enum : int {
	LOW_PART = 0, LOW_BANK, LOW_ICON, LOW_VOL, LOW_EXP, LOW_PAN,
	LOW_REV, LOW_CHO, LOW_VAR, LOW_KEY, LOW_MODE, LOW_COUNT
};
constexpr int CELL_W = 5, CELL_H = 8;

struct snapshot {
	u8   dots[LCD_ROWS * LCD_COLS * CELL_H] = {};   // 各バイトの下位 5bit
	u16  leds = 0;
	bool lcd_on = false;
	bool ready = false;          // 起動が終わったか
	char message[96] = {};       // 起動中／ROM が無い等。空なら出さない
};

} // namespace ui

#endif // S_MU2000_UI_SNAPSHOT_H
