// license:BSD-3-Clause
//
// XG のエフェクトの種類（MSB/LSB）と、その値を、C++ のエフェクト（dsp::blocks.h）へ割り当てる。
// 軽量モードの入り口。doc/native-dsp.md を見よ。
//
// **実機（MEG）の再現ではない。** 種類ごとの系統（残響・ディレイ・揺れ・歪み・EQ・ダイナミクス・
// ローファイ）に合わせて、似た掛かり方の C++ の作りを当てているだけで、同じ音にはならない。
// 正しさが要るときは今までどおり MEG を回す（既定はそちら）。
//
// 値は XG の生の数（0-127 など）で渡す。意味（秒・ミリ秒・Hz・dB）は xg/fx_params.h の表から引く。

#ifndef S_MU2000_DSP_FX_NATIVE_H
#define S_MU2000_DSP_FX_NATIVE_H

#pragma once

#include "blocks.h"
#include "reverb.h"
#include "xg/fx_params.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace smu2000::dsp {

// 生の値を、表を見て物理量に直す（"2.5" → 2.5、"0.1" → 0.1、数でない表示は def）
inline float fx_value(const xg::fx_param &p, int raw, float def = 0.0f)
{
	const int v = raw < int(p.lo) ? int(p.lo) : (raw > int(p.hi) ? int(p.hi) : raw);
	switch (p.fmt) {
	case xg::fx_fmt::tenths:
		return float(v) / 10.0f;
	case xg::fx_fmt::table: {
		if (!p.texts || v < int(p.lo))
			return float(v);
		const char *t = p.texts[v - int(p.lo)];
		if (!t || !*t)
			return def;
		char *end = nullptr;
		float f = std::strtof(t, &end);
		if (end == t)
			return def;                 // "off" や "D63>W" のような表示
		if (*end == 'k' || *end == 'K')
			f *= 1000.0f;               // "6.3k" は 6300
		return f;
	}
	default:
		return float(v);
	}
}

// 1 つのエフェクト（リバーブ・コーラス・バリエーション・インサーション 1-4）
class fx_slot
{
public:
	enum class kind { none, thru, reverb, early, delay, mod, rotary, drive, eq, wah, dyn, lofi };

	void set_rate(float rate)
	{
		m_rate = rate;
		m_rev.set_rate(rate);
		m_er.set_rate(rate);
		m_dly.set_rate(rate);
		m_mod.set_rate(rate);
		m_rot.set_rate(rate);
		m_drv.set_rate(rate);
		m_eq.set_rate(rate);
		m_wah.set_rate(rate);
		m_dyn.set_rate(rate);
		m_lofi.set_rate(rate);
	}

	kind current() const { return m_kind; }
	int  type() const { return m_type; }

	// 種類と、その種類のパラメータ（生の値。並びは xg/fx_params.h の順）
	void set(int type, const int *raw, int count)
	{
		const bool new_type = type != m_type;
		m_type = type;
		m_kind = kind_of(type >> 7);
		apply(raw, count);
		if (new_type)
			reset();
	}

	void reset()
	{
		m_rev.reset();
		m_er.reset();
		m_dly.reset();
		m_mod.reset();
		m_rot.reset();
		m_drv.reset();
		m_eq.reset();
		m_wah.reset();
		m_dyn.reset();
		m_lofi.reset();
	}

	void process(float l, float r, float &ol, float &orr)
	{
		// 入り口が黙ったままなら、中身が落ち着いたところで回すのをやめる（軽くするため）。
		// 尾（リバーブなど）が消えるまでは回し続ける
		if (l == 0.0f && r == 0.0f) {
			if (m_quiet > QUIET_LIMIT) {
				ol = orr = 0.0f;
				return;
			}
			m_quiet++;
		} else {
			m_quiet = 0;
		}

		switch (m_kind) {
		case kind::reverb: m_rev.process(l, r, ol, orr); break;
		case kind::early:  m_er.process(l, r, ol, orr); break;
		case kind::delay:  m_dly.process(l, r, ol, orr); break;
		case kind::mod:    m_mod.process(l, r, ol, orr); break;
		case kind::rotary: m_rot.process(l, r, ol, orr); break;
		case kind::drive:  m_drv.process(l, r, ol, orr); break;
		case kind::eq:     m_eq.process(l, r, ol, orr); break;
		case kind::wah:    m_wah.process(l, r, ol, orr); break;
		case kind::dyn:    m_dyn.process(l, r, ol, orr); break;
		case kind::lofi:   m_lofi.process(l, r, ol, orr); break;
		case kind::thru:   ol = l; orr = r; break;
		default:           ol = orr = 0.0f; break;
		}
	}

