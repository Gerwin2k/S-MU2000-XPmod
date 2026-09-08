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

// LCD の窓。firmware は 2 行 40 桁で使うが、出ているのは 24 桁ぶん
constexpr int LCD_ROWS = 2, LCD_COLS = 24;
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
