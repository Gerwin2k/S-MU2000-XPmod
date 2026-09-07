// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

// Yamaha SWP30/30B, rompler/dsp combo

#ifndef MAME_SOUND_SWP30_H
#define MAME_SOUND_SWP30_H

#pragma once

// S-MU2000: MAME 本体の代わりに互換層を使う
#include "../../compat/mamecompat.h"


// S-MU2000: swp30_disassembler はデバッガ用なので削除した

class swp30_device
{
public:
	swp30_device();

	// S-MU2000: address_map の代わり。レジスタは 64ch x 64 スロットの格子
	u16  read16(offs_t addr);
	void write16(offs_t addr, u16 data);

	// 外から与えるメモリ
	void set_wave_rom(const void *base, size_t bytes);
	void set_sintab(const u16 *base, size_t count);

	void reset();

	// S-MU2000: swp30 が machine() を使うのは乱数のためだけ。
	// MAME と同じ数列でないと波形が一致しないので、同じ実装を持たせる。
	running_machine &machine() { return m_machine; }

	// 1 サンプル進めて、DAC 出力 2ch を返す
	void run_sample(s32 &left, s32 &right);

private:
	// S-MU2000: device_start/reset, state_*, disassembler, rom_region の宣言は削除

	struct streaming_block {
		static const std::array<u16, 0x400> pitch_base;
		static const std::array<s16,   256> dpcm_expand;
		static const std::array<std::array<s16, 0x800>, 2> interpolation_table;
		static const std::array<s32, 8> max_value;

		s32 m_start;
		s32 m_loop;
		u32 m_address;
		u16 m_pitch;

		s32 m_loop_size;
		s32 m_pos;
		s32 m_pos_dec;
		s16 m_dpcm_s0, m_dpcm_s1, m_dpcm_s2, m_dpcm_s3;
		u32 m_dpcm_pos;
		s32 m_dpcm_delta;

		bool m_first, m_finetune_active, m_done;
		s16 m_last;

		void clear();
		void keyon();
		std::pair<s16, bool> step(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s32 pitch_lfo);

		void start_h_w(u16 data);
		void start_l_w(u16 data);
		void loop_h_w(u16 data);
		void loop_l_w(u16 data);
		void address_h_w(u16 data);
		void address_l_w(u16 data);
		void pitch_w(u16 data);

		u16 start_h_r() const;
		u16 start_l_r() const;
		u16 loop_h_r() const;
		u16 loop_l_r() const;
		u16 address_h_r() const;
		u16 address_l_r() const;
		u16 pitch_r() const;

		void read_16(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);
		void read_12(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);
		void read_8(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);
		void read_8c(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);

		void dpcm_step(u8 input);
		void update_loop_size();
		void scale_and_clamp_one(s16 &val, u32 scale, s32 limit);
		void scale_and_clamp(s16 &val0, s16 &val1, s16 &val2, s16 &val3);

		std::string describe() const;
	};

	struct filter_block {
		u16 m_filter_1_a;
		u16 m_level_1;
		u16 m_filter_2_a;
		u16 m_level_2;
		u16 m_filter_b;

		s32 m_filter_1_p1;
		s32 m_filter_2_p1;
		s32 m_filter_p2;

		s32 m_filter_1_x1;
		s32 m_filter_1_x2;
		s32 m_filter_1_y0;
		s32 m_filter_1_y1;

		s32 m_filter_1_h;
		s32 m_filter_1_b;
		s32 m_filter_1_l;
		s32 m_filter_1_n;

		s32 m_filter_2_x1;
		s32 m_filter_2_x2;
		s32 m_filter_2_y0;
		s32 m_filter_2_y1;

		s32 m_filter_2_h;
		s32 m_filter_2_b;
		s32 m_filter_2_l;
		s32 m_filter_2_n;

		void clear();
		void keyon();
		s32 step(s16 input);

		void f1_chamberlin_step(s16 input);

		static s32 volume_apply(u8 level, s32 sample);

		u16 filter_1_a_r() const;
		u16 level_1_r() const;
		u16 filter_2_a_r() const;
		u16 level_2_r() const;
		u16 filter_b_r() const;

		void filter_1_a_w(u16 data);
		void level_1_w(u16 data);
		void filter_2_a_w(u16 data);
		void level_2_w(u16 data);
		void filter_b_w(u16 data);
	};

	struct iir1_block {
		s16 m_a[2][2];
		s16 m_b[2];
		s32 m_hx[2], m_hy[2];

		void clear();
		void keyon();
		s32 step(s32 input);

		template<u32 Filter> u16 a0_r() const;
		template<u32 Filter> u16 a1_r() const;
		template<u32 Filter> u16 b1_r() const;
		template<u32 Filter> void a0_w(u16 data);
		template<u32 Filter> void a1_w(u16 data);
		template<u32 Filter> void b1_w(u16 data);
	};

	struct envelope_block {
		// Hardware values readable through internal read on variable 0, do not change
		enum {
			ATTACK  = 0,
			DECAY1  = 1,
			DECAY2  = 2,
			RELEASE = 3
		};

