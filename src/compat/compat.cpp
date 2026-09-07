// license:BSD-3-Clause
//
// 互換層の実体。ログと、移植の突き合わせ用の PC 追跡。

#include "mamecompat.h"

#include <cstdio>

namespace smu2000 {

bool g_verbose = false;

// PC の追跡。MAME の debugger_instruction_hook に相当する。
// 出力先が入っているときだけ書く。MAME 側は debugger の trace コマンドで
// 同じものが取れるので、突き合わせて最初に食い違う命令を探せる
std::FILE *g_pc_trace = nullptr;
u64        g_pc_trace_left = 0;

void pc_trace(u32 pc)
{
	if (!g_pc_trace_left) {
		g_pc_trace = nullptr;
		return;
	}
	g_pc_trace_left--;
	std::fprintf(g_pc_trace, "%08X\n", pc);
}

} // namespace smu2000
