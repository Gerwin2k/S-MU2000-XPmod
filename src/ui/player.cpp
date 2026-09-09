// license:BSD-3-Clause

#include "player.h"

#include <windows.h>

namespace ui {

namespace {

// 鳴りっぱなしを消す。16 チャンネルぶん
void all_off(bridge &br)
{
	for (int ch = 0; ch < 16; ch++) {
		const u8 msg[3] = { u8(0xb0 | ch), 0x7b, 0x00 };   // オールノートオフ
		br.send(msg, 3);
		const u8 sus[3] = { u8(0xb0 | ch), 0x40, 0x00 };   // ダンパも離す
		br.send(sus, 3);
	}
}

} // namespace


bool player::start(const std::string &path, bridge &br, std::string &err)
{
	stop();

	std::vector<smf::event> evs;
	if (!smf::load(path, evs, err))
		return false;
	if (evs.empty()) {
		err = "中身が空";
		return false;
	}

	m_events = std::move(evs);
	m_len = m_events.back().time;
	const size_t slash = path.find_last_of("/\\");
	m_name = (slash == std::string::npos) ? path : path.substr(slash + 1);

	m_quit.store(false);
	m_pos.store(0);
	m_playing.store(true, std::memory_order_release);
	m_thread = std::thread([this, &br] { run(br); });
	return true;
}

void player::stop()
{
	m_quit.store(true, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();
	m_playing.store(false, std::memory_order_release);
	m_pos.store(0);
}

void player::run(bridge &br)
{
	// 1 ミリ秒で起きられるようにしておく。既定の 15.6 ミリ秒だと
	// 音符の頭がばらつく
	timeBeginPeriod(1);

	LARGE_INTEGER f, t0;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);

	size_t at = 0;
	while (!m_quit.load(std::memory_order_acquire) && at < m_events.size()) {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const double sec = double(now.QuadPart - t0.QuadPart) / double(f.QuadPart);
		m_pos.store(sec, std::memory_order_relaxed);

		// 来ている分をまとめて送る
		while (at < m_events.size() && m_events[at].time <= sec) {
			br.send(m_events[at].bytes.data(), m_events[at].bytes.size());
			at++;
		}
		if (at >= m_events.size())
			break;

		// 次まで待つ。長く待ちすぎないように刻む
		const double wait = m_events[at].time - sec;
		Sleep(wait > 0.010 ? 5 : 1);
	}

	all_off(br);
	timeEndPeriod(1);
	m_playing.store(false, std::memory_order_release);
}

} // namespace ui
