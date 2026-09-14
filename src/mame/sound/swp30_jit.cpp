// license:BSD-3-Clause
//
// S-MU2000: MEG のプログラムをその場で x86-64 の機械語にする（JIT）。
//
// firmware が MEG に書いたプログラムを、プログラムか番地の割り当てが変わるたびに訳し直す。
// 訳した機械語は meg_state::run_program() と**ビット単位で同じこと**をする:
//
//   * 命令ごとの判定（ALU の種類・読み先・書き先・メモリ操作）は訳すときに決める
//   * 3 命令遅れの書き込み・2 命令遅れのメモリポートと t の値は、遅れの輪のどの枠かが
//     訳すときに決まるので、反映する命令の位置に直に置く。サンプルを跨ぐ分
//     （頭の 3 命令が読む枠と、終わりの 3 命令が書く枠）だけ輪そのものを使う
//   * 定数・番地表・LFO は firmware が動かしている最中にも書くので、実行時に読む
//   * 乱数（ディザ）は同じ順に同じ回数だけ引く
//
// 分岐（bit 0x3f）を含むプログラムは訳さずに run_program() に任せる（LO-FI と DYNA 系だけ）。
// 訳した物はどこにも保存しない（firmware 由来のものを配らない。実行時に作って捨てる）。
//
// x86-64（Windows・macOS・Linux）と arm64 で使う。
// そのほかでは build() が false を返し、今までどおり解釈実行する。

#include "swp30.h"

#if defined(__x86_64__) || defined(__aarch64__)
#define SMU2000_MEG_JIT 1
#include "compat/exec_mem.h"
#ifdef __aarch64__
#include "a64asm.h"
#else
#include "x64asm.h"
#endif
#else
#define SMU2000_MEG_JIT 0
#endif

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

#if defined(__x86_64__)

using namespace x64asm;
using meg_asm = assembler;
using meg_arg_t = x64_arg0_t;

// meg_state::revram_encode と同じことをする。入力 eax（u32）、出力 eax（u16）。rcx と rdx を壊す
void emit_revram_encode(assembler &a)
{
	a.and32i(RAX, 0x7ffffff);
	a.xor32(RDX, RDX);                                   // s
	a.test32ri(RAX, 0x4000000);
	const size_t pos = a.jcc_fwd(0x84);
	a.xor32ri(RAX, 0x7ffffff);
	a.imm32(RDX, 1);
	a.patch(pos);
	// e は bit 11〜25 のうち一番上の 1 の位置 - 10。無ければ e = 0 で m = v（v < 0x800）
	a.mov32(RCX, RAX);
	a.shr32(RCX, 11);
	const size_t small = a.jcc_fwd(0x84);
	a.bsr32(RCX, RAX);
	a.sub32ri(RCX, 11);                                  // e - 1
	a.shr32cl(RAX);
	a.and32i(RAX, 0x7ff);
	a.add32ri(RCX, 1);
	a.shl32(RCX, 12);
	a.or32(RAX, RCX);
	a.patch(small);
	a.shl32(RDX, 11);
	a.or32(RAX, RDX);
}

// meg_state::m1_expand と同じことをする。入力 eax（下の 16bit が s16）、出力 rax（0〜0x7ffc）。rcx を壊す
void emit_m1_expand(assembler &a)
{
	a.test32ri(RAX, 0x8000);
	const size_t neg = a.jcc_fwd(0x85);
	a.mov32(RCX, RAX);
	a.shr32(RCX, 12);
	a.and32i(RCX, 7);                                    // s
	a.and32i(RAX, 0xfff);
	a.or32ri(RAX, 0x1000);
	a.cmp32ri(RCX, 5);
	const size_t done1 = a.jcc_fwd(0x84);                // s == 5
	const size_t less = a.jcc_fwd(0x82);                 // s < 5（jb）
	a.sub32ri(RCX, 5);
	a.shl32cl(RAX);
	const size_t done2 = a.jmp_fwd();
	a.patch(less);
	a.neg32(RCX);
	a.add32ri(RCX, 5);
	a.shr32cl(RAX);
	const size_t done3 = a.jmp_fwd();
	a.patch(neg);
	a.xor32(RAX, RAX);
	a.patch(done1);
	a.patch(done2);
	a.patch(done3);
}

// meg_state::revram_decode と同じことをする。入力 eax（u16）、出力 eax。rcx rdx r8 を壊す
void emit_revram_decode(assembler &a)
{
	a.mov32(R8, RAX);                                    // v
	a.mov32(RCX, RAX);
	a.shr32(RCX, 12);                                    // e
	a.and32i(RAX, 0x7ff);                                // m
	a.test32(RCX, RCX);
	const size_t e0 = a.jcc_fwd(0x84);
	a.or32ri(RAX, 0x800);
	a.sub32ri(RCX, 1);
	a.shl32cl(RAX);
	a.imm32(RDX, 0xffffffff);
	a.shl32cl(RDX);
	const size_t join = a.jmp_fwd();
	a.patch(e0);
	a.imm32(RDX, 0xffffffe0);
	a.patch(join);
	a.test32ri(R8, 0x800);
	const size_t no_sign = a.jcc_fwd(0x84);
	a.xor32(RAX, RDX);
	a.patch(no_sign);
}

#elif defined(__aarch64__)

using namespace a64;
using meg_asm = emitter;
using meg_arg_t = u32;

// The three helpers work on the same scratch registers the JIT uses, so one can
// be dropped into a block exactly as it stands: HA carries the value in and
// out, HC/HD/HM are scratch. The W forms are used throughout because the JIT's
// x86 counterpart also works on the low 32 bits and the W writes clear the top
// half of the X register, which is what the callers expect to read back.
enum : u8 { HA = X2, HC = X3, HD = X4, HE = X5, HM = X7 };

// Same as meg_state::revram_encode. Input HA (u32), result HA (low 16 bits).
// HC and HD are clobbered.
void emit_revram_encode(emitter &a)
{
	a.and_imm(HA, HA, 0x7ffffff);
	a.eor_reg(HD, HD, HD);                               // s
	a.tst_imm(HA, 0x4000000);
	const size_t pos = a.b_cond(EQ);
	a.eor_imm(HA, HA, 0x7ffffff);
	a.mov_imm32(HD, 1);
	a.patch(pos);
	// e is the highest set bit minus 10 when the value reaches bit 11; below
	// that e = 0 and m is the value itself
	a.lsr_imm(HC, HA, 11);
	const size_t small = a.cbz_w(HC);
	a.clz(HC, HA);
	a.mov_imm32(HM, 31);
	a.sub_reg(HC, HM, HC);                               // bsr(v)
	a.sub_imm(HC, HC, 11);                               // e - 1
	a.lsrv(HA, HA, HC);
	a.and_imm(HA, HA, 0x7ff);
	a.add_imm(HC, HC, 1);                                // e
	a.lsl_imm(HC, HC, 12);
	a.orr_reg(HA, HA, HC);
	a.patch(small);
	a.lsl_imm(HD, HD, 11);
	a.orr_reg(HA, HA, HD);
}

