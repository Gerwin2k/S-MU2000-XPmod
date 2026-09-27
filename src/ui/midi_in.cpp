// license:BSD-3-Clause

// Modified by GB 2026 for Windows XP Compatibility of live.exe 
// AI disclosure" assisted by GPT-5.6 Luna (ChatGPT.com).

#include "midi_in.h"
#include "mm_open.h"
#include "text.h"

#include <cstdio>

#include <windows.h>
#include <mmsystem.h>

namespace ui {

namespace {

void CALLBACK cb(
	HMIDIIN,
	UINT msg,
	DWORD_PTR user,
	DWORD_PTR p1,
	DWORD_PTR)
{
	midi_in *self =
		reinterpret_cast<midi_in *>(user);

	if (!self)
		return;

	if (msg == MIM_DATA) {
		self->on_short(u32(p1));
	}
	else if (msg == MIM_LONGDATA) {
		MIDIHDR *h =
			reinterpret_cast<MIDIHDR *>(p1);

		if (h) {
			self->on_long(
				reinterpret_cast<const u8 *>(h->lpData),
				h->dwBytesRecorded);

			self->requeue(h);
		}
	}
}

/*
void CALLBACK cb(
    HMIDIIN,
    UINT msg,
    DWORD_PTR user,
    DWORD_PTR p1,
    DWORD_PTR p2)
{
    midi_in *self =
        reinterpret_cast<midi_in *>(user);

//    std::printf(
//        "MIDI CALLBACK: msg=%u p1=%08lX p2=%08lX\n",
//        (unsigned)msg,
//        (unsigned long)p1,
//        (unsigned long)p2);

//    std::fflush(stdout);

    if (!self)
        return;

    if (msg == MIM_DATA) {
        const u32 v = u32(p1);

//        std::printf(
//            "  MIM_DATA: %08lX  bytes=%02X %02X %02X\n",
//            (unsigned long)v,
//            (unsigned)(v & 0xff),
//            (unsigned)((v >> 8) & 0xff),
//            (unsigned)((v >> 16) & 0xff));

//        std::fflush(stdout);

        self->on_short(v);

    } else if (msg == MIM_LONGDATA) {
        MIDIHDR *h =
            reinterpret_cast<MIDIHDR *>(p1);

        std::printf(
            "  MIM_LONGDATA: recorded=%lu flags=%08lX\n",
            (unsigned long)(h ? h->dwBytesRecorded : 0),
            (unsigned long)(h ? h->dwFlags : 0));

        if (h && h->lpData) {
            std::printf("  LONG:");

            for (DWORD i = 0;
                 i < h->dwBytesRecorded;
                 ++i) {
                std::printf(
                    " %02X",
                    (unsigned char)h->lpData[i]);
            }

            std::printf("\n");
        }

        std::fflush(stdout);

        if (h) {
            self->on_long(
                reinterpret_cast<const u8 *>(h->lpData),
                h->dwBytesRecorded);

            self->requeue(h);
        }
    } else {
        std::printf(
            "  UNKNOWN MIDI MESSAGE: %u\n",
            (unsigned)msg);

        std::fflush(stdout);
    }
}
*/
/*
void CALLBACK cb(HMIDIIN, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR)
{
	midi_in *self = reinterpret_cast<midi_in *>(user);
	if (!self)
		return;

//	if (msg == MIM_DATA) {
//		self->on_short(u32(p1));
//	}
// GB 2026	
if (msg == MIM_DATA) {
    const u32 v = u32(p1);

    std::printf(
        "MIM_DATA: %08lX  bytes=%02X %02X %02X\n",
        (unsigned long)v,
        (unsigned)(v & 0xff),
        (unsigned)((v >> 8) & 0xff),
        (unsigned)((v >> 16) & 0xff));

    std::fflush(stdout);	
}
	
	
	else if (msg == MIM_LONGDATA) {
		MIDIHDR *h = reinterpret_cast<MIDIHDR *>(p1);
		self->on_long(reinterpret_cast<const u8 *>(h->lpData), h->dwBytesRecorded);
		// 使い終わった入れ物をすぐ返す。返さないと次の SysEx が受けられない。
		// 閉じている最中は返さない（midiInReset が全部戻してくるので、
		// そこで返すと終わらなくなる）
		self->requeue(h);
	}
}

*/

} // namespace


std::vector<std::string> midi_in::list()
{
	std::vector<std::string> out;
	const UINT n = midiInGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		MIDIINCAPSW caps{};
		if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			out.push_back(to_utf8(caps.szPname));
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

	const MMRESULT r = midiInOpen(
		&h,
		UINT(device),
		DWORD_PTR(cb),
		DWORD_PTR(this),
		CALLBACK_FUNCTION);

	if (r != MMSYSERR_NOERROR) {
		char text[256] = {};

		midiInGetErrorTextA(r, text, sizeof(text));

		err = std::string("MIDI input could not be opened: ") + text;
		return false;
	}

	/*
	 * Get the device name before publishing m_handle.
	 */
	MIDIINCAPSW caps = {};
	if (midiInGetDevCapsW(
			UINT(device),
			&caps,
			sizeof(caps)) == MMSYSERR_NOERROR) {
		m_name = to_utf8(caps.szPname);
	} else {
		m_name = "?";
	}

	m_handle = reinterpret_cast<void *>(h);
	m_closing.store(false, std::memory_order_release);
	m_in_sysex = false;
	rollback();

	/*
	 * SysEx input is deliberately independent from the short-message
	 * path.
	 *
	 * A failure here must NOT prevent MIM_DATA from working.
	 */
	for (int i = 0; i < SYSEX_BUFFERS; ++i) {
		m_sysex[i] = new u8[SYSEX_SIZE];
		m_hdr[i] = new MIDIHDR{};

//		MIDIHDR *hdr = m_hdr[i];


MIDIHDR *hdr =
    reinterpret_cast<MIDIHDR *>(m_hdr[i]);


		hdr->lpData =
			reinterpret_cast<LPSTR>(m_sysex[i]);
		hdr->dwBufferLength = DWORD(SYSEX_SIZE);
		hdr->dwBytesRecorded = 0;
		hdr->dwUser = 0;
		hdr->dwFlags = 0;

		const MMRESULT pr =
			midiInPrepareHeader(
				h,
				hdr,
				sizeof(MIDIHDR));

		if (pr != MMSYSERR_NOERROR) {
			char text[256] = {};

			midiInGetErrorTextA(
				pr,
				text,
				sizeof(text));

			std::fprintf(
				stderr,
				"midiInPrepareHeader[%d] failed: %u (%s)\n",
				i,
				(unsigned)pr,
				text);

//			delete m_hdr[i];
delete reinterpret_cast<MIDIHDR *>(m_hdr[i]);
			m_hdr[i] = nullptr;

			delete[] m_sysex[i];
			m_sysex[i] = nullptr;

			continue;
		}

		const MMRESULT ar =
			midiInAddBuffer(
				h,
				hdr,
				sizeof(MIDIHDR));

		if (ar != MMSYSERR_NOERROR) {
			char text[256] = {};

			midiInGetErrorTextA(
				ar,
				text,
				sizeof(text));

			std::fprintf(
				stderr,
				"midiInAddBuffer[%d] failed: %u (%s)\n",
				i,
				(unsigned)ar,
				text);

			midiInUnprepareHeader(
				h,
				hdr,
				sizeof(MIDIHDR));

			delete reinterpret_cast<MIDIHDR *>(m_hdr[i]);
			m_hdr[i] = nullptr;

			delete[] m_sysex[i];
			m_sysex[i] = nullptr;

			continue;
		}

        // GB 2026 diag
		//std::printf("SysEx buffer %d armed (%lu bytes)\n",	i,	(unsigned long)SYSEX_SIZE);

		std::fflush(stdout);
	}

	/*
	 * Short messages are independent of the SysEx buffers.
	 * Therefore even zero successfully armed SysEx buffers is
	 * not an open failure.
	 */
	const MMRESULT sr = midiInStart(h);

	if (sr != MMSYSERR_NOERROR) {
		char text[256] = {};

		midiInGetErrorTextA(
			sr,
			text,
			sizeof(text));

		std::fprintf(
			stderr,
			"midiInStart() failed: %u (%s)\n",
			(unsigned)sr,
			text);

		close();

		err =
			std::string("MIDI input could not be started: ") +
			text;

		return false;
	}

	std::printf(
		"MIDI input started successfully: %s\n",
		m_name.c_str());

	std::fflush(stdout);

	return true;
}

/*
// コールバックから。使い終わった入れ物を返して、次の SysEx を待たせる
void midi_in::requeue(void *hdr)
{
	if (!m_handle || closing())
		return;
	midiInAddBuffer(reinterpret_cast<HMIDIIN>(m_handle),
	                reinterpret_cast<MIDIHDR *>(hdr), sizeof(MIDIHDR));
}
*/
/*
void midi_in::requeue(void *ptr)
{
	if (!ptr)
		return;

	if (!m_handle || closing())
		return;

	HMIDIIN h =
		reinterpret_cast<HMIDIIN>(m_handle);

	MIDIHDR *hdr =
		reinterpret_cast<MIDIHDR *>(ptr);

	hdr->dwBytesRecorded = 0;

	const MMRESULT r =
		midiInAddBuffer(
			h,
			hdr,
			sizeof(MIDIHDR));

	if (r != MMSYSERR_NOERROR) {
		char text[256] = {};

		midiInGetErrorTextA(
			r,
			text,
			sizeof(text));

		std::fprintf(
			stderr,
			"midiInAddBuffer(requeue) failed: %u (%s)\n",
			(unsigned)r,
			text);

		std::fflush(stderr);
	}
}
*/
void midi_in::requeue(void *ptr)
{
	if (!ptr)
		return;

	if (!m_handle || closing())
		return;

	HMIDIIN h =
		reinterpret_cast<HMIDIIN>(m_handle);

	MIDIHDR *hdr =
		reinterpret_cast<MIDIHDR *>(ptr);

	hdr->dwBytesRecorded = 0;

	midiInAddBuffer(
		h,
		hdr,
		sizeof(MIDIHDR));
}


void midi_in::close()
{
	if (!m_handle)
		return;
	HMIDIIN h = reinterpret_cast<HMIDIIN>(m_handle);
	// 先に印を立てる。midiInReset は入れ物を全部コールバックへ戻すので、
	// そこで返し直すと閉じられなくなる
	m_closing.store(true, std::memory_order_release);

	// 閉じる呼び出しも、相手が固まっていると戻らない。別の糸でやらせる。
	// 入れ物はその糸に渡し、最後まで走ったところで捨てる（mm_open.h）
	void *hdrs[SYSEX_BUFFERS];
	u8   *bufs[SYSEX_BUFFERS];
	for (int i = 0; i < SYSEX_BUFFERS; i++) {
		hdrs[i] = m_hdr[i];
		bufs[i] = m_sysex[i];
		m_hdr[i] = nullptr;
		m_sysex[i] = nullptr;
	}
	std::vector<void *> hv(hdrs, hdrs + SYSEX_BUFFERS);
	std::vector<u8 *>   bv(bufs, bufs + SYSEX_BUFFERS);
	const bool in_time = run_with_timeout([h, hv, bv] {
		midiInStop(h);
		midiInReset(h);
		for (size_t i = 0; i < hv.size(); i++) {
			if (hv[i]) {
				MIDIHDR *hdr = reinterpret_cast<MIDIHDR *>(hv[i]);
				midiInUnprepareHeader(h, hdr, sizeof(MIDIHDR));
				delete hdr;
			}
			delete[] bv[i];
		}
		midiInClose(h);
	});
	if (!in_time)
		std::fprintf(stderr, "MIDI 入力「%s」が閉じる呼び出しに応答しない。置いていく\n",
		             m_name.c_str());
	m_handle = nullptr;
	m_name.clear();
}

// 短いメッセージ（MIM_DATA）。Windows はランニングステータスを省かずに渡してくる
void midi_in::on_short(u32 p1)
{
	const u8 status = u8(p1);
	if (status < 0x80)
		return;
	// リアルタイム（クロックなど）は SysEx の途中に挟まってもよい決まり。
	// SysEx を待っている間は、SysEx と一緒に読ませる
	if (status >= 0xf8) {
		push(status);
		if (!m_in_sysex)
			commit();
		return;
	}
	// それ以外が来たら、線の上では SysEx はそこで終わっている
	if (m_in_sysex) {
		m_in_sysex = false;
		commit();
	}
	// 長さは種別で決まる。プログラムチェンジとチャンネルプレッシャだけ 1 バイト
	int n = 3;
	const u8 kind = status & 0xf0;
	if (kind == 0xc0 || kind == 0xd0) n = 2;
	if (status == 0xf1 || status == 0xf3) n = 2;
	else if (status == 0xf2) n = 3;
	else if (status >= 0xf4 && status < 0xf8) n = 1;

	push(status);
	if (n > 1) push(u8(p1 >> 8));
	if (n > 2) push(u8(p1 >> 16));
	commit();
}

// SysEx の切れ端（MIM_LONGDATA）。長いものは何枚かに分かれて届くので、F7 まで読ませない
void midi_in::on_long(const u8 *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		const u8 v = p[i];
		if (v == 0xf0) {
			if (m_in_sysex)
				commit();                 // F7 の無いまま次が始まった。そこまでで区切る
			m_in_sysex = true;
		}
		if (!m_in_sysex)
			continue;                     // F0 より前の半端なバイト。どこにも属さない
		push(v);
		if (v == 0xf7) {
			m_in_sysex = false;
			commit();
		}
	}
}

void midi_in::push(u8 v)
{
	if (m_overflow)
		return;
	const size_t next = (m_pending + 1) & MASK;
	if (next == m_read.load(std::memory_order_acquire)) {
		m_overflow = true;            // 溢れ。実機の受信バッファ溢れと同じ。このメッセージは捨てる
		return;
	}
	m_buf[m_pending] = v;
	m_pending = next;
	m_bytes.fetch_add(1, std::memory_order_relaxed);
}

void midi_in::commit()
{
	if (m_overflow) {
		rollback();
		return;
	}
	m_write.store(m_pending, std::memory_order_release);
}

void midi_in::rollback()
{
	m_pending = m_write.load(std::memory_order_relaxed);
	m_overflow = false;
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