	// 種類の系統。xg::fx_categories() の分け方に合わせてある
	static kind kind_of(int msb)
	{
		if (msb == 0x00) return kind::none;
		if (msb == 0x40) return kind::thru;
		if ((msb >= 0x01 && msb <= 0x04) || (msb >= 0x10 && msb <= 0x14)) return kind::reverb;
		if (msb >= 0x09 && msb <= 0x0b) return kind::early;
		if ((msb >= 0x05 && msb <= 0x08) || msb == 0x15 || msb == 0x16) return kind::delay;
		if (msb == 0x41 || msb == 0x42 || msb == 0x57 || msb == 0x43 || msb == 0x44 ||
		    msb == 0x48 || msb == 0x68 || msb == 0x6b || msb == 0x6c || msb == 0x6e ||
		    msb == 0x6f || msb == 0x50 || msb == 0x51 || msb == 0x55 || msb == 0x58 ||
		    msb == 0x5d || msb == 0x70 || msb == 0x71) return kind::mod;
		if (msb == 0x45 || msb == 0x46 || msb == 0x47 || msb == 0x56 || msb == 0x63) return kind::rotary;
		if (msb == 0x49 || msb == 0x4a || msb == 0x4b || msb == 0x62 ||
		    msb == 0x5f || msb == 0x60 || msb == 0x61) return kind::drive;
		if (msb == 0x4c || msb == 0x4d || msb == 0x73) return kind::eq;
		if (msb == 0x4e || msb == 0x52 || msb == 0x6d || msb == 0x74) return kind::wah;
		if (msb == 0x53 || msb == 0x54 || msb == 0x69) return kind::dyn;
		if (msb == 0x5e || msb == 0x72 || msb == 0x75 || msb == 0x76) return kind::lofi;
		return kind::mod;
	}

private:
	// 名前でパラメータを引く（無ければ def）
	float par(const char *label, float def) const
	{
		if (!m_def)
			return def;
		for (int i = 0; i < m_def->count && i < MAX_PAR; i++)
			if (!std::strcmp(m_def->params[i].label, label))
				return fx_value(m_def->params[i], m_raw[i], def);
		return def;
	}

	// 生の値のまま（0-127 など）
	float raw_of(const char *label, int def) const
	{
		if (m_def)
			for (int i = 0; i < m_def->count && i < MAX_PAR; i++)
				if (!std::strcmp(m_def->params[i].label, label))
					return float(m_raw[i]);
		return float(def);
	}

	// EQ のゲイン（XG は 52-76 で ±12dB、64 が 0）
	float gain_db(const char *label) const
	{
		return clampf(raw_of(label, 64) - 64.0f, -12.0f, 12.0f);
	}

	// Dry/Wet（1-127。64 で半々、127 が全部 wet）
	float wet_of(float def) const
	{
		if (!has("Dry/Wet"))
			return def;
		return clampf(raw_of("Dry/Wet", 127) / 127.0f, 0.0f, 1.0f);
	}

	bool has(const char *label) const
	{
		if (!m_def)
			return false;
		for (int i = 0; i < m_def->count; i++)
			if (!std::strcmp(m_def->params[i].label, label))
				return true;
		return false;
	}