// Same as meg_state::m1_expand. Input HA (a sign-extended s16 in the low half),
// result HA (0..0x7ffc). HC is clobbered.
void emit_m1_expand(emitter &a)
{
	a.tst_imm(HA, 0x8000);
	const size_t neg = a.b_cond(NE);
	a.lsr_imm(HC, HA, 12);
	a.and_imm(HC, HC, 7);                                // s
	a.and_imm(HA, HA, 0xfff);
	a.or_imm(HA, HA, 0x1000);
	a.cmp_imm(HC, 5);
	const size_t done1 = a.b_cond(EQ);
	const size_t less = a.b_cond(LO);
	a.sub_imm(HC, HC, 5);
	a.lslv(HA, HA, HC);
	const size_t done2 = a.b();
	a.patch(less);
	a.neg_reg(HC, HC);
	a.add_imm(HC, HC, 5);
	a.lsrv(HA, HA, HC);
	const size_t done3 = a.b();
	a.patch(neg);
	a.eor_reg(HA, HA, HA);
	a.patch(done1);
	a.patch(done2);
	a.patch(done3);
}

// Same as meg_state::revram_decode. Input HA (u16), result HA. HC, HD and HE
// are clobbered.
void emit_revram_decode(emitter &a)
{
	a.mov_reg(HE, HA);                                   // v
	a.lsr_imm(HC, HA, 12);                               // e
	a.and_imm(HA, HA, 0x7ff);                            // m
	const size_t e0 = a.cbz_w(HC);
	a.or_imm(HA, HA, 0x800);
	a.sub_imm(HC, HC, 1);
	a.lslv(HA, HA, HC);
	a.mov_imm32(HD, 0xffffffff);
	a.lslv(HD, HD, HC);
	const size_t join = a.b();
	a.patch(e0);
	a.mov_imm32(HD, 0xffffffe0);
	a.patch(join);
	a.tst_imm(HE, 0x800);
	const size_t no_sign = a.b_cond(EQ);
	a.eor_reg(HA, HA, HD);
	a.patch(no_sign);
}

#endif

} // namespace


struct swp30_device::meg_jit {
	using fn_t = void (*)(meg_state *, swp30_device *, u16 *);
	fn_t fn = nullptr;
	void *buf = nullptr;
	size_t buf_size = 0;
	u32 d3 = 0, d2 = 0;

	~meg_jit()
	{
#if SMU2000_MEG_JIT
		if (buf)
			exec_mem::free_mem(buf, buf_size);
#endif
	}

#if SMU2000_MEG_JIT
	static u32 call_lfo(meg_state *ms, u32 lfo) { return ms->get_lfo(int(lfo)); }
#endif

	bool build(meg_state &ms, const meg_state::op *ops, swp30_device &swp);
};

void swp30_device::meg_jit_delete(meg_jit *j)
{
	delete j;
}

bool swp30_device::meg_jit_enabled()
{
#if SMU2000_MEG_JIT
	static const bool on = [] {
		const char *e = std::getenv("SMU2000_MEG_JIT");
		return !(e && e[0] == '0');
	}();
	return on;
#else
	return false;
#endif
}

void swp30_device::meg_jit_rebuild()
{
	if (!meg_jit_enabled()) {
		m_jit.reset();
		return;
	}
	if (!m_jit)
		m_jit.reset(new meg_jit);
	if (!m_jit->build(*m_meg, m_meg_ops.data(), *this))
		m_jit->fn = nullptr;
}