		u16 m_attack;
		u16 m_decay1;
		u16 m_decay2;
		u16 m_release_glo;
		s32 m_envelope_level;
		u8  m_envelope_mode;

		void clear();
		void keyon();
		u16 status() const;
		bool active() const;
		u16 step(u32 sample_counter);
		void trigger_release();

		void attack_w(u16 data);
		void decay1_w(u16 data);
		void decay2_w(u16 data);
		void release_glo_w(u16 data);

		u16 attack_r() const;
		u16 decay1_r() const;
		u16 decay2_r() const;
		u16 release_glo_r() const;

		u16 level_step(u32 speed, u32 sample_counter);
	};

	struct lfo_block {
		u32 m_counter;
		u16 m_state;

		u16 m_r_type_step_pitch;
		u16 m_r_amplitude;

		u8 m_type;
		u8 m_step;
		u8 m_amplitude;
		bool m_pitch_mode;
		s8 m_pitch_depth;

		void clear();
		void keyon(running_machine &machine);
		u16 get_amplitude() const;
		s16 get_pitch() const;
		void step(running_machine &machine);

		void type_step_pitch_w(u16 data);
		void amplitude_w(u16 data);

		u16 type_step_pitch_r();
		u16 amplitude_r();
	};

	struct mixer_slot {
		std::array<u16, 3> vol;
		std::array<u16, 3> route;
	};

	struct meg_state {
	public:
		static const std::array<u32, 256> lfo_increment_table;

		swp30_device          *m_swp;
		std::array<u64, 0x180> m_program;
		std::array<s16, 0x180> m_const;
		std::array<u16,  0x80> m_offset;
		std::array<u16,  0x18> m_lfo;
		std::array<u32,  0x18> m_lfo_increment;
		std::array<u32,  0x18> m_lfo_counter;
		std::array<u16,     8> m_map;

		std::array<s32,  0x40> m_m;
		std::array<s32,  0x80> m_r;
		std::array<s16,     8> m_t;
		s64                    m_p;

		std::array<s32,     3> m_mw_value;
		std::array<u8,      3> m_mw_reg;
		std::array<s32,     3> m_rw_value;
		std::array<u8,      3> m_rw_reg;
		std::array<s32,     3> m_index_value;
		std::array<bool,    3> m_index_active;
		std::array<s32,     3> m_memw_value;
		std::array<s32,     3> m_memr_value;
		std::array<s16,     2> m_t_value;
		std::array<bool,    3> m_memw_active;
		std::array<bool,    3> m_memr_active;
		u32                    m_delay_3;
		u32                    m_delay_2;

		u32 m_ram_read, m_ram_write;
		s32 m_ram_index;
		u32 m_sample_counter;
		u16 m_program_address;
		u16 m_pc;
		int m_icount;
		u32 m_retval;

		u16 prg_address_r();
		void prg_address_w(u16 data);
		template<int Sel> u16 prg_r();
		template<int Sel> void prg_w(u16 data);
		template<int Sel> u16 map_r();
		template<int Sel> void map_w(u16 data);
		u16 const_r(offs_t offset);
		void const_w(offs_t offset, u16 data);
		u16 offset_r(offs_t offset);
		void offset_w(offs_t offset, u16 data);
		u16 lfo_r(offs_t offset);
		void lfo_w(offs_t offset, u16 data);
		void lfo_commit_w();
		void lfo_step();
		u32 get_lfo(int lfo);
		u32 resolve_address(u16 pc, s32 offset);

		static u16 revram_encode(u32 v);
		static u32 revram_decode(u16 v);
		static s16 m1_expand(s16 v);

		static void call_rand(void *ms);
		static void call_revram_encode(void *ms);
		static void call_revram_decode(void *ms);

		void step();
		void reset();
	};

	// S-MU2000: address_space の代わりにフラットな領域を直接持つ
	running_machine m_machine;
	region_ptr<u16> m_sintab;
	std::vector<u64> m_meg_program;      // MEG のプログラム（9bit 空間）
	std::vector<u16> m_reverb_ram;       // リバーブ RAM（18bit 空間）

	memory_access< 9, 3, -3, ENDIANNESS_LITTLE>::cache m_program_cache;
	memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache m_wave_cache;
	memory_access<18, 1, -1, ENDIANNESS_LITTLE>::cache m_reverb_cache;

	// S-MU2000: sound_stream の代わり。1 サンプル分だけ持つ
	sound_buffer m_buf;

	std::array<streaming_block, 0x40> m_streaming;
	std::array<filter_block,    0x40> m_filter;
	std::array<iir1_block,      0x40> m_iir1;
	std::array<envelope_block,  0x40> m_envelope;
	std::array<lfo_block,       0x40> m_lfo;

	std::array<mixer_slot, 0x80> m_mixer;

	std::array<s32,  0x10> m_melo;
	std::array<s32,  0x10> m_meli;
	std::array<s32,     4> m_adc;

	// S-MU2000: DRC は使わない。meg_state はそのまま持つ
	std::unique_ptr<meg_state> m_meg_storage;
	meg_state *m_meg;
	bool m_meg_program_changed;

