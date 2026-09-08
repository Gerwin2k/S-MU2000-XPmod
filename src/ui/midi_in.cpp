// license:BSD-3-Clause

#include "midi_in.h"

#include <windows.h>
#include <mmsystem.h>

namespace ui {

namespace {

void CALLBACK cb(HMIDIIN, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR)
{
	midi_in *self = reinterpret_cast<midi_in *>(user);
	if (!self)
		return;

	if (msg == MIM_DATA) {
		const u8 status = u8(p1);
		if (status < 0x80)
			return;
		// 長さは種別で決まる。プログラムチェンジとチャンネルプレッシャだけ 1 バイト
		int n = 3;
		const u8 kind = status & 0xf0;
		if (kind == 0xc0 || kind == 0xd0) n = 2;
		if (status >= 0xf8) n = 1;                       // リアルタイム
		else if (status == 0xf1 || status == 0xf3) n = 2;
		else if (status == 0xf2) n = 3;
		else if (status >= 0xf4 && status < 0xf8) n = 1;

		self->push(status);
		if (n > 1) self->push(u8(p1 >> 8));
		if (n > 2) self->push(u8(p1 >> 16));
	} else if (msg == MIM_LONGDATA) {
		MIDIHDR *h = reinterpret_cast<MIDIHDR *>(p1);
		for (DWORD i = 0; i < h->dwBytesRecorded; i++)
			self->push(u8(h->lpData[i]));
		// 使い終わった入れ物をすぐ返す。返さないと次の SysEx が受けられない。
		// 閉じている最中は返さない（midiInReset が全部戻してくるので、
		// そこで返すと終わらなくなる）
		self->requeue(h);
	}
}

} // namespace


std::vector<std::string> midi_in::list()
{
	std::vector<std::string> out;
	const UINT n = midiInGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		MIDIINCAPSA caps{};
		if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			out.push_back(caps.szPname);
		else
			out.push_back("?");
	}
	return out;
}

bool midi_in::open(int device, std::string &err)
{
	close();
	if (device < 0)
		return true;
	if (UINT(device) >= midiInGetNumDevs()) {
		err = "その番号の MIDI 入力は無い";
		return false;
	}

	HMIDIIN h = nullptr;
	if (midiInOpen(&h, UINT(device), DWORD_PTR(cb), DWORD_PTR(this),
	               CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
		err = "MIDI 入力を開けない";
		return false;
	}
	MIDIINCAPSA caps{};
	midiInGetDevCapsA(UINT(device), &caps, sizeof(caps));
	m_name = caps.szPname;
	m_handle = h;
	m_closing.store(false, std::memory_order_release);

	// SysEx の入れ物を Windows へ渡す。**これをやらないと SysEx は来ない**。
	// 1 枚に収まらない長いものは何枚かに分かれて、順に届く
	for (int i = 0; i < SYSEX_BUFFERS; i++) {
		m_sysex[i] = new u8[SYSEX_SIZE];
		MIDIHDR *hdr = new MIDIHDR{};
		hdr->lpData = reinterpret_cast<LPSTR>(m_sysex[i]);
		hdr->dwBufferLength = DWORD(SYSEX_SIZE);
		if (midiInPrepareHeader(h, hdr, sizeof(MIDIHDR)) != MMSYSERR_NOERROR ||
		    midiInAddBuffer(h, hdr, sizeof(MIDIHDR)) != MMSYSERR_NOERROR) {
			delete hdr;
			delete[] m_sysex[i];
			m_sysex[i] = nullptr;
			continue;                 // 音符は受けられるので、これだけで諦めはしない
		}
		m_hdr[i] = hdr;
	}

	midiInStart(h);
	return true;
}

// コールバックから。使い終わった入れ物を返して、次の SysEx を待たせる
void midi_in::requeue(void *hdr)
{
	if (!m_handle || closing())
		return;
	midiInAddBuffer(reinterpret_cast<HMIDIIN>(m_handle),
	                reinterpret_cast<MIDIHDR *>(hdr), sizeof(MIDIHDR));
}

void midi_in::close()
{
	if (!m_handle)
		return;
	HMIDIIN h = reinterpret_cast<HMIDIIN>(m_handle);
	// 先に印を立てる。midiInReset は入れ物を全部コールバックへ戻すので、
	// そこで返し直すと閉じられなくなる
	m_closing.store(true, std::memory_order_release);
	midiInStop(h);
	midiInReset(h);
	for (int i = 0; i < SYSEX_BUFFERS; i++) {
		if (m_hdr[i]) {
			MIDIHDR *hdr = reinterpret_cast<MIDIHDR *>(m_hdr[i]);
			midiInUnprepareHeader(h, hdr, sizeof(MIDIHDR));
			delete hdr;
			m_hdr[i] = nullptr;
		}
		delete[] m_sysex[i];
		m_sysex[i] = nullptr;
	}
	midiInClose(h);
	m_handle = nullptr;
	m_name.clear();
}

void midi_in::push(u8 v)
{
	const size_t w = m_write.load(std::memory_order_relaxed);
	const size_t next = (w + 1) & MASK;
	if (next == m_read.load(std::memory_order_acquire))
		return;                       // 溢れ。実機の受信バッファ溢れと同じ
	m_buf[w] = v;
	m_write.store(next, std::memory_order_release);
	m_bytes.fetch_add(1, std::memory_order_relaxed);
}

bool midi_in::pop(u8 &v)
{
	const size_t r = m_read.load(std::memory_order_relaxed);
	if (r == m_write.load(std::memory_order_acquire))
		return false;
	v = m_buf[r];
	m_read.store((r + 1) & MASK, std::memory_order_release);
	return true;
}

} // namespace ui
