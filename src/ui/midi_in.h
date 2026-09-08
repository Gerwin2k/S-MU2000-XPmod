// license:BSD-3-Clause
//
// Windows の MIDI 入力を受けて、音声スレッドへバイトで渡す。
// 書くのは MIDI のコールバック、読むのは音声スレッドの一本ずつなので、
// 添字を atomic にしておけば錠は要らない（音声スレッドで錠を待つのは禁物）。

#ifndef S_MU2000_UI_MIDI_IN_H
#define S_MU2000_UI_MIDI_IN_H

#pragma once

#include "compat/mamecompat.h"

#include <atomic>
#include <string>
#include <vector>

namespace ui {

class midi_in
{
public:
	~midi_in() { close(); }

	static std::vector<std::string> list();

	// 番号が負なら開かない（MIDI 無しで動かす）
	bool open(int device, std::string &err);
	void close();

	bool  is_open() const { return m_handle != nullptr; }
	std::string device_name() const { return m_name; }
	u64   bytes() const { return m_bytes.load(); }

	// 音声スレッドから。溜まっているバイトを 1 つずつ取り出す
	bool pop(u8 &v);

	void push(u8 v);   // コールバックから

private:
	static constexpr size_t SIZE = 4096, MASK = SIZE - 1;
	u8 m_buf[SIZE] = {};
	std::atomic<size_t> m_read{0}, m_write{0};
	std::atomic<u64>    m_bytes{0};

	void       *m_handle = nullptr;   // HMIDIIN
	std::string m_name;
};

} // namespace ui

#endif // S_MU2000_UI_MIDI_IN_H