bool swp30_device::meg_jit_run()
{
	meg_jit *j = m_jit.get();
	if (!j || !j->fn || j->d3 != m_meg->m_delay_3 || j->d2 != m_meg->m_delay_2)
		return false;
	static const bool check = [] {
		const char *e = std::getenv("SMU2000_MEG_JIT_CHECK");
		return e && e[0] != '0';
	}();
	if (check) {
		// DEBUG: run the block through the JIT, then through the interpreter from
		// the same starting state, and report the first field that differs.
		static int shown = 0;
		meg_state before(*m_meg);
		std::vector<u16> ram0(m_reverb_ram);
		u32 seed0 = m_rand_seed;
		bool fn0 = m_meg_flag_n, fz0 = m_meg_flag_z;
		j->fn(m_meg, this, m_reverb_ram.data());
		const meg_state jit(*m_meg);
		const std::vector<u16> ramj(m_reverb_ram);
		const u32 seedj = m_rand_seed;
		const bool fnj = m_meg_flag_n, fzj = m_meg_flag_z;
		*m_meg = before;
		m_reverb_ram = ram0;
		m_rand_seed = seed0;
		m_meg_flag_n = fn0;
		m_meg_flag_z = fz0;
		if (const char *e = std::getenv("SMU2000_MEG_JIT_UPTO")) {
			const int upto = std::atoi(e);
			for (int i = 0; i != upto; i++)
				m_meg->step();
		} else
			m_meg->run_program(m_meg_ops.data());
		const u8 *jb = reinterpret_cast<const u8 *>(&jit);
		const u8 *ib = reinterpret_cast<const u8 *>(m_meg);
		// run_program() subtracts the 0x180 icount itself, so the icount (and the
		// retval next to it, which only the helper-call paths write) is expected
		// to differ here; stop short of it
		const size_t end = size_t(reinterpret_cast<const u8 *>(&m_meg->m_icount) - ib);
		size_t first = end;
		for (size_t i = 0; i < end; i++)
			if (jb[i] != ib[i]) { first = i; break; }
		size_t rambad = 0, ramfirst = 0;
		for (size_t i = 0; i < ramj.size(); i++)
			if (ramj[i] != m_reverb_ram[i]) { if (!rambad) ramfirst = i; rambad++; }
		if ((first != end || rambad || seedj != m_rand_seed || fnj != m_meg_flag_n || fzj != m_meg_flag_z) && shown < 40) {
			shown++;
			std::fprintf(stderr, "MEGCHECK sample %d state@%zu/%zu ram %zu (first %zu) seed %s flags %s%s\n",
				shown, first, end, rambad, ramfirst,
				seedj == m_rand_seed ? "ok" : "BAD", fnj == m_meg_flag_n ? "ok" : "BAD", fzj == m_meg_flag_z ? "ok" : "BAD");
			if (shown == 1) {
				for (int b = 0; b < 0x40; b += 8) {
					std::fprintf(stderr, "  m[%02x..] jit", b);
					for (int i = 0; i < 8; i++) std::fprintf(stderr, " %8d", jit.m_m[b + i]);
					std::fprintf(stderr, "\n         interp");
					for (int i = 0; i < 8; i++) std::fprintf(stderr, " %8d", m_meg->m_m[b + i]);
					std::fprintf(stderr, "\n");
				}
				std::fprintf(stderr, "  jit    m32 %d m33 %d m48 %d m49 %d mwv %d,%d,%d mwr %d,%d,%d\n",
					jit.m_m[32], jit.m_m[33], jit.m_m[48], jit.m_m[49],
					jit.m_mw_value[0], jit.m_mw_value[1], jit.m_mw_value[2], jit.m_mw_reg[0], jit.m_mw_reg[1], jit.m_mw_reg[2]);
				std::fprintf(stderr, "  interp m32 %d m33 %d m48 %d m49 %d mwv %d,%d,%d mwr %d,%d,%d\n",
					m_meg->m_m[32], m_meg->m_m[33], m_meg->m_m[48], m_meg->m_m[49],
					m_meg->m_mw_value[0], m_meg->m_mw_value[1], m_meg->m_mw_value[2], m_meg->m_mw_reg[0], m_meg->m_mw_reg[1], m_meg->m_mw_reg[2]);
				for (u32 k = 0; k != 0x180; k++) {
					const meg_state::op &o = m_meg_ops[k];
					if (o.dm == 32 || o.dm == 33 || o.dm == 48 || o.dm == 49)
						std::fprintf(stderr, "  ops[%u] dm=%d dm_src=%d mmode=%d asel=%d rop=%d shift=%d clamp=%d sm=%d sr=%d alu=%d\n",
							k, o.dm, o.dm_src, o.mmode, o.asel, o.rop, o.shift, o.clamp, o.sm, o.sr, o.alu);
				}
				std::fprintf(stderr, "  before m32 %d m33 %d m48 %d m49 %d p %lld mwv %d,%d,%d mwr %d,%d,%d d3 %d d2 %d\n",
					before.m_m[32], before.m_m[33], before.m_m[48], before.m_m[49], (long long)before.m_p,
					before.m_mw_value[0], before.m_mw_value[1], before.m_mw_value[2], before.m_mw_reg[0], before.m_mw_reg[1], before.m_mw_reg[2], before.m_delay_3, before.m_delay_2);
			}
			for (int i = 0; i < 0x40; i++)
				if (jit.m_m[i] != m_meg->m_m[i]) std::fprintf(stderr, "  m[%d] jit %d interp %d\n", i, jit.m_m[i], m_meg->m_m[i]);
			for (int i = 0; i < 0x80; i++)
				if (jit.m_r[i] != m_meg->m_r[i]) std::fprintf(stderr, "  r[%d] jit %d interp %d\n", i, jit.m_r[i], m_meg->m_r[i]);
			for (int i = 0; i < 8; i++)
				if (jit.m_t[i] != m_meg->m_t[i]) std::fprintf(stderr, "  t[%d] jit %d interp %d\n", i, jit.m_t[i], m_meg->m_t[i]);
			if (jit.m_p != m_meg->m_p) std::fprintf(stderr, "  p jit %lld interp %lld\n", (long long)jit.m_p, (long long)m_meg->m_p);
			if (jit.m_ram_index != m_meg->m_ram_index) std::fprintf(stderr, "  ix jit %d interp %d\n", jit.m_ram_index, m_meg->m_ram_index);
			if (jit.m_ram_read != m_meg->m_ram_read) std::fprintf(stderr, "  rr jit %d interp %d\n", jit.m_ram_read, m_meg->m_ram_read);
			if (jit.m_ram_write != m_meg->m_ram_write) std::fprintf(stderr, "  rw jit %d interp %d\n", jit.m_ram_write, m_meg->m_ram_write);
			if (jit.m_delay_3 != m_meg->m_delay_3 || jit.m_delay_2 != m_meg->m_delay_2) std::fprintf(stderr, "  d3/d2 jit %d,%d interp %d,%d\n", jit.m_delay_3, jit.m_delay_2, m_meg->m_delay_3, m_meg->m_delay_2);
			for (int i = 0; i < 3; i++) {
				if (jit.m_mw_value[i] != m_meg->m_mw_value[i] || jit.m_mw_reg[i] != m_meg->m_mw_reg[i]) std::fprintf(stderr, "  mw[%d] jit %d/%d interp %d/%d\n", i, jit.m_mw_value[i], jit.m_mw_reg[i], m_meg->m_mw_value[i], m_meg->m_mw_reg[i]);
				if (jit.m_rw_value[i] != m_meg->m_rw_value[i] || jit.m_rw_reg[i] != m_meg->m_rw_reg[i]) std::fprintf(stderr, "  rw[%d] jit %d/%d interp %d/%d\n", i, jit.m_rw_value[i], jit.m_rw_reg[i], m_meg->m_rw_value[i], m_meg->m_rw_reg[i]);
				if (jit.m_index_value[i] != m_meg->m_index_value[i] || jit.m_index_active[i] != m_meg->m_index_active[i]) std::fprintf(stderr, "  ixv[%d] jit %d/%d interp %d/%d\n", i, jit.m_index_value[i], jit.m_index_active[i], m_meg->m_index_value[i], m_meg->m_index_active[i]);
				if (jit.m_memw_value[i] != m_meg->m_memw_value[i] || jit.m_memw_active[i] != m_meg->m_memw_active[i]) std::fprintf(stderr, "  memw[%d] jit %d/%d interp %d/%d\n", i, jit.m_memw_value[i], jit.m_memw_active[i], m_meg->m_memw_value[i], m_meg->m_memw_active[i]);
				if (jit.m_memr_value[i] != m_meg->m_memr_value[i] || jit.m_memr_active[i] != m_meg->m_memr_active[i]) std::fprintf(stderr, "  memr[%d] jit %d/%d interp %d/%d\n", i, jit.m_memr_value[i], jit.m_memr_active[i], m_meg->m_memr_value[i], m_meg->m_memr_active[i]);
			}
			for (int i = 0; i < 2; i++)
				if (jit.m_t_value[i] != m_meg->m_t_value[i]) std::fprintf(stderr, "  tv[%d] jit %d interp %d\n", i, jit.m_t_value[i], m_meg->m_t_value[i]);
		}
	}
	j->fn(m_meg, this, m_reverb_ram.data());
	m_meg->m_pc = 0;
	m_meg->m_icount -= 0x180;
	return true;
}

// 機械語にしたリバーブ RAM の詰め方・戻し方を、meg_state の関数と全部の入力で突き合わせる。
// 食い違った入力の数を返す（JIT が無い環境では 0）。make test の verify から呼ぶ
u64 swp30_device::meg_jit_selftest()
{
#if SMU2000_MEG_JIT
	u64 bad = 0;
	for (int which = 0; which < 3; which++) {
		meg_asm a;
#ifdef __aarch64__
		a.mov_reg(HA, W0);                              // the value arrives in w0
		if (which == 0) emit_revram_encode(a); else if (which == 1) emit_revram_decode(a); else emit_m1_expand(a);
		a.mov_reg(W0, HA);                              // and the result goes back in w0
#else
		a.mov32(RAX, ARG0);
		if (which == 0) emit_revram_encode(a); else if (which == 1) emit_revram_decode(a); else emit_m1_expand(a);
#endif
		a.ret();
		// RW buffer, made executable after the copy (the code is bytes on x86
		// and 32-bit instructions on arm64, hence code[0] rather than a literal)
		const size_t bytes = a.code.size() * sizeof(a.code[0]);
		void *buf = exec_mem::alloc_rw(bytes);
		if (!buf)
			return ~u64(0);
		std::memcpy(buf, a.code.data(), bytes);
		if (!exec_mem::make_executable(buf, bytes)) {
			exec_mem::free_mem(buf, bytes);
			return ~u64(0);
		}
		const auto fn = reinterpret_cast<u32 (*)(meg_arg_t)>(buf);
		if (which == 0) {
			for (u32 v = 0; v < 0x8000000; v++)        // encode は下の 27bit しか見ない
				if ((fn(v) & 0xffff) != meg_state::revram_encode(v))
					bad++;
			for (u32 v : { 0xffffffffu, 0x80000000u, 0xf8000001u })
				if ((fn(v) & 0xffff) != meg_state::revram_encode(v))
					bad++;
		} else if (which == 1) {
			for (u32 v = 0; v < 0x10000; v++)
				if (fn(v) != meg_state::revram_decode(u16(v)))
					bad++;
		} else {
			// 呼ぶ側は loads16 で 64bit に符号拡張した値を渡す。出力は 64bit のまま使う
			const auto fn64 = reinterpret_cast<s64 (*)(s64)>(buf);
			for (s32 v = -0x8000; v < 0x8000; v++)
				if (fn64(v) != s64(meg_state::m1_expand(s16(v))))
					bad++;
		}
		exec_mem::free_mem(buf, bytes);
	}
	return bad;
#else
	return 0;
#endif
}

