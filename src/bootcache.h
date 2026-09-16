// license:BSD-3-Clause
//
// 起動後の状態を取っておいて、次からはそれを読み込む。
//
// firmware の起動は音の時間で 5〜8 秒かかる。1 台なら待てるが、DAW に
// 何枚も挿すとそのたびに黙るので、実用上いちばん痛い所になる。
//
// 中身は mu2000::save_state() がそのまま。**戻した状態は起動し切った姿と
// 1 ビットも違わない**（make test の statetest が毎回確かめている）ので、
// 音は起動を回したときとまったく同じになる。
//
// 置き場は <設定>/S-MU2000/boot/<鍵>.bin。鍵は
//   ・プログラム ROM（firmware）
//   ・起動に使ったワーク RAM（NVRAM。設定が違えば起動後の姿も違う）
//   ・波形 ROM（大きいので飛び飛びに拾う）
//   ・状態の形の版
// から作る。どれかが変われば鍵が変わるので、古い写しを読むことはない。
//
// 使い方は nvram と同じ並びで、reset() の直前に load()、起動し切った所で save()。
// カードを差したまま起動するときは使わない（カードの中身は状態に入らない）。

#ifndef S_MU2000_BOOTCACHE_H
#define S_MU2000_BOOTCACHE_H

#pragma once

#include "mu2000.h"
#include "nvram.h"

#include "compat/paths.h"

#include <cstdio>
#include <string>
#include <vector>

namespace smu2000 {
namespace bootcache {

// 鍵。プログラム ROM・ワーク RAM・波形 ROM・状態の版から作る。
// **reset() の前に、起動に使う RAM が入った状態で呼ぶこと**
inline u64 key(const mu2000 &mu)
{
	auto mix = [](u64 h, u8 b) {
		h ^= b;
		return h * 0x100000001b3ull;
	};
	u64 h = nvram::rom_key(mu);          // プログラム ROM 4MB
	const std::vector<u8> ram = mu.nvram();
	for (u8 b : ram)
		h = mix(h, b);
	// 波形 ROM は 32MB あって毎回なぞるには重い。頭・真ん中・終わりの
	// 4KB ずつと大きさだけ見る。差し替えに気づければ足りる
	if (const auto wave = mu.wave_rom()) {
		const size_t n = wave->size();
		for (int k = 0; k < 8; k++)
			h = mix(h, u8(n >> (k * 8)));
		const size_t spots[3] = { 0, n / 2, n > 4096 ? n - 4096 : 0 };
		for (size_t at : spots)
			for (size_t i = 0; i < 4096 && at + i < n; i++)
				h = mix(h, (*wave)[at + i]);
	}
	for (int k = 0; k < 4; k++)
		h = mix(h, u8(mu2000::state_version() >> (k * 8)));
	return h;
}

// 置き場。作れなければ空
inline std::string path(u64 k)
{
	const std::string base = smu2000::ensure_config_dir();
	if (base.empty())
		return {};
	const std::string dir = smu2000::join(base, "boot");
	if (!smu2000::ensure_dir(dir))
		return {};
	char name[32];
	std::snprintf(name, sizeof(name), "%016llx.bin", (unsigned long long)k);
	return smu2000::join(dir, name);
}

// 起動後の状態を読む。読めたら機械はもう起動し切った姿になっている。
// **reset() の代わりに呼ぶ**（reset() したあとに呼んでも構わない）
inline bool load(mu2000 &mu, u64 k)
{
	const std::string p = path(k);
	if (p.empty())
		return false;
	std::FILE *f = std::fopen(p.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<u8> buf(size > 0 ? size_t(size) : 0);
	const bool read_ok = !buf.empty() && std::fread(buf.data(), 1, buf.size(), f) == buf.size();
	std::fclose(f);
	if (!read_ok)
		return false;
	std::string err;
	if (!mu.load_state(buf.data(), buf.size(), err)) {
		std::fprintf(stderr, "起動の写しを読めない: %s\n", err.c_str());
		return false;
	}
	return true;
}

// 起動し切った所で残す。**起動に成功したときだけ呼ぶこと**
inline bool save(const mu2000 &mu, u64 k)
{
	const std::string p = path(k);
	if (p.empty())
		return false;
	const std::vector<u8> st = mu.save_state();
	// 書いている途中で落ちても壊れた写しを残さないよう、別名で書いてから置き換える
	const std::string tmp = p + ".new";
	std::FILE *f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(st.data(), 1, st.size(), f) == st.size();
	std::fclose(f);
	if (!ok) {
		std::remove(tmp.c_str());
		return false;
	}
	std::remove(p.c_str());
	return std::rename(tmp.c_str(), p.c_str()) == 0;
}

} // namespace bootcache
} // namespace smu2000

#endif // S_MU2000_BOOTCACHE_H