	void apply(const int *raw, int count)
	{
		m_def = xg::fx_find(m_type);
		for (int i = 0; i < MAX_PAR; i++)
			m_raw[i] = i < count ? raw[i] : 0;

		switch (m_kind) {
		case kind::reverb: {
			reverb::params p;
			p.time        = par("ReverbTime", 2.0f);
			p.predelay_ms = par("InitDelay", 20.0f);
			p.hpf_hz      = par("HPF Cutoff", 80.0f);
			// High Damp（1-10）は高い音の減り方。表の LPF Cutoff（Hz）と合わせて使う
			p.damp_hz     = par("LPF Cutoff", 8000.0f) * clampf(par("High Damp", 5.0f) / 5.0f, 0.3f, 2.0f);
			p.diffusion   = clampf(par("Diffusion", 7.0f) / 10.0f, 0.0f, 1.0f);
			p.er_level    = clampf(par("Er/Rev", 5.0f) / 10.0f, 0.0f, 1.0f);
			p.width       = 1.0f;
			m_rev.set_params(p);
			break;
		}
		case kind::early: {
			early_ref::params p;
			p.room     = clampf(par("Room Size", 5.0f) / 10.0f, 0.1f, 1.0f);
			p.liveness = clampf(par("Liveness", 5.0f) / 10.0f, 0.0f, 1.0f);
			p.time_ms  = par("Gate Time", par("InitDelay", 200.0f));
			p.diffuse  = clampf(par("Diffusion", 7.0f) / 10.0f, 0.0f, 1.0f);
			p.gate     = (m_type >> 7) == 0x0a;
			p.reverse  = (m_type >> 7) == 0x0b;
			m_er.set_params(p);
			break;
		}
		case kind::delay: {
			delay_fx::params p;
			p.l_ms = par("LchDelay", par("Lch Delay", par("DelayTime", 300.0f)));
			p.r_ms = par("RchDelay", par("Rch Delay", p.l_ms * 1.3f));
			p.c_ms = par("CchDelay", (p.l_ms + p.r_ms) * 0.5f);
			p.fb_ms = par("FB Delay", par("FBDelay1", p.l_ms));
			// FB Level は 1-127 で 64 が 0。実機の尾に合わせて少し強めにする
			p.feedback = clampf((raw_of("FB Level", 64) / 64.0f - 1.0f) * 1.2f, -0.95f, 0.95f);
			p.c_level = clampf(raw_of("Cch Level", 100) / 127.0f, 0.0f, 1.0f);
			p.hpf_hz = par("HPF Cutoff", 60.0f);
			p.lpf_hz = par("LPF Cutoff", 8000.0f) * clampf(par("High Damp", 5.0f) / 5.0f, 0.3f, 2.0f);
			p.cross = (m_type >> 7) == 0x07;
			m_dly.set_params(p);
			break;
		}
		case kind::mod: {
			mod_fx::params p;
			const int msb = m_type >> 7;
			p.rate_hz = par("LFO Freq", 0.6f);
			p.depth = clampf(raw_of("LFO Depth", 40) / 127.0f, 0.0f, 1.0f);
			p.delay_ms = par("DelayOfst", par("ModDlyOfst", 10.0f));
			p.feedback = clampf(raw_of("FB Level", 64) / 64.0f - 1.0f, -0.95f, 0.95f);
			p.phase_deg = par("LFO Phase", par("PhaseShift", 90.0f));
			p.stages = par("Stage", 6.0f);
			if (msb == 0x43 || msb == 0x44 || msb == 0x68 || msb == 0x6b || msb == 0x6e)
				p.type = mod_fx::kind::flanger;
			else if (msb == 0x48 || msb == 0x6c || msb == 0x6f)
				p.type = mod_fx::kind::phaser;
			else if (msb == 0x42)
				p.type = mod_fx::kind::celeste;
			else if (msb == 0x57 || msb == 0x62)
				p.type = mod_fx::kind::ensemble;
			else
				p.type = mod_fx::kind::chorus;
			m_mod.set_params(p);
			break;
		}
		case kind::rotary: {
			rotary_fx::params p;
			const int msb = m_type >> 7;
			p.speed_hz = par("LFO Freq", par("RotorSpd", 5.0f));
			p.depth = clampf(raw_of("LFO Depth", raw_of("Mic Angle", 60)) / 127.0f, 0.0f, 1.0f);
			p.drive = clampf(raw_of("Drive", 0) / 127.0f, 0.0f, 1.0f);
			p.type = msb == 0x46 ? rotary_fx::kind::tremolo
			       : msb == 0x47 ? rotary_fx::kind::auto_pan
			                     : rotary_fx::kind::rotary;
			m_rot.set_params(p);
			break;
		}
		case kind::drive: {
			drive_fx::params p;
			p.drive = clampf(raw_of("Drive", raw_of("Dist Drive", 60)) / 127.0f, 0.0f, 1.0f);
			p.edge = clampf(raw_of("Edge", 64) / 127.0f, 0.0f, 1.0f);
			// 実機の歪みは思ったより小さい。同じ曲で rms を合わせた
			p.out_level = clampf(raw_of("OutputLvl", raw_of("DistOutLvl", 64)) / 127.0f, 0.0f, 1.0f);
			p.lpf_hz = par("LPF Cutoff", 4000.0f);
			p.eq_low_db = gain_db("EQ LowGain");
			p.eq_low_hz = par("EQ LowFreq", 200.0f);
			p.eq_mid_db = gain_db("EQ MidGain");
			p.eq_mid_hz = par("EQ MidFreq", 1400.0f);
			p.eq_mid_q = std::max(0.1f, par("EQ MidWidt", 10.0f) / 10.0f);
			p.dry_wet = wet_of(1.0f);
			m_drv.set_params(p);
			break;
		}
		case kind::eq: {
			eq_fx::params p;
			p.low_hz = par("EQ LowFreq", par("Low Freq", 200.0f));
			p.low_db = has("EQ LowGain") ? gain_db("EQ LowGain") : gain_db("Low Gain");
			p.mid_hz = par("EQ MidFreq", par("Mid Freq", 1000.0f));
			p.mid_db = has("EQ MidGain") ? gain_db("EQ MidGain") : gain_db("Mid Gain");
			p.mid_q = std::max(0.1f, par("EQ MidWidt", par("Mid Width", 10.0f)) / 10.0f);
			p.high_hz = par("EQHighFreq", par("High Freq", 6000.0f));
			p.high_db = has("EQHighGain") ? gain_db("EQHighGain") : gain_db("High Gain");
			p.three_band = has("EQ MidGain") || has("Mid Gain");
			m_eq.set_params(p);
			break;
		}
		case kind::wah: {
			wah_fx::params p;
			p.by_envelope = has("Sensitivty");
			p.rate_hz = par("LFO Freq", 1.0f);
			p.depth = clampf(raw_of("LFO Depth", 64) / 127.0f, 0.0f, 1.0f);
			p.sens = clampf(raw_of("Sensitivty", 64) / 127.0f, 0.0f, 1.0f);
			p.low_hz = par("CutoffFreq", 300.0f);
			p.high_hz = std::max(p.low_hz * 2.0f, p.low_hz * 8.0f);
			p.resonance = std::max(0.5f, par("Resonance", 30.0f) / 10.0f);
			p.dry_wet = wet_of(1.0f);
			m_wah.set_params(p);
			break;
		}
		case kind::dyn: {
			dyn_fx::params p;
			p.gate = (m_type >> 7) == 0x54;
			p.threshold_db = par("Threshold", par("ThreshLevel", -20.0f));
			p.ratio = std::max(1.0f, par("Ratio", 4.0f));
			p.attack_ms = par("Attack", par("AttackTime", 5.0f));
			p.release_ms = par("Release", par("RelesTime", 100.0f));
			p.out_level = clampf(raw_of("OutputLvl", 64) / 64.0f, 0.0f, 4.0f);
			m_dyn.set_params(p);
			break;
		}
		case kind::lofi: {
			lofi_fx::params p;
			p.bits = clampf(par("WordLength", par("Bit Assign", 8.0f)), 1.0f, 16.0f);
			p.rate_div = std::max(1.0f, par("SmplFreq", 4.0f));
			p.noise = clampf(raw_of("NoiseLevel", 0) / 127.0f, 0.0f, 0.2f);
			p.lpf_hz = par("LPF Cutoff", 8000.0f);
			m_lofi.set_params(p);
			break;
		}
		default:
			break;
		}
	}