#if !SMU2000_MEG_JIT

bool swp30_device::meg_jit::build(meg_state &, const meg_state::op *, swp30_device &)
{
	return false;
}

#elif defined(__x86_64__)

bool swp30_device::meg_jit::build(meg_state &ms, const meg_state::op *ops, swp30_device &swp)
{
	fn = nullptr;
	for (u32 pc = 0; pc != 0x180; pc++)
		if (ops[pc].jump)
			return false;
	if (swp.m_reverb_ram.size() < 0x40000)
		return false;

	d3 = ms.m_delay_3;
	d2 = ms.m_delay_2;
	const auto slot3 = [&](u32 k) { return (d3 + k) % 3; };
	const auto slot2 = [&](u32 k) { return (d2 + k) % 2; };

	// 要素の位置（meg_state と swp30_device の中）
	const auto off = [](const void *base, const void *field) { return s32(intptr_t(field) - intptr_t(base)); };
	const s32 o_m        = off(&ms, ms.m_m.data());
	const s32 o_r        = off(&ms, ms.m_r.data());
	const s32 o_t        = off(&ms, ms.m_t.data());
	const s32 o_p        = off(&ms, &ms.m_p);
	const s32 o_const    = off(&ms, ms.m_const.data());
	const s32 o_offset   = off(&ms, ms.m_offset.data());
	const s32 o_mw_value = off(&ms, ms.m_mw_value.data());
	const s32 o_mw_reg   = off(&ms, ms.m_mw_reg.data());
	const s32 o_rw_value = off(&ms, ms.m_rw_value.data());
	const s32 o_rw_reg   = off(&ms, ms.m_rw_reg.data());
	const s32 o_ix_value = off(&ms, ms.m_index_value.data());
	const s32 o_ix_act   = off(&ms, ms.m_index_active.data());
	const s32 o_memw_val = off(&ms, ms.m_memw_value.data());
	const s32 o_memr_val = off(&ms, ms.m_memr_value.data());
	const s32 o_t_value  = off(&ms, ms.m_t_value.data());
	const s32 o_memw_act = off(&ms, ms.m_memw_active.data());
	const s32 o_memr_act = off(&ms, ms.m_memr_active.data());
	const s32 o_ram_read = off(&ms, &ms.m_ram_read);
	const s32 o_ram_write = off(&ms, &ms.m_ram_write);
	const s32 o_ram_index = off(&ms, &ms.m_ram_index);
	const s32 o_sample   = off(&ms, &ms.m_sample_counter);
	const s32 o_seed     = off(&swp, &swp.m_rand_seed);
	const s32 o_flag_n   = off(&swp, &swp.m_meg_flag_n);
	const s32 o_flag_z   = off(&swp, &swp.m_meg_flag_z);

	if (sizeof(ms.m_mw_reg[0]) != 1 || sizeof(ms.m_index_active[0]) != 1 || sizeof(ms.m_memw_active[0]) != 1 ||
	    sizeof(swp.m_meg_flag_n) != 1 || sizeof(ms.m_t_value[0]) != 2 || sizeof(ms.m_const[0]) != 2 ||
	    sizeof(ms.m_offset[0]) != 2 || sizeof(ms.m_m[0]) != 4 || sizeof(ms.m_r[0]) != 4)
		return false;

	// t の値（2 命令遅れ）を書いておく必要がある命令: 2 つ後に t を p から書く命令があるか、終わりの 2 つ
	bool need_tval[0x180] = {};
	for (u32 k = 0; k != 0x180; k++) {
		if (k >= 0x17e)
			need_tval[k] = true;
		if (k + 2 < 0x180 && ops[k + 2].t_write && ops[k + 2].t_from_p)
			need_tval[k] = true;
	}

	assembler a;
	const u8 MS = RBX, SWP = R12, P = R13, SC = R14, RAM = R15;
	const auto M = [&](s32 disp) { return mem{MS, NOREG, 1, disp}; };

	// 入口（引数は ARG0 = ms、ARG1 = swp、ARG2 = リバーブ RAM。Windows x64 でも SysVでも）
	a.push(RBX); a.push(R12); a.push(R13); a.push(R14); a.push(R15);
	a.subrsp(48);                                    // 呼ぶ先の影 32 + 自分の置き場 16。rsp は 16 の倍数
	a.mov64(MS, ARG0);
	a.mov64(SWP, ARG1);
	a.mov64(RAM, ARG2);
	a.load64(P, M(o_p));
	a.load32(SC, M(o_sample));

	// p を 24bit に詰める（meg_pack24）。入力 rax、出力 eax
	const auto pack24 = [&]() {
		a.sar64(RAX, 15);
		a.imm64(RCX, u64(s64(-0x800000)));
		a.cmp64(RAX, RCX);
		a.cmovl64(RAX, RCX);
		a.imm64(RCX, 0x7fffff);
		a.cmp64(RAX, RCX);
		a.cmovg64(RAX, RCX);
	};
	// 乱数を 1 つ引く（swp30_device::rand）。出力 eax
	const auto rnd = [&]() {
		a.load32(RAX, mem{SWP, NOREG, 1, o_seed});
		a.imul32i(RAX, RAX, 1664525);
		a.add32i(RAX, 1013904223);
		a.store32(mem{SWP, NOREG, 1, o_seed}, RAX);
		a.rol32(RAX, 16);
	};
	// p に雑音を足して詰める（dm の 6 番、dr の p）。出力 eax
	const auto p_packed = [&](bool noise) {
		if (noise) {
			rnd();
			a.and32i(RAX, 0x07e0);
			a.mov64(RDX, RAX);
			a.mov64(RAX, P);
			a.add64(RAX, RDX);
		} else
			a.mov64(RAX, P);
		pack24();
	};

	for (u32 k = 0; k != 0x180; k++) {
		const meg_state::op &o = ops[k];

		// ---- 反映（遅れて入る書き込み）----
		if (k < 3) {
			const u32 s = slot3(k);
			// m
			a.loadu8(RAX, M(o_mw_reg + s));
			a.test32(RAX, RAX);
			size_t j1 = a.jz_fwd();
			a.load32(RCX, M(o_mw_value + 4 * s));
			a.store32(mem{MS, RAX, 4, o_m}, RCX);
			a.patch(j1);
			// r
			a.loadu8(RAX, M(o_rw_reg + s));
			a.test32(RAX, RAX);
			size_t j2 = a.jz_fwd();
			a.load32(RCX, M(o_rw_value + 4 * s));
			a.store32(mem{MS, RAX, 4, o_r}, RCX);
			a.patch(j2);
			// index
			a.loadu8(RAX, M(o_ix_act + s));
			a.test32(RAX, RAX);
			size_t j3 = a.jz_fwd();
			a.load32(RCX, M(o_ix_value + 4 * s));
			a.store32(M(o_ram_index), RCX);
			a.patch(j3);
		} else {
			const meg_state::op &w = ops[k - 3];
			const u32 s = slot3(k);
			if (w.dm) {
				a.load32(RCX, M(o_mw_value + 4 * s));
				a.store32(M(o_m + 4 * w.dm), RCX);
			}
			if (w.dr) {
				a.load32(RCX, M(o_rw_value + 4 * s));
				a.store32(M(o_r + 4 * w.dr), RCX);
			}
			if (w.index) {
				a.load32(RCX, M(o_ix_value + 4 * s));
				a.store32(M(o_ram_index), RCX);
			}
		}
		if (k < 2) {
			const u32 s = slot2(k);
			a.loadu8(RAX, M(o_memw_act + s));
			a.test32(RAX, RAX);
			size_t j1 = a.jz_fwd();
			a.load32(RCX, M(o_memw_val + 4 * s));
			a.store32(M(o_ram_write), RCX);
			a.store8i(M(o_memw_act + s), 0);
			a.patch(j1);
			a.loadu8(RAX, M(o_memr_act + s));
			a.test32(RAX, RAX);
			size_t j2 = a.jz_fwd();
			a.load32(RCX, M(o_memr_val + 4 * s));
			a.store32(M(o_ram_read), RCX);
			a.store8i(M(o_memr_act + s), 0);
			a.patch(j2);
		} else {
			const meg_state::op &w = ops[k - 2];
			const u32 s = slot2(k);
			if (w.memw) {
				a.load32(RCX, M(o_memw_val + 4 * s));
				a.store32(M(o_ram_write), RCX);
			}
			if (w.memop == 2 || w.memop == 3) {
				a.load32(RCX, M(o_memr_val + 4 * s));
				a.store32(M(o_ram_read), RCX);
			}
		}

		// ---- ALU ----
		if (o.alu) {
			if (o.m1_from_t)
				a.loads16(RAX, M(o_t + 2 * o.t));
			else
				a.loads16(RAX, M(o_const + 2 * s32(k)));
			if (o.m1_expand)
				emit_m1_expand(a);                           // meg_state::m1_expand を機械語で
			switch (o.mmode) {
			case 1:
				a.shl64(RAX, 8 + 15);
				break;
			case 2:
				a.loads32(RCX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.imul64(RAX, RCX);
				break;
			default:
				a.loads32(RAX, o.m2_from_m ? M(o_m + 4 * o.sm) : M(o_r + 4 * o.sr));
				a.shl64(RAX, 15);
				break;
			}
			switch (o.asel) {
			case 0: a.mov64(RCX, P); break;
			case 1: a.loads32(RCX, M(o_r + 4 * o.sr)); a.shl64(RCX, 15); break;
			case 2: a.loads32(RCX, M(o_m + 4 * o.sm)); a.shl64(RCX, 15); break;
			case 3: a.mov64(RCX, P); a.sar64(RCX, 15); break;
			default: a.xor32(RCX, RCX); break;
			}
			switch (o.rop) {
			case 0: a.add64(RAX, RCX); break;
			case 1: a.sub64(RAX, RCX); break;
			case 2:
				a.mov64(RDX, RCX);
				a.neg64(RDX);
				a.cmovs64(RDX, RCX);
				a.add64(RAX, RDX);
				break;
			default: a.and64(RAX, RCX); break;
			}
			if (o.shift)
				a.shl64(RAX, o.shift);
			a.shl64(RAX, 22);
			a.sar64(RAX, 22);
			switch (o.clamp) {
			case 0: break;
			case 1:
				a.imm64(RCX, u64(s64(-0x4000000000)));
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x3fffffffff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				break;
			case 2:
				a.xor32(RCX, RCX);
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x3fffffffff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				break;
			default:
				a.mov64(RDX, RAX);
				a.neg64(RDX);
				a.cmovs64(RDX, RAX);
				a.mov64(RAX, RDX);
				a.imm64(RCX, 0x3fffffffff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
				break;
			}
			a.mov64(P, RAX);
			if (o.latch) {
				a.test64(P, P);
				a.setl_mem(mem{SWP, NOREG, 1, o_flag_n});
				a.test64(P, P);
				a.sete_mem(mem{SWP, NOREG, 1, o_flag_z});
			}
		}

		// ---- dm ----
		if (o.dm) {
			switch (o.dm_src) {
			case 0: case 1: case 2: case 3:
				a.mov64(ARG0, MS);
				a.imm32(ARG1, o.lfo);
				a.call_abs(reinterpret_cast<void *>(&meg_jit::call_lfo));
				break;
			case 4:
				a.load32(RAX, M(o_ram_read));
				break;
			case 5:
				rnd();
				a.shl32(RAX, 8);
				a.sar32(RAX, 8);
				break;
			case 6:
				p_packed(!o.no_noise);
				break;
			default:
				a.load32(RAX, M(o_m + 4 * o.sm));
				break;
			}
			a.store32(M(o_mw_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d)
			a.store8i(M(o_mw_reg + slot3(k)), o.dm);

		// ---- dr ----
		if (o.dr) {
			if (o.dr_from_r)
				a.load32(RAX, M(o_r + 4 * o.sr));
			else
				p_packed(!o.no_noise);
			a.store32(M(o_rw_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d)
			a.store8i(M(o_rw_reg + slot3(k)), o.dr);

		// ---- メモリへの書き値 ----
		if (o.memw) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15);
			a.store32(M(o_memw_val + 4 * slot2(k)), RAX);
		}
		if (k >= 0x17e)
			a.store8i(M(o_memw_act + slot2(k)), o.memw ? 1 : 0);

		// ---- index ----
		if (o.index) {
			a.mov64(RAX, P);
			a.sar64(RAX, 15 + 8);
			a.store32(M(o_ix_value + 4 * slot3(k)), RAX);
		}
		if (k >= 0x17d)
			a.store8i(M(o_ix_act + slot3(k)), o.index ? 1 : 0);

		// ---- t ----
		if (o.t_write) {
			if (o.t_from_p)
				a.loadu16(RAX, M(o_t_value + 2 * slot2(k)));
			else
				a.loadu16(RAX, M(o_const + 2 * s32(k)));
			a.store16(M(o_t + 2 * o.t), RAX);
		}
		if (need_tval[k]) {
			a.mov64(RAX, P);
			if (o.index) {
				a.sar64(RAX, 8);
				a.and32i(RAX, 0x7fff);
			} else {
				a.sar64(RAX, 15 + 8);
				a.imm64(RCX, u64(s64(-0x8000)));
				a.cmp64(RAX, RCX);
				a.cmovl64(RAX, RCX);
				a.imm64(RCX, 0x7fff);
				a.cmp64(RAX, RCX);
				a.cmovg64(RAX, RCX);
			}
			a.store16(M(o_t_value + 2 * slot2(k)), RAX);
		}

		// ---- メモリ操作 ----
		if (o.memop) {
			a.loadu16(RAX, M(o_offset + 2 * s32(o.offset_index)));
			if (o.mem_use_index) {
				a.load32(RCX, M(o_ram_index));
				a.add32(RAX, RCX);
			}
			a.sub32(RAX, SC);
			if (o.memop == 3)
				a.add32i(RAX, 1);
			a.and32i(RAX, o.addr_mask);
			a.add32i(RAX, o.addr_base);
			a.and32i(RAX, 0x3ffff);
			if (o.memop == 1) {
				// meg_state::revram_encode を機械語で（関数は呼ばない）。番地は r8 に取っておく
				a.mov64(R8, RAX);
				a.load32(RAX, M(o_ram_write));
				emit_revram_encode(a);
				a.store16(mem{RAM, R8, 2, 0}, RAX);
			} else {
				// meg_state::revram_decode を機械語で（関数は呼ばない）
				a.loadu16(RAX, mem{RAM, RAX, 2, 0});
				emit_revram_decode(a);
				a.store32(M(o_memr_val + 4 * slot2(k)), RAX);
			}
		}
		if (k >= 0x17e)
			a.store8i(M(o_memr_act + slot2(k)), (o.memop == 2 || o.memop == 3) ? 1 : 0);
	}

	// 出口
	a.store64(M(o_p), P);
	a.addrsp(48);
	a.pop(R15); a.pop(R14); a.pop(R13); a.pop(R12); a.pop(RBX);
	a.ret();

	if (a.code.size() > buf_size) {
		if (buf)
			exec_mem::free_mem(buf, buf_size);
		buf_size = (a.code.size() + 0xffff) & ~size_t(0xffff);
		buf = exec_mem::alloc_rw(buf_size);
		if (!buf) {
			buf_size = 0;
			return false;
		}
	}
	// make writable, copy, make executable again. Needed on rebuilds, which arrive still executable
	if (!exec_mem::make_writable(buf, buf_size))
		return false;
	std::memcpy(buf, a.code.data(), a.code.size());
	if (!exec_mem::make_executable(buf, buf_size))
		return false;
	fn = reinterpret_cast<fn_t>(buf);
	return true;
}

#else // __aarch64__

// The arm64 MEG JIT. Same shape as the x86-64 version above (which carries the
// comments explaining what each section computes); only the notes where the
// two differ are repeated here.
bool swp30_device::meg_jit::build(meg_state &ms, const meg_state::op *ops, swp30_device &swp)
{
	using namespace a64;

	fn = nullptr;
	for (u32 pc = 0; pc != 0x180; pc++)
		if (ops[pc].jump)
			return false;
	if (swp.m_reverb_ram.size() < 0x40000)
		return false;

	d3 = ms.m_delay_3;
	d2 = ms.m_delay_2;
	const auto slot3 = [&](u32 k) { return (d3 + k) % 3; };
	const auto slot2 = [&](u32 k) { return (d2 + k) % 2; };

	// element positions (inside meg_state and swp30_device)
	const auto off = [](const void *base, const void *field) { return s32(intptr_t(field) - intptr_t(base)); };
	const s32 o_m        = off(&ms, ms.m_m.data());
	const s32 o_r        = off(&ms, ms.m_r.data());
	const s32 o_t        = off(&ms, ms.m_t.data());
	const s32 o_p        = off(&ms, &ms.m_p);
	const s32 o_const    = off(&ms, ms.m_const.data());
	const s32 o_offset   = off(&ms, ms.m_offset.data());
	const s32 o_mw_value = off(&ms, ms.m_mw_value.data());
	const s32 o_mw_reg   = off(&ms, ms.m_mw_reg.data());
	const s32 o_rw_value = off(&ms, ms.m_rw_value.data());
	const s32 o_rw_reg   = off(&ms, ms.m_rw_reg.data());
	const s32 o_ix_value = off(&ms, ms.m_index_value.data());
	const s32 o_ix_act   = off(&ms, ms.m_index_active.data());
	const s32 o_memw_val = off(&ms, ms.m_memw_value.data());
	const s32 o_memr_val = off(&ms, ms.m_memr_value.data());
	const s32 o_t_value  = off(&ms, ms.m_t_value.data());
	const s32 o_memw_act = off(&ms, ms.m_memw_active.data());
	const s32 o_memr_act = off(&ms, ms.m_memr_active.data());
	const s32 o_ram_read = off(&ms, &ms.m_ram_read);
	const s32 o_ram_write = off(&ms, &ms.m_ram_write);
	const s32 o_ram_index = off(&ms, &ms.m_ram_index);
	const s32 o_sample   = off(&ms, &ms.m_sample_counter);
	const s32 o_seed     = off(&swp, &swp.m_rand_seed);
	const s32 o_flag_n   = off(&swp, &swp.m_meg_flag_n);
	const s32 o_flag_z   = off(&swp, &swp.m_meg_flag_z);

	if (sizeof(ms.m_mw_reg[0]) != 1 || sizeof(ms.m_index_active[0]) != 1 || sizeof(ms.m_memw_active[0]) != 1 ||
	    sizeof(swp.m_meg_flag_n) != 1 || sizeof(ms.m_t_value[0]) != 2 || sizeof(ms.m_const[0]) != 2 ||
	    sizeof(ms.m_offset[0]) != 2 || sizeof(ms.m_m[0]) != 4 || sizeof(ms.m_r[0]) != 4)
		return false;

	// instructions that must leave a t value in the ring (2 later one is read), plus the tail
	bool need_tval[0x180] = {};
	for (u32 k = 0; k != 0x180; k++) {
		if (k >= 0x17e)
			need_tval[k] = true;
		if (k + 2 < 0x180 && ops[k + 2].t_write && ops[k + 2].t_from_p)
			need_tval[k] = true;
	}

	emitter a;
	// Block state in callee-saved registers (rbx/r12-r15 in the x86 version).
	// A carries the value a program instruction computes, E holds an address
	// across the revram helpers, T takes a materialized struct address; A, C, D,
	// E and T are all caller-saved and hold nothing across a helper call.
	const u8 MS = X19, SWP = X20, P = X21, SC = X22, RAM = X23;
	const u8 A = HA, C = HC, D = HD, E = HE, T = X6;

	// [base + disp] with disp known at build time. Each size reaches further
	// than the plain imm12 (the field is scaled by the access size), so a disp
	// past that has its address materialized in T first.
	const auto ldb   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 4095)  a.ldrb(rt, base, d);  else { a.add_off(T, base, d); a.ldrb(rt, T, 0); } };
	const auto stb   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 4095)  a.strb(rt, base, d);  else { a.add_off(T, base, d); a.strb(rt, T, 0); } };
	const auto ldh   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 8190)  a.ldrh(rt, base, d);  else { a.add_off(T, base, d); a.ldrh(rt, T, 0); } };
	const auto ldh_s = [&](u32 rt, u32 base, s32 d) {
		if (d >= 0 && d <= 8190)
			a.ldrsh(rt, base, d);
		else {
			a.add_off(T, base, d);
			a.ldrsh(rt, T, 0);
		}
		// LDRSH Wt sign-extends to 32 bits; the MEG ALU consumes a signed
		// 64-bit m1 value, so extend the result through the top half of Xt.
		a.sxtw64(rt, rt);
	};
	const auto sth   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 8190)  a.strh(rt, base, d);  else { a.add_off(T, base, d); a.strh(rt, T, 0); } };
	const auto ldw   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 16380) a.ldr_w(rt, base, d); else { a.add_off(T, base, d); a.ldr_w(rt, T, 0); } };
	const auto ldw_s = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 16380) a.ldrsw(rt, base, d); else { a.add_off(T, base, d); a.ldrsw(rt, T, 0); } };
	const auto stw   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 16380) a.str_w(rt, base, d); else { a.add_off(T, base, d); a.str_w(rt, T, 0); } };
	const auto ldx   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 32760) a.ldr_x(rt, base, d); else { a.add_off(T, base, d); a.ldr_x(rt, T, 0); } };
	const auto stx   = [&](u32 rt, u32 base, s32 d) { if (d >= 0 && d <= 32760) a.str_x(rt, base, d); else { a.add_off(T, base, d); a.str_x(rt, T, 0); } };

	// entry: x0 = ms, x1 = swp, x2 = the reverb RAM. x30 is saved because the
	// LFO is fetched with BLR, and x19-x23 because they carry the block state.
	a.stp_x(X19, X20, X31, -16, true);
	a.stp_x(X21, X22, X31, -16, true);
	a.stp_x(X23, X30, X31, -16, true);
	a.mov_x(MS, X0);
	a.mov_x(SWP, X1);
	a.mov_x(RAM, X2);
	ldx(P, MS, o_p);
	ldw(SC, MS, o_sample);

	// pack p into 24 bits (meg_state::meg_pack24). Input A, output A
	const auto pack24 = [&]() {
		a.asr_imm_x(A, A, 15);
		a.mov_imm64(C, u64(s64(-0x800000)));
		a.cmp_x(A, C);
		a.csel_x(A, C, A, LT);
		a.mov_imm64(C, 0x7fffff);
		a.cmp_x(A, C);
		a.csel_x(A, C, A, GT);
	};
	// one step of swp30_device::rand. Output A
	const auto rnd = [&]() {
		ldw(A, SWP, o_seed);
		a.mov_imm32(T, 1664525);
		a.mul(A, A, T);
		a.mov_imm32(D, 1013904223);
		a.add_reg(A, A, D);
		stw(A, SWP, o_seed);
		a.ror_imm(A, A, 16);
	};
	// p plus noise, packed (dm 6, and dr's p). Output A
	const auto p_packed = [&](bool noise) {
		if (noise) {
			rnd();
			a.and_imm(A, A, 0x07e0);
			a.mov_x(D, A);
			a.mov_x(A, P);
			a.add_x(A, A, D);
		} else
			a.mov_x(A, P);
		pack24();
	};

	// DEBUG: stop the block early, to bisect against the interpreter
	u32 upto = 0x180;
	if (const char *e = std::getenv("SMU2000_MEG_JIT_UPTO"))
		upto = u32(std::atoi(e));
	for (u32 k = 0; k != upto; k++) {
		const meg_state::op &o = ops[k];

		// ---- delayed writes, applied ----
		if (k < 3) {
			const u32 s = slot3(k);
			// m: the slot's register number is the index, and it may be 0 (= none)
			ldb(A, MS, o_mw_reg + s);
			a.cmp_reg(A, WZR);
			const size_t j1 = a.b_cond(EQ);
			ldw(C, MS, o_mw_value + 4 * s);
			a.add_off(T, MS, o_m);
			a.str_w_sr(C, T, A);
			a.patch(j1);
			// r
			ldb(A, MS, o_rw_reg + s);
			a.cmp_reg(A, WZR);
			const size_t j2 = a.b_cond(EQ);
			ldw(C, MS, o_rw_value + 4 * s);
			a.add_off(T, MS, o_r);
			a.str_w_sr(C, T, A);
			a.patch(j2);
			// index
			ldb(A, MS, o_ix_act + s);
			a.cmp_reg(A, WZR);
			const size_t j3 = a.b_cond(EQ);
			ldw(C, MS, o_ix_value + 4 * s);
			stw(C, MS, o_ram_index);
			a.patch(j3);
		} else {
			const meg_state::op &w = ops[k - 3];
			const u32 s = slot3(k);
			if (w.dm) {
				ldw(C, MS, o_mw_value + 4 * s);
				stw(C, MS, o_m + 4 * w.dm);
			}
			if (w.dr) {
				ldw(C, MS, o_rw_value + 4 * s);
				stw(C, MS, o_r + 4 * w.dr);
			}
			if (w.index) {
				ldw(C, MS, o_ix_value + 4 * s);
				stw(C, MS, o_ram_index);
			}
		}
		if (k < 2) {
			const u32 s = slot2(k);
			ldb(A, MS, o_memw_act + s);
			a.cmp_reg(A, WZR);
			const size_t j1 = a.b_cond(EQ);
			ldw(C, MS, o_memw_val + 4 * s);
			stw(C, MS, o_ram_write);
			stb(WZR, MS, o_memw_act + s);
			a.patch(j1);
			ldb(A, MS, o_memr_act + s);
			a.cmp_reg(A, WZR);
			const size_t j2 = a.b_cond(EQ);
			ldw(C, MS, o_memr_val + 4 * s);
			stw(C, MS, o_ram_read);
			stb(WZR, MS, o_memr_act + s);
			a.patch(j2);
		} else {
			const meg_state::op &w = ops[k - 2];
			const u32 s = slot2(k);
			if (w.memw) {
				ldw(C, MS, o_memw_val + 4 * s);
				stw(C, MS, o_ram_write);
			}
			if (w.memop == 2 || w.memop == 3) {
				ldw(C, MS, o_memr_val + 4 * s);
				stw(C, MS, o_ram_read);
			}
		}

		// ---- ALU ----
		if (o.alu) {
			// the interpreter widens the s16 constant (or t) to s64, so the load
			// has to sign-extend all the way
			if (o.m1_from_t)
				ldh_s(A, MS, o_t + 2 * o.t);
			else
				ldh_s(A, MS, o_const + 2 * s32(k));
			if (o.m1_expand)
				emit_m1_expand(a);
			switch (o.mmode) {
			case 1:
				a.lsl_imm_x(A, A, 8 + 15);
				break;
			case 2:
				ldw_s(C, MS, o.m2_from_m ? o_m + 4 * o.sm : o_r + 4 * o.sr);
				a.mul_x(A, A, C);
				break;
			default:
				ldw_s(A, MS, o.m2_from_m ? o_m + 4 * o.sm : o_r + 4 * o.sr);
				a.lsl_imm_x(A, A, 15);
				break;
			}
			switch (o.asel) {
			case 0: a.mov_x(C, P); break;
			case 1: ldw_s(C, MS, o_r + 4 * o.sr); a.lsl_imm_x(C, C, 15); break;
			case 2: ldw_s(C, MS, o_m + 4 * o.sm); a.lsl_imm_x(C, C, 15); break;
			case 3: a.mov_x(C, P); a.asr_imm_x(C, C, 15); break;
			default: a.eor_reg(C, C, C); break;
			}
			switch (o.rop) {
			case 0: a.add_x(A, A, C); break;
			case 1: a.sub_x(A, A, C); break;
			case 2:
				// |a|: NEG does not set the flags here (unlike x86), so compare
				a.mov_x(D, C);
				a.neg_x(D, D);
				a.cmp_x(D, X31);
				a.csel_x(D, C, D, MI);
				a.add_x(A, A, D);
				break;
			default: a.and_x(A, A, C); break;
			}
			if (o.shift)
				a.lsl_imm_x(A, A, o.shift);
			a.lsl_imm_x(A, A, 22);
			a.asr_imm_x(A, A, 22);
			switch (o.clamp) {
			case 0: break;
			case 1:
				a.mov_imm64(C, u64(s64(-0x4000000000)));
				a.cmp_x(A, C);
				a.csel_x(A, C, A, LT);
				a.mov_imm64(C, 0x3fffffffff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
				break;
			case 2:
				a.eor_reg(C, C, C);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, LT);
				a.mov_imm64(C, 0x3fffffffff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
				break;
			default:
				a.mov_x(D, A);
				a.neg_x(D, D);
				a.cmp_x(D, X31);
				a.csel_x(D, A, D, MI);
				a.mov_x(A, D);
				a.mov_imm64(C, 0x3fffffffff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
				break;
			}
			a.mov_x(P, A);
			if (o.latch) {
				a.cmp_x(P, X31);
				a.cset(C, MI);
				stb(C, SWP, o_flag_n);
				a.cmp_x(P, X31);
				a.cset(C, EQ);
				stb(C, SWP, o_flag_z);
			}
		}

		// ---- dm ----
		if (o.dm) {
			switch (o.dm_src) {
			case 0: case 1: case 2: case 3:
				a.mov_x(X0, MS);
				a.mov_imm32(X1, o.lfo);
				a.mov_imm64(X17, u64(uintptr_t(&meg_jit::call_lfo)));
				a.blr(X17);
				a.mov_reg(A, W0);                        // the result comes back in w0
				break;
			case 4:
				ldw(A, MS, o_ram_read);
				break;
			case 5:
				rnd();
				a.lsl_imm(A, A, 8);
				a.sar_imm(A, A, 8);
				break;
			case 6:
				p_packed(!o.no_noise);
				break;
			default:
				ldw(A, MS, o_m + 4 * o.sm);
				break;
			}
			stw(A, MS, o_mw_value + 4 * slot3(k));
		}
		if (k >= 0x17d) {
			a.mov_imm32(C, o.dm);
			stb(C, MS, o_mw_reg + slot3(k));
		}

		// ---- dr ----
		if (o.dr) {
			if (o.dr_from_r)
				ldw(A, MS, o_r + 4 * o.sr);
			else
				p_packed(!o.no_noise);
			stw(A, MS, o_rw_value + 4 * slot3(k));
		}
		if (k >= 0x17d) {
			a.mov_imm32(C, o.dr);
			stb(C, MS, o_rw_reg + slot3(k));
		}

		// ---- value to write to memory ----
		if (o.memw) {
			a.mov_x(A, P);
			a.asr_imm_x(A, A, 15);
			stw(A, MS, o_memw_val + 4 * slot2(k));
		}
		if (k >= 0x17e) {
			a.mov_imm32(C, o.memw ? 1 : 0);
			stb(C, MS, o_memw_act + slot2(k));
		}

		// ---- index ----
		if (o.index) {
			a.mov_x(A, P);
			a.asr_imm_x(A, A, 15 + 8);
			stw(A, MS, o_ix_value + 4 * slot3(k));
		}
		if (k >= 0x17d) {
			a.mov_imm32(C, o.index ? 1 : 0);
			stb(C, MS, o_ix_act + slot3(k));
		}

		// ---- t ----
		if (o.t_write) {
			if (o.t_from_p)
				ldh(A, MS, o_t_value + 2 * slot2(k));
			else
				ldh(A, MS, o_const + 2 * s32(k));
			sth(A, MS, o_t + 2 * o.t);
		}
		if (need_tval[k]) {
			a.mov_x(A, P);
			if (o.index) {
				a.asr_imm_x(A, A, 8);
				a.and_imm(A, A, 0x7fff);
			} else {
				a.asr_imm_x(A, A, 15 + 8);
				a.mov_imm64(C, u64(s64(-0x8000)));
				a.cmp_x(A, C);
				a.csel_x(A, C, A, LT);
				a.mov_imm64(C, 0x7fff);
				a.cmp_x(A, C);
				a.csel_x(A, C, A, GT);
			}
			sth(A, MS, o_t_value + 2 * slot2(k));
		}

		// ---- memory operation ----
		if (o.memop) {
			ldh(A, MS, o_offset + 2 * s32(o.offset_index));
			if (o.mem_use_index) {
				ldw(C, MS, o_ram_index);
				a.add_reg(A, A, C);
			}
			a.sub_reg(A, A, SC);
			if (o.memop == 3)
				a.add_imm(A, A, 1);
			a.and_imm_any(A, A, o.addr_mask);
			a.add_imm_any(A, A, o.addr_base);
			a.and_imm(A, A, 0x3ffff);
			if (o.memop == 1) {
				// meg_state::revram_encode written out in machine code, with the
				// address parked in E (the helper clobbers C and D)
				a.mov_x(E, A);
				ldw(A, MS, o_ram_write);
				emit_revram_encode(a);
				a.strh_sr(A, RAM, E);
			} else {
				// meg_state::revram_decode written out in machine code
				a.ldrh_sr(A, RAM, A);
				emit_revram_decode(a);
				stw(A, MS, o_memr_val + 4 * slot2(k));
			}
		}
		if (k >= 0x17e) {
			a.mov_imm32(C, (o.memop == 2 || o.memop == 3) ? 1 : 0);
			stb(C, MS, o_memr_act + slot2(k));
		}
	}

	// exit
	stx(P, MS, o_p);
	a.ldp_x(X23, X30, X31, 16, true);
	a.ldp_x(X21, X22, X31, 16, true);
	a.ldp_x(X19, X20, X31, 16, true);
	a.ret();

	// a.code is a vector of 32-bit instructions, so every size here has to
	// count four bytes each (the x86 version's vector is bytes)
	const size_t bytes = a.code.size() * sizeof(a.code[0]);
	if (bytes > buf_size) {
		if (buf)
			exec_mem::free_mem(buf, buf_size);
		buf_size = (bytes + 0xffff) & ~size_t(0xffff);
		buf = exec_mem::alloc_rw(buf_size);
		if (!buf) {
			buf_size = 0;
			return false;
		}
	}
	// make writable, copy, make executable again. Needed on rebuilds, which arrive still executable
	if (!exec_mem::make_writable(buf, buf_size))
		return false;
	std::memcpy(buf, a.code.data(), bytes);
	if (!exec_mem::make_executable(buf, buf_size))
		return false;
	fn = reinterpret_cast<fn_t>(buf);
	return true;
}

#endif
