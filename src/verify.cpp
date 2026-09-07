// 移植が成立しているかの最小確認
#include "mame/sound/swp30.h"
#include <cstdio>

int main()
{
	std::vector<u8>  wave(64 * 1024 * 1024, 0);   // 波形 ROM 相当のダミー
	std::vector<u16> sintab(0x8000, 0);

	swp30_device swp;
	swp.set_wave_rom(wave.data(), wave.size());
	swp.set_sintab(sintab.data(), sintab.size());
	swp.reset();

	// レジスタの読み書きが素通しできるか（ピッチ = slot 0x11）
	swp.write16(0 * 0x40 + 0x11, 0x1234);
	u16 back = swp.read16(0 * 0x40 + 0x11);
	std::printf("ピッチレジスタ 書き 0x1234 -> 読み 0x%04x  %s\n",
	            back, back == 0x1234 ? "一致" : "不一致");

	// 1 サンプル回してみる（無音のはず）
	s32 l = 0, r = 0;
	for (int i = 0; i < 100; i++) swp.run_sample(l, r);
	std::printf("100 サンプル実行  最後の出力 L=%d R=%d\n", l, r);

	// 乱数が MAME と同じ数列か
	// printf の引数は評価順が未規定。1 個ずつ取り出さないと順序が入れ替わる
	std::printf("乱数 1〜3 個目:");
	for (int i = 0; i < 3; i++) std::printf(" %08x", swp.machine().rand());
	std::printf("\n");
	return 0;
}
