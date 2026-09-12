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

#include "state.h"
#include "compat/mamecompat.h"
#include "compat/membus.h"
#include "mame/cpu/sh7042.h"
#include "mame/sound/swp30.h"
#include "mame/machine/sci4.h"
#include "mame/video/hd44780.h"

#include <cstdio>
#include <atomic>
#include <array>
#include <deque>
#include <memory>
#include <thread>
#include <string>

class mu2000
{
public:
	mu2000();
	~mu2000();

	// ---- ROM。どれも利用者が自分の実機から吸い出したもの

	// ROM は読むだけなので、何台の MU2000 で分け合っても構わない。
	// 一度読んだものを渡せば、読み直しも 36MB の複製もしなくて済む
	using u8rom  = std::shared_ptr<std::vector<u8>>;
	using u16rom = std::shared_ptr<std::vector<u16>>;
	u8rom  program_rom() const { return m_prog; }
	u8rom  wave_rom()    const { return m_wave; }
	u16rom sintab_rom()  const { return m_sintab; }
	void set_program_rom(u8rom p);
	void set_wave_rom(u8rom p);
	void set_sintab_rom(u16rom p);
	u8rom  lcd_font()    const { return m_lcd_font; }
	void set_lcd_font(u8rom p);
	// 代用フォントに欠けているレベルメータの字を規則から起こす
	static void fill_missing_glyphs(std::vector<u8> &rom);

	// CPU から見えるままの 4MB（MU2000 リポジトリの roms/mu2000_flash.bin）
	bool load_program(const std::string &path);
	// 波形 ROM 32MB。ic49/ic50/ic53/ic54 を 32bit 語に組む
	bool load_wave(const std::string &dir);
	// MEG が使う sin 表。まだ実機から取れていないので代用品でもよい
	bool load_sintab(const std::string &path);
	// LCD の文字の絵（HD44780U B04 の CGROM 4KB）。無くても音は出る
	bool load_lcd_font(const std::string &path);

	void reset();

	// 状態の保存と復元。**機械まるごと**（CPU・RAM・SWP30・LCD・タイマ）。
	// ROM は入れないので、戻すときは同じ ROM を積んでおくこと。
	// 正しさは「戻した続きの音が、戻さず走り続けた音と 1 バイトも
	// 違わないこと」で確かめる（tools/state_test.py）
	std::vector<u8> save_state() const;
	void state(state_io &s);
	bool load_state(const u8 *p, size_t n, std::string &err);

	// n サイクルぶん進める。周辺のイベントはこの中で挟む
	void run_cycles(u64 n);

	// MIDI IN A の受信が有効になったか。firmware が起動を終えた印。
	// これを待たずに流すと、曲頭のリセットや音色指定が全部捨てられる
	bool midi_ready() const { return m_cpu->sci(0)->rx_enabled(); }

	// MIDI IN A に 1 バイト送る。実機と同じく 31250bps の直列で流れる
	void midi_in(u8 byte) { m_midi_queue.push_back(byte); }
	bool midi_idle() const { return m_midi_bit < 0 && m_midi_queue.empty(); }

	// スレーブの SWP30 を別スレッドで回すか。
	// 2 個の SWP30 は 1 サンプルの中では互いに独立している（相手の出力は
	// 前サンプルのものしか使わない）ので、並べて走らせても結果は変わらない
	void set_threaded(bool on);

	// 1 サンプル（44.1kHz 相当）ぶん進めて、DAC 出力を返す。
	// 値は MAME 内部と同じ目盛りで、全振幅が DAC_FULL_SCALE。
	// 16bit にするときは >> 2（MAME の put_int_clamp(..., 1<<17) と同じ）
	static constexpr s32 DAC_FULL_SCALE = 1 << 17;
	void run_sample(s32 &left, s32 &right);

	sh7043a_device &cpu()  { return *m_cpu; }
	swp30_device   &swpm() { return m_swpm; }
	swp30_device   &swps() { return m_swps; }
	hd44780_device &lcd()  { return m_lcd; }

	// ---- フロントパネル

	// パネルのボタン。MAME の mu500 の入力ポートと同じ並び。
	// firmware は m_ledsw1 で行を選び、押されている桁を 0 で読む
	enum class button {
		strings, bass, guitar, organ, chrom_perc, piano,
		synth_pad, synth_lead, pipe, reed, brass, ensemble,
		drum, model_excl, sfx, percussive, ethnic, synth_effects,
		part_plus, part_minus, mute_solo, effect, util, edit, play,
		value_plus, value_minus, exit, select_right, select_left, enter, seq,
		audition, select, sampling_mode,
		count
	};
	static const char *button_name(button b);
	void set_button(button b, bool pressed);
	bool button_pressed(button b) const;

