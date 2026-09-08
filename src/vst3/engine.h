// license:BSD-3-Clause
//
// VST3 プラグインの中身。MU2000 の面倒を全部ここで見る。
//
//   ・ROM の置き場を探す
//   ・起動（4 秒ぶんの空回し）を別スレッドで済ませる
//   ・ホストの標本化周波数へ変換する（MU2000 は 44100 固定）
//
// plugin.cpp からはこれだけを触る。VST3 の型は一切出てこない。

#ifndef S_MU2000_VST3_ENGINE_H
#define S_MU2000_VST3_ENGINE_H

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

class mu2000;

namespace smu2000 {
namespace vst3 {

// MU2000 が動く唯一の周波数
constexpr double NATIVE_RATE = 44100.0;

enum class status {
	loading,   // ROM を読んで起動している最中。音は出ない
	ready,
	failed,    // ROM が見つからないなど。message() に理由が入る
};

class engine
{
public:
	engine();
	~engine();

	// ROM を探して読み、起動するまでを別スレッドで進める。すぐ返る
	void start();

	status state() const { return m_state.load(std::memory_order_acquire); }
	// state() が failed のときの理由。ready でも「代用品を使った」等が入る
	std::string message() const;

	// ホスト側の標本化周波数。44100 ちょうどなら変換を通さない
	void set_output_rate(double rate);
	// 変換のぶんだけ音が遅れる。ホストに申告する
	uint32_t latency_samples() const { return m_latency; }

	// MIDI を 1 メッセージ流す。実機と同じく 31250bps の直列に崩される。
	// 起動が終わっていない間は溜めておいて、終わってから流す
	void midi(const uint8_t *bytes, size_t n);
	// 全チャンネルのオールノートオフ + リセットオールコントローラ
	void all_notes_off();

	// n サンプルぶん作る。左右は別々の配列（VST3 はそういう渡し方をする）
	void fill(float *left, float *right, int n);

	// 記録（%LOCALAPPDATA%\S-MU2000\log.txt）へ 1 行書く。画面が無いのでここが窓口
	void log_line(const char *text);

	// 再生位置が飛んだ、止まった等。変換器の中身だけ捨てる
	void flush_resampler();

private:
	void boot();
	void one_sample(float &l, float &r);
	void build_table();

	std::atomic<status> m_state{status::loading};
	std::thread         m_thread;
	std::atomic<bool>   m_abort{false};

	mu2000     *m_mu = nullptr;
	// boot() が state を立てる前に書き、読むのは state が loading でなくなってから
	std::string m_message;

	// ---- 標本化周波数の変換。窓関数付き sinc の畳み込み
	//
	// 44100 で作った音を任意の周波数へ。ホストが 44100 なら丸ごと省く。
	static constexpr int TAPS = 64;
	static constexpr int HALF = TAPS / 2;
	static constexpr int STEPS = 256;              // 1 サンプル間隔あたりの表の刻み
	static constexpr int RING = 256, RMASK = RING - 1;

	std::vector<float> m_tab;      // 窓関数付き sinc。[0, HALF] を STEPS 刻みで
	float   m_ring_l[RING] = {};
	float   m_ring_r[RING] = {};
	int64_t m_written = 0;         // これまでに作った 44100 側のサンプル数
	double  m_pos = 0.0;           // 次に出す音の、44100 側での位置
	double  m_step = 1.0;          // 出力 1 サンプルあたり 44100 側で進む量
	double  m_cutoff = 1.0;
	bool    m_direct = true;       // 変換なし
	uint32_t m_latency = 0;

	// 起動前に来た MIDI。音声スレッドしか触らない
	std::vector<uint8_t> m_pending;
};

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_ENGINE_H
