// license:BSD-3-Clause
//
// MAME 依存を肩代わりする最小限の層。
//
// MAME のデバイス実装（swp30.cpp など）は BSD-3-Clause なので流用できるが、
// device_t / address_space / sound_stream といった MAME 本体の仕組みに依存している。
// ここではそれらのうち **実際に使われている部分だけ** を用意する。
//
// 方針:
//   - アルゴリズム本体には手を触れない
//   - セーブステート、デバッガ、逆アセンブラは作らない（ソフトシンセに要らない）
//   - メモリ空間はフラットな配列。バンク切り替えもハンドラも無い
//   - 動的再コンパイラ(DRC)は使わない。インタプリタ経路だけを使う

#ifndef S_MU2000_MAMECOMPAT_H
#define S_MU2000_MAMECOMPAT_H

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ---- MAME の整数型 ---------------------------------------------------------

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using offs_t = std::uint32_t;

// MAME のソースに散っている属性・注釈。中身は要らない
#define ATTR_COLD
#define ATTR_FORCE_INLINE inline

// ---- セーブステートとログ ---------------------------------------------------
//
// save_item は swp30.cpp だけで 100 箇所あるが、すべて初期化関数の中の登録処理。
// セーブステートを作らないので、まるごと消してよい。

#define NAME(x) x
#define save_item(...)      do {} while(0)
#define state_add(...)      (*this)

namespace smu2000 {
extern bool g_verbose;   // 既定では黙る。デバッグ時だけ true にする
}

#define logerror(...)                                            \
	do {                                                         \
		if (::smu2000::g_verbose) std::fprintf(stderr, __VA_ARGS__); \
	} while(0)

// ---- メモリ空間 -------------------------------------------------------------
//
// MAME の memory_access<AddrBits, DataWidth, AddrShift, Endian>::cache の代わり。
// 相手はどれも素の配列（波形 ROM、リバーブ RAM、MEG のプログラム）なので、
// 範囲外を折り返すだけの単純な実装で足りる。
//
// AddrShift の意味は MAME と同じ:
//    -1 : アドレス 1 = 16bit ワード 1 個
//    -2 : アドレス 1 = 32bit ワード 1 個
//    -3 : アドレス 1 = 64bit ワード 1 個

template <int AddrBits, int DataWidth, int AddrShift>
class flat_space
{
public:
	void set(const void *base, size_t bytes)
	{
		m_base  = reinterpret_cast<const u8 *>(base);
		m_bytes = bytes;
		m_mask  = bytes ? (bytes - 1) : 0;   // 2 の冪でない場合は下の wrap() で丸める
		m_pow2  = bytes && !(bytes & (bytes - 1));
	}

	u16 read_word(offs_t addr) const
	{
		return read_at<u16>(offset_of(addr, 2));
	}

	u32 read_dword(offs_t addr) const
	{
		return read_at<u32>(offset_of(addr, 4));
	}

	u64 read_qword(offs_t addr) const
	{
		return read_at<u64>(offset_of(addr, 8));
	}

	// 書き込みは可変な領域（リバーブ RAM）でのみ使う
	void set_writable(void *base, size_t bytes)
	{
		set(base, bytes);
		m_write = reinterpret_cast<u8 *>(base);
	}

	void write_word(offs_t addr, u16 data)
	{
		if (m_write) std::memcpy(m_write + offset_of(addr, 2), &data, 2);
	}

	void write_dword(offs_t addr, u32 data)
	{
		if (m_write) std::memcpy(m_write + offset_of(addr, 4), &data, 4);
	}

private:
	// AddrShift 分だけ左シフトしてバイト単位にし、領域内へ折り返す
	size_t offset_of(offs_t addr, size_t elem) const
	{
		size_t byte = size_t(addr) << (-AddrShift);
		return wrap(byte, elem);
	}

	size_t wrap(size_t byte, size_t elem) const
	{
		if (!m_bytes) return 0;
		if (m_pow2)   return byte & m_mask & ~(elem - 1);
		return (byte % m_bytes) & ~(elem - 1);
	}

	template <typename T> T read_at(size_t byte) const
	{
		T v = 0;
		if (m_base) std::memcpy(&v, m_base + byte, sizeof(T));
		return v;
	}

	const u8 *m_base  = nullptr;
	u8       *m_write = nullptr;
	size_t    m_bytes = 0;
	size_t    m_mask  = 0;
	bool      m_pow2  = false;
};

// ---- 音声バッファ -----------------------------------------------------------
//
// MAME の sound_stream の代わり。swp30.cpp が使うのは get / put_int_clamp の 2 つだけ。

// swp30 は 1 サンプルにつき 1 回 sound_stream_update を呼ばれる作りで、
// index には常に 0 が渡る。出力は DAC 4 本 + 外部シリアル 16 本の計 20 本。
class sound_buffer
{
public:
	static constexpr int OUTPUTS = 20;   // 0-3 = DAC, 4-19 = MELO
	static constexpr int INPUTS  = 16;   // MELI（MU2000 では未使用）

	void reset(int inputs, int outputs, int samples)
	{
		m_samples = samples;
		m_in.assign(size_t(inputs) * samples, 0.0f);
		m_out.assign(size_t(outputs) * samples, 0.0f);
		m_inputs  = inputs;
		m_outputs = outputs;
	}

	int samples() const { return m_samples; }

	// 入力（外部シリアル入力。MU2000 では使わないので常に 0）
	float get(int channel, int index) const
	{
		if (channel >= m_inputs || index >= m_samples) return 0.0f;
		return m_in[size_t(channel) * m_samples + index];
	}

	// 出力。value / scale を -1.0〜+1.0 に収めて書き込む
	void put_int_clamp(int channel, int index, s32 value, s32 scale)
	{
		if (channel >= m_outputs || index >= m_samples) return;
		s32 v = std::clamp<s32>(value, -scale, scale - 1);
		m_out[size_t(channel) * m_samples + index] = float(v) / float(scale);
	}

	const float *output(int channel) const
	{
		return m_out.data() + size_t(channel) * m_samples;
	}

private:
	std::vector<float> m_in, m_out;
	int m_samples = 0, m_inputs = 0, m_outputs = 0;
};

#endif // S_MU2000_MAMECOMPAT_H