	// 前面の大きなダイヤル（ロータリーエンコーダ）。正が右回り。
	// 線はポート A の bit17（A 相）と bit16（B 相）。
	// firmware は 2.5ms ごとにここを読み、**A が立っていれば 1 目盛り**、
	// 向きは B（0 で増、1 で減）で決める。実測でそう決まっている。
	// 走査 1 回につき 1 目盛りなので、最大 400 目盛り/秒
	void turn_encoder(int detents) { m_enc_pending += detents; }
	bool encoder_busy() const { return m_enc_pending != 0; }

	// パネルの LED 10 個。MAME の mulcd_device::set_leds と同じ並び
	u16 leds() const;

	const std::string &error() const { return m_error; }

	void print_swp_widths() const
	{ std::printf("SWP30 アクセス: 書き byte %llu / word %llu / dword %llu、読み byte %llu\n",
	              (unsigned long long)m_swp_w8, (unsigned long long)m_swp_w16,
	              (unsigned long long)m_swp_w32, (unsigned long long)m_swp_r8); }

	// SWP30 への書き込みを全部書き出す（MAME と突き合わせるため）
	void set_swp_trace(std::FILE *f, bool with_reads = false)
	{ m_swp_trace = f; m_swp_trace_reads = with_reads; }

private:
	void build_bus();
	void start_devices();

	machine_config  m_config;
	running_machine m_machine;   // 時計とタイマの置き場
	required_device<sh7043a_device> m_cpu_finder;
	sh7043a_device *m_cpu = nullptr;

	swp30_device m_swpm, m_swps;   // マスタ 0x800000 / スレーブ 0x802000
	required_device<sci4_device> m_sci4_finder;
	sci4_device *m_sci4 = nullptr;   // PLG ボード用 0xf00000
	mem_bus      m_bus;

	u8rom  m_prog;                  // プログラム ROM 4MB
	u8rom  m_wave;                  // 波形 ROM 32MB
	u16rom m_sintab;
	std::vector<u8>  m_ram;         // ワーク RAM  0x400000-0x43ffff
	std::vector<u8>  m_dram;        // DRAM        0x1000000-0x107ffff
	std::vector<u8>  m_iram;        // CPU 内蔵    0xfffff000-0xffffffff
	std::vector<u8>  m_sampram;     // SWP30 のサンプリング RAM

	// パネルまわり。音そのものには関わらないが、firmware は起動時に触る。
	// LCD は「要らない」ように見えて必要だった。firmware は初期化のたびに
	// ビジーフラグが立つのを確かめており、常に空いていると先へ進まない
	hd44780_device m_lcd;
	u8  m_ledsw1 = 0, m_ledsw2 = 0;
	// 押されているボタン。行 6 × 桁 8。押すと 0 になる
	u8  m_sws[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	u8   ledsw_r() const;

	// あと何目盛りぶん送るか。符号が向き。読まれるたびに 1 ずつ減る
	int m_enc_pending = 0;
	bool m_enc_high = true;
	u16 m_pe = 0;
	u8rom m_lcd_font;             // HD44780 の CGROM 4KB

	u16  lcd_port_r();
	void lcd_port_w(u16 data);

	// SCI4 の割り込み。0 と 1 は OR して CPU の IRQ0 へ（MAME の input_merger）
	int  m_sci_irq[2] = { 0, 0 };
	void update_sci_irq();

	std::string m_error;
	// SWP30 へのアクセス幅の内訳（byte 幅があると片側が壊れる）
	u64 m_swp_w8 = 0, m_swp_r8 = 0, m_swp_w16 = 0, m_swp_w32 = 0;

	std::FILE  *m_swp_trace = nullptr;
	bool        m_swp_trace_reads = false;

	// 44.1kHz 1 サンプルあたりの CPU サイクル。端数は繰り越す
	u64 m_cycle_debt = 0;
	// 命令の途中で止まれず走りすぎた分。次の呼び出しから引く
	u64 m_overrun = 0;

	// スレーブ用のスレッド。合図は atomic の回し合いで、錠は使わない。
	// 44100 回/秒の受け渡しなので、待つのは眠らずに回して待つ
	std::thread m_slave_thread;
	std::atomic<u64> m_slave_go{0}, m_slave_done{0};
	std::atomic<bool> m_slave_quit{false};
	s32 m_slave_l = 0, m_slave_r = 0;
	void slave_loop();

public:
	// 速さの手掛かり。1 サンプルあたり実行ループを何周したか
	u64 m_loops = 0, m_timer_fires = 0, m_event_fires = 0;
	// 区間ごとの所要時間（QueryPerformanceCounter の刻み）
	u64 m_t_cpu = 0, m_t_swpm = 0, m_t_swps = 0;
private:

	// MIDI IN A。バイトを 31250bps の直列に崩して RX 線に流す
	void midi_step(u64 now);
	std::deque<u8> m_midi_queue;
	int m_midi_bit = -1;      // -1 待ち / 0 スタート / 1-8 データ / 9 ストップ
	u8  m_midi_cur = 0;
	u64 m_midi_next = 0;
};

#endif // S_MU2000_MU2000_H
