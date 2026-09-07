// license:BSD-3-Clause
//
// MU2000 一台ぶんの組み立て。
//
// MAME の src/mame/yamaha/ymmu2000.cpp（mu500_state / mu1000_state /
// mu2000_state）に当たるもの。machine_config と address_map で書かれていた
// 配線を、素のコードに置き換えてある。

#ifndef S_MU2000_MU2000_H
#define S_MU2000_MU2000_H

#pragma once

#include "compat/mamecompat.h"
#include "compat/membus.h"
#include "mame/cpu/sh7042.h"
#include "mame/sound/swp30.h"

#include <string>

class mu2000
{
public:
	mu2000();
	~mu2000();

	// ---- ROM。どれも利用者が自分の実機から吸い出したもの

	// CPU から見えるままの 4MB（MU2000 リポジトリの roms/mu2000_flash.bin）
	bool load_program(const std::string &path);
	// 波形 ROM 32MB。ic49/ic50/ic53/ic54 を 32bit 語に組む
	bool load_wave(const std::string &dir);
	// MEG が使う sin 表。まだ実機から取れていないので代用品でもよい
	bool load_sintab(const std::string &path);

	void reset();

	// n サイクルぶん進める。周辺のイベントはこの中で挟む
	void run_cycles(u64 n);

	// 1 サンプル（44.1kHz 相当）ぶん進めて、DAC 出力を返す
	void run_sample(s32 &left, s32 &right);

	sh7043a_device &cpu()  { return *m_cpu; }
	swp30_device   &swpm() { return m_swpm; }
	swp30_device   &swps() { return m_swps; }

	const std::string &error() const { return m_error; }

	// SWP30 への書き込みを全部書き出す（MAME と突き合わせるため）
	void set_swp_trace(std::FILE *f, bool with_reads = false)
	{ m_swp_trace = f; m_swp_trace_reads = with_reads; }

private:
	void build_bus();
	void start_devices();

	machine_config  m_config;
	required_device<sh7043a_device> m_cpu_finder;
	sh7043a_device *m_cpu = nullptr;

	swp30_device m_swpm, m_swps;   // マスタ 0x800000 / スレーブ 0x802000
	mem_bus      m_bus;

	std::vector<u8>  m_prog;        // プログラム ROM 4MB
	std::vector<u8>  m_wave;        // 波形 ROM 32MB
	std::vector<u16> m_sintab;
	std::vector<u8>  m_ram;         // ワーク RAM  0x400000-0x43ffff
	std::vector<u8>  m_dram;        // DRAM        0x1000000-0x107ffff
	std::vector<u8>  m_iram;        // CPU 内蔵    0xfffff000-0xffffffff
	std::vector<u8>  m_sampram;     // SWP30 のサンプリング RAM

	// パネルまわり。音を出すのに要らないが、firmware は起動時に触る
	u8 m_ledsw1 = 0, m_ledsw2 = 0;
	u16 m_pe = 0;

	std::string m_error;
	std::FILE  *m_swp_trace = nullptr;
	bool        m_swp_trace_reads = false;

	// 44.1kHz 1 サンプルあたりの CPU サイクル。端数は繰り越す
	u64 m_cycle_debt = 0;
};

#endif // S_MU2000_MU2000_H