	u32 m_sample_counter;
	u32 m_wave_adr, m_wave_size, m_wave_val, m_revram_adr, m_revram_data;
	u16 m_wave_access, m_revram_enable;

	u64 m_keyon_mask;
	u16 m_internal_adr;

	// Streaming block trampolines
	u16 start_h_r(offs_t offset);
	u16 start_l_r(offs_t offset);
	void start_h_w(offs_t offset, u16 data);
	void start_l_w(offs_t offset, u16 data);
	u16 loop_h_r(offs_t offset);
	u16 loop_l_r(offs_t offset);
	void loop_h_w(offs_t offset, u16 data);
	void loop_l_w(offs_t offset, u16 data);
	u16 address_h_r(offs_t offset);
	u16 address_l_r(offs_t offset);
	void address_h_w(offs_t offset, u16 data);
	void address_l_w(offs_t offset, u16 data);
	u16 pitch_r(offs_t offset);
	void pitch_w(offs_t offset, u16 data);


	// Filter block trampolines
	u16 filter_1_a_r(offs_t offset);
	u16 level_1_r(offs_t offset);
	u16 filter_2_a_r(offs_t offset);
	u16 level_2_r(offs_t offset);
	u16 filter_b_r(offs_t offset);

	void filter_1_a_w(offs_t offset, u16 data);
	void level_1_w(offs_t offset, u16 data);
	void filter_2_a_w(offs_t offset, u16 data);
	void level_2_w(offs_t offset, u16 data);
	void filter_b_w(offs_t offset, u16 data);


	// IIR1 block trampolines
	template<u32 Filter> u16 a0_r(offs_t offset);
	template<u32 Filter> u16 a1_r(offs_t offset);
	template<u32 Filter> u16 b1_r(offs_t offset);
	template<u32 Filter> void a0_w(offs_t offset, u16 data);
	template<u32 Filter> void a1_w(offs_t offset, u16 data);
	template<u32 Filter> void b1_w(offs_t offset, u16 data);

	// Envelope block trampolines
	u16 attack_r(offs_t offset);
	void attack_w(offs_t offset, u16 data);
	u16 decay1_r(offs_t offset);
	void decay1_w(offs_t offset, u16 data);
	u16 decay2_r(offs_t offset);
	void decay2_w(offs_t offset, u16 data);
	u16 release_glo_r(offs_t offset);
	void release_glo_w(offs_t offset, u16 data);

	void lfo_amplitude_w(offs_t offset, u16 data);
	u16 lfo_amplitude_r(offs_t offset);
	void lfo_type_step_pitch_w(offs_t offset, u16 data);
	u16 lfo_type_step_pitch_r(offs_t offset);

	u16 internal_adr_r();
	void internal_adr_w(u16 data);
	u16 internal_r();
	template<int Sel> u16 route_r(offs_t offset);
	template<int Sel> void route_w(offs_t offset, u16 data);
	template<int Sel> u16 vol_r(offs_t offset);
	template<int Sel> void vol_w(offs_t offset, u16 data);

	static s32 volume_apply(s32 level, s32 sample);

	static s32 mixer_att(s32 sample, s32 att);

	// Control registers
	template<int Sel> u16 keyon_mask_r();
	template<int Sel> void keyon_mask_w(u16 data);
	u16 keyon_r();
	void keyon_w(u16);
	u16 meg_prg_address_r();
	void meg_prg_address_w(u16 data);
	void meg_lfo_commit_w(u16);
	template<int Sel> u16 meg_prg_r();
	template<int Sel> void meg_prg_w(u16 data);
	template<int Sel> u16 meg_map_r();
	template<int Sel> void meg_map_w(u16 data);
	template<int Sel> void wave_adr_w(u16 data);
	template<int Sel> u16 wave_adr_r();
	template<int Sel> void wave_size_w(u16 data);
	template<int Sel> u16 wave_size_r();
	void wave_access_w(u16 data);
	u16 wave_access_r();
	u16 wave_busy_r();
	template<int Sel> u16 wave_val_r();
	template<int Sel> void wave_val_w(u16 data);
	void revram_enable_w(u16 data);
	void revram_clear_w(u16 data);
	u16 revram_status_r();
	template<int Sel> void revram_adr_w(u16 data);
	template<int Sel> void revram_data_w(u16 data);
	template<int Sel> u16 revram_data_r();

	// MEG registers
	template<int Sel> u16 meg_const_r(offs_t offset);
	template<int Sel> void meg_const_w(offs_t offset, u16 data);
	template<int Sel> u16 meg_offset_r(offs_t offset);
	template<int Sel> void meg_offset_w(offs_t offset, u16 data);
	template<int Sel> u16 meg_lfo_r(offs_t offset);
	template<int Sel> void meg_lfo_w(offs_t offset, u16 data);

	u64 meg_prg_map_r(offs_t address);



	// Generic catch-all
	u16 snd_r(offs_t offset);
	void snd_w(offs_t offset, u16 data);


	void awm2_step(std::array<s32, 0x40> &samples_per_chan);
	void mixer_step(const std::array<s32, 0x40> &samples_per_chan);
	void adc_step();
	void sample_step();
};

#endif // MAME_SOUND_SWP30_H