	static constexpr int MAX_PAR = 16;

	// 入り口が黙ってから回し続けるサンプル数（44100Hz で 4 秒ぶん。いちばん長い尾より長く）
	static constexpr int QUIET_LIMIT = 44100 * 4;
	int m_quiet = 0;

	kind m_kind = kind::none;
	int  m_type = 0;
	int  m_raw[MAX_PAR] = {};
	const xg::fx_def *m_def = nullptr;
	float m_rate = 44100.0f;

	reverb    m_rev;
	early_ref m_er;
	delay_fx  m_dly;
	mod_fx    m_mod;
	rotary_fx m_rot;
	drive_fx  m_drv;
	eq_fx     m_eq;
	wah_fx    m_wah;
	dyn_fx    m_dyn;
	lofi_fx   m_lofi;
};

// 7 つの口（リバーブ・コーラス・バリエーション・インサーション 1-4）をまとめたもの
class native_fx
{
public:
	enum slot_id { REVERB = 0, CHORUS = 1, VARIATION = 2, INS1 = 3, INS2 = 4, INS3 = 5, INS4 = 6, SLOTS = 7 };

	void set_rate(float rate)
	{
		for (auto &s : m_slot)
			s.set_rate(rate);
		m_meq.set_rate(rate);
	}

	master_eq &meq() { return m_meq; }

	void set(slot_id id, int type, const int *raw, int count) { m_slot[id].set(type, raw, count); }
	void reset() { for (auto &s : m_slot) s.reset(); }

	fx_slot &slot(slot_id id) { return m_slot[id]; }

	void process(slot_id id, float l, float r, float &ol, float &orr)
	{
		// インサーションは音がそこを通るので、種類が無い・分からないときは素通し。
		// 送り（リバーブなど）は、種類が無ければ何も出さない
		if (id >= INS1 && m_slot[id].current() == fx_slot::kind::none) {
			ol = l;
			orr = r;
			return;
		}
		m_slot[id].process(l, r, ol, orr);
	}

	// 送りに対する戻りの量（リバーブ・コーラス・バリエーション）
	void set_return(slot_id id, float gain) { m_return[id] = gain; }
	float ret(slot_id id) const { return m_return[id]; }

private:
	fx_slot   m_slot[SLOTS];
	master_eq m_meq;
	float   m_return[SLOTS] = { 0.6f, 0.6f, 0.6f, 1.0f, 1.0f, 1.0f, 1.0f };
};

} // namespace smu2000::dsp

#endif // S_MU2000_DSP_FX_NATIVE_H
