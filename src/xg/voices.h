// license:BSD-3-Clause
//
// 音色の名前と楽器の絵を、利用者のプログラム ROM から読む（doc/pc-editor.md）。
//
// 名前も絵もこのリポジトリには入れない。実行時に ROM の中を見るだけ。
// 番地は MU2000 EX（firmware v2.01）で調べたもの。違う版の ROM では ok() が false になり、
// 画面は GM の名前に戻る。
//
// 調べ方（doc/pc-editor.md の「音色の名前と絵」）:
//   * パートの塊の +0xF8 に、firmware が選んだ音色の記録を指す値がある（ROM の中）。
//     記録は「要素の印 1 バイト・1 バイト・名前 10 文字」で始まる
//   * ドラムキットはその値が 0。プログラム → キットの番号の表と、名前 8 文字＋4 バイトの並びを引く
//   * 楽器の絵は 16×16 ドットを 16 ワードで持つ。通常の音色はプログラム番号 → 絵の番号の表で決まる

#ifndef S_MU2000_XG_VOICES_H
#define S_MU2000_XG_VOICES_H

#pragma once

#include "compat/mamecompat.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace xg {

class voice_rom
{
public:
	static constexpr u32 PART_VOICE = 0xf8;      // パートの塊の中の、音色の記録を指す値

	explicit voice_rom(std::shared_ptr<const std::vector<u8>> rom) : m_rom(std::move(rom))
	{
		// 版の確かめ。どれか 1 つでも違えば使わない
		m_ok = m_rom && m_rom->size() == 0x400000 &&
		       !std::memcmp(at(VOICES + 2), "GrandPno", 8) &&
		       !std::memcmp(at(KIT_NAMES), "StandKit", 8) &&
		       !std::memcmp(at(SFX_NAMES), "SFXKit 1", 8) &&
		       word(ICONS) == 0xfffc;
	}

	bool ok() const { return m_ok; }

	// 名前。part_ram はパートの塊（+0xF8 まで含む）。分からなければ空
	std::string name(const u8 *part_ram, int msb, int prog) const
	{
		if (!m_ok)
			return {};
		if (msb == 127 || msb == 126) {
			const u32 map = msb == 127 ? KIT_MAP : SFX_MAP;
			const u32 names = msb == 127 ? KIT_NAMES : SFX_NAMES;
			const u8 idx = (*m_rom)[map + (prog & 0x7f)];
			std::string s(reinterpret_cast<const char *>(at(names + idx * 12)), 8);
			return trim(s);
		}
		const u32 rec = record(part_ram);
		if (!rec)
			return {};
		return trim(std::string(reinterpret_cast<const char *>(at(rec + 2)), 10));
	}

	// 楽器の絵。16 行、各行 16 ビット（上の桁が左）。無ければ false（ドラムはまだ分からない）
	bool icon(const u8 *part_ram, int msb, int prog, u16 rows[16]) const
	{
		if (!m_ok || msb == 127 || msb == 126)
			return false;
		const u32 rec = record(part_ram);
		if (!rec)
			return false;
		int index;
		if (!std::memcmp(at(rec + 2), "Silence", 7))
			index = 56;
		else if (msb == 64)
			index = 57;                                // 効果音
		else
			index = (*m_rom)[ICON_OF_PROGRAM + (prog & 0x7f)];
		const u32 p = ICONS + u32(index) * 32;
		for (int y = 0; y < 16; y++)
			rows[y] = word(p + u32(y) * 2);
		return true;
	}

private:
	static constexpr u32 VOICES          = 0x200ee0;   // 音色の記録の並び
	static constexpr u32 VOICES_END      = 0x23cece;
	static constexpr u32 KIT_MAP         = 0x299fcc;   // バンク 127: プログラム → キットの番号
	static constexpr u32 KIT_NAMES       = 0x299dc0;   //   名前 8 文字 + 4 バイト
	static constexpr u32 SFX_MAP         = 0x29be58;   // バンク 126
	static constexpr u32 SFX_NAMES       = 0x29bdec;
	static constexpr u32 ICON_OF_PROGRAM = 0x1cd044;   // プログラム → 絵の番号
	static constexpr u32 ICONS           = 0x1bbf70;   // 絵。16 ワードずつ

	const u8 *at(u32 a) const { return m_rom->data() + a; }
	u16 word(u32 a) const { return u16((*m_rom)[a] << 8 | (*m_rom)[a + 1]); }

	u32 record(const u8 *part_ram) const
	{
		const u32 r = u32(part_ram[PART_VOICE]) << 24 | u32(part_ram[PART_VOICE + 1]) << 16 |
		              u32(part_ram[PART_VOICE + 2]) << 8 | part_ram[PART_VOICE + 3];
		if (r < VOICES || r + 16 > VOICES_END)
			return 0;
		return r;
	}

	static std::string trim(std::string s)
	{
		while (!s.empty() && (s.back() == ' ' || s.back() == 0))
			s.pop_back();
		return s;
	}

	std::shared_ptr<const std::vector<u8>> m_rom;
	bool m_ok = false;
};

} // namespace xg

#endif // S_MU2000_XG_VOICES_H
