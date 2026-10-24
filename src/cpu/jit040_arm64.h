/*
  PreviousTurbo - jit040_arm64.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The AArch64 code of jit040_native.c (Linux and macOS).

  x19 holds &regs, x21 &regflags, x22 the page cache, x23 &jit040_stop and
  x24 this file's data; w20 counts the cycles. The virtual registers are w10
  (V0), w11 (V1), w12 (VA, addresses), w13 (VJ, jump targets) and w14 (VI,
  indexes); x9, x15 (host addresses), x16 and x17 are the code's own.

  regflags.cznv here has the generic layout of machdep/m68k.h (N bit 7, Z 6,
  C 0, V 11). The host's flags are turned into it with a table, by NZCV;
  byte and word operations are done in the top bits of a register, so the
  host's flags are those of the 68k operation.
*/

#ifdef __APPLE__
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

#define HOST_F_C 0x0001
#define HOST_F_Z 0x0040
#define HOST_F_N 0x0080
#define HOST_F_V 0x0800
#define HOST_CZNV (HOST_F_C | HOST_F_Z | HOST_F_N | HOST_F_V)

#define CODE_START 0

enum { XB = 19, XC = 20, XF = 21, XT = 22, XS = 23, XD = 24 };
enum { W9 = 9, W15 = 15, W16 = 16, W17 = 17, WZR = 31 };
static const int vreg[] = { 10, 11, 12, 13, 14 };   /* V0, V1, VA, VJ, VI */

enum { A_EQ = 0, A_NE, A_HS, A_LO, A_MI, A_PL, A_VS, A_VC, A_HI, A_LS, A_GE, A_LT, A_GT, A_LE };

/* What the code reaches through x24 */
static struct {
	uae_u16 flags[32];          /* NZCV >> 28 to cznv: additions, subtractions (C inverted) */
	void *regs, *regflags, *tlb, *stop;
	uae_u8 *mmu_restart;
	uae_u16 *mmu_opcode;
	uae_u8 **link_site;
	struct flag_struct *f;
} arm;

#define D_REGS     offsetof(__typeof__(arm), regs)
#define D_REGFLAGS offsetof(__typeof__(arm), regflags)
#define D_TLB      offsetof(__typeof__(arm), tlb)
#define D_STOP     offsetof(__typeof__(arm), stop)
#define D_RESTART  offsetof(__typeof__(arm), mmu_restart)
#define D_OPCODE   offsetof(__typeof__(arm), mmu_opcode)
#define D_LINK     offsetof(__typeof__(arm), link_site)
#define D_F        offsetof(__typeof__(arm), f)

/* Offsets of the fields, from regs and regflags */
static uint32_t o_regs, o_pc, o_instruction_pc, o_opcode, o_spcflags, o_s;
static uint32_t o_cznv, o_x, o_f_cznv, o_f_x;

static bool be_init(bool *mem_ok)
{
	int i;

	if (sizeof(regflags.cznv) != 4 || sizeof(regflags.x) != 4 || sizeof(mmu_restart) != 1 ||
	    sizeof(regs.s) != 1 || sizeof(regs.opcode) != 2 || sizeof(mmu_opcode) != 2)
		return false;
	o_regs = offsetof(struct regstruct, regs);
	o_pc = offsetof(struct regstruct, pc);
	o_instruction_pc = offsetof(struct regstruct, instruction_pc);
	o_opcode = offsetof(struct regstruct, opcode);
	o_spcflags = offsetof(struct regstruct, spcflags);
	o_s = offsetof(struct regstruct, s);
	o_cznv = offsetof(struct flag_struct, cznv);
	o_x = offsetof(struct flag_struct, x);
	o_f_cznv = o_cznv;
	o_f_x = o_x;
	/* reached with scaled 12 bit offsets */
	if ((o_regs | o_pc | o_instruction_pc | o_spcflags | o_cznv | o_x) & 3 || (o_opcode & 1) ||
	    o_regs + 64 > 16380 || o_pc > 16380 || o_instruction_pc > 16380 || o_spcflags > 16380 ||
	    o_opcode > 8190 || o_s > 4095)
		return false;
	for (i = 0; i < 16; i++) {
		int n = i >> 3 & 1, z = i >> 2 & 1, c = i >> 1 & 1, v = i & 1;
		uae_u16 nzv = (n ? HOST_F_N : 0) | (z ? HOST_F_Z : 0) | (v ? HOST_F_V : 0);
		arm.flags[i] = nzv | (c ? HOST_F_C : 0);
		arm.flags[16 + i] = nzv | (c ? 0 : HOST_F_C);
	}
	arm.regs = &regs;
	arm.regflags = &regflags;
	arm.tlb = &pt_tlb[0][0];
	arm.stop = &jit040_stop;
	arm.mmu_restart = (uae_u8 *)&mmu_restart;
	arm.mmu_opcode = &mmu_opcode;
	arm.link_site = &jit040_link_site;
	arm.f = &jit040_f;
	*mem_ok = sizeof(struct pt_tlb_entry) == 32 &&
		offsetof(struct pt_tlb_entry, jit_tag) == 20 && offsetof(struct pt_tlb_entry, jit_host) == 24;
	return true;
}

static bool be_register_code(uae_u8 *buf, size_t size)
{
	return true;
}

/* On macOS the code memory is either writable or executable for a thread */
static void be_write_begin(void)
{
#ifdef __APPLE__
	pthread_jit_write_protect_np(0);
#endif
}
static void be_write_end(uae_u8 *start, uae_u8 *end)
{
#ifdef __APPLE__
	pthread_jit_write_protect_np(1);
	if (end > start)
		sys_icache_invalidate(start, end - start);
#else
	if (end > start)
		__builtin___clear_cache((char *)start, (char *)end);
#endif
}

/* Encoding */

static void ins(uae_u32 x)
{
	memcpy(cp, &x, 4);
	cp += 4;
}

/* the N:immr:imms of a 32 bit logical immediate, or -1 */
static int logimm32(uae_u32 v)
{
	int e, rot;
	uae_u32 mask, elem;

	if (v == 0 || v == 0xffffffff)
		return -1;
	for (e = 2; e <= 32; e *= 2) {
		uae_u32 rep = 0;
		int i;
		mask = e == 32 ? 0xffffffff : (1u << e) - 1;
		elem = v & mask;
		for (i = 0; i < 32; i += e)
			rep |= elem << i;
		if (rep == v)
			break;
	}
	for (rot = 0; rot < e; rot++) {
		uae_u32 r = rot ? ((elem >> rot) | (elem << (e - rot))) & mask : elem;
		if (r && (r & (r + 1)) == 0) {
			int ones = __builtin_popcount(r);
			int imms = (~(2 * e - 1) & 0x3f) | (ones - 1);
			int immr = (e - rot) % e;
			return (immr << 6) | imms;
		}
	}
	return -1;
}

/* data processing */
static void movz(int rd, int v, int hw) { ins(0x52800000 | (hw << 21) | ((v & 0xffff) << 5) | rd); }
static void movk(int rd, int v, int hw) { ins(0x72800000 | (hw << 21) | ((v & 0xffff) << 5) | rd); }
static void movn(int rd, int v, int hw) { ins(0x12800000 | (hw << 21) | ((v & 0xffff) << 5) | rd); }
static void movw(int rd, uae_u32 v)
{
	if ((v & 0xffff0000) == 0)
		movz(rd, v, 0);
	else if ((v & 0xffff) == 0)
		movz(rd, v >> 16, 1);
	else if ((v | 0xffff) == 0xffffffff)
		movn(rd, ~v & 0xffff, 0);
	else {
		movz(rd, v & 0xffff, 0);
		movk(rd, v >> 16, 1);
	}
}
static void movx(int rd, uae_u64 v)
{
	int hw;
	ins(0xd2800000 | ((v & 0xffff) << 5) | rd);
	for (hw = 1; hw < 4; hw++) {
		if ((v >> (16 * hw)) & 0xffff)
			ins(0xf2800000 | (hw << 21) | (((v >> (16 * hw)) & 0xffff) << 5) | rd);
	}
}
static void mov_rr(int rd, int rm) { ins(0x2a0003e0 | (rm << 16) | rd); }

/* shifted register forms: shift 0 LSL, 1 LSR, 2 ASR */
#define SR(base, rd, rn, rm, shift, amount) ins((base) | ((shift) << 22) | ((rm) << 16) | ((amount) << 10) | ((rn) << 5) | (rd))
#define I_ADD  0x0b000000
#define I_ADDS 0x2b000000
#define I_SUB  0x4b000000
#define I_SUBS 0x6b000000
#define I_AND  0x0a000000
#define I_ANDS 0x6a000000
#define I_ORR  0x2a000000
#define I_EOR  0x4a000000

static void addi(int rd, int rn, int v)   /* 0 <= v < 4096 */
{
	ins(0x11000000 | (v << 10) | (rn << 5) | rd);
}
static void subi(int rd, int rn, int v)
{
	ins(0x51000000 | (v << 10) | (rn << 5) | rd);
}
static void addi_any(int rd, int rn, uae_s32 v)
{
	if (v >= 0 && v < 4096)
		addi(rd, rn, v);
	else if (v < 0 && v > -4096)
		subi(rd, rn, -v);
	else {
		movw(W16, (uae_u32)v);
		SR(I_ADD, rd, rn, W16, 0, 0);
	}
}
static void cmpi(int rn, int v) { ins(0x7100001f | (v << 10) | (rn << 5)); }
static void cmp_rr(int rn, int rm) { SR(I_SUBS, WZR, rn, rm, 0, 0); }
static void tst_rr(int rn, int rm) { SR(I_ANDS, WZR, rn, rm, 0, 0); }
/* logical immediates, with a register for those that cannot be encoded */
static void logi(uae_u32 base_imm, uae_u32 base_reg, int rd, int rn, uae_u32 v)
{
	int e = logimm32(v);
	if (e >= 0) {
		ins(base_imm | (e << 10) | (rn << 5) | rd);
	} else {
		movw(W17, v);
		ins(base_reg | (W17 << 16) | (rn << 5) | rd);
	}
}
static void andi(int rd, int rn, uae_u32 v) { logi(0x12000000, I_AND, rd, rn, v); }
static void orri(int rd, int rn, uae_u32 v) { logi(0x32000000, I_ORR, rd, rn, v); }
static void eori(int rd, int rn, uae_u32 v) { logi(0x52000000, I_EOR, rd, rn, v); }
static void tsti(int rn, uae_u32 v) { logi(0x72000000, I_ANDS, WZR, rn, v); }
/* bitfields */
static void ubfm(int rd, int rn, int immr, int imms) { ins(0x53000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void sbfm(int rd, int rn, int immr, int imms) { ins(0x13000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void lsli(int rd, int rn, int s) { ubfm(rd, rn, (32 - s) & 31, 31 - s); }
static void lsri(int rd, int rn, int s) { ubfm(rd, rn, s, 31); }
static void asri(int rd, int rn, int s) { sbfm(rd, rn, s, 31); }
static void ubfx(int rd, int rn, int lsb, int width) { ubfm(rd, rn, lsb, lsb + width - 1); }
static void sxt(int sz, int rd, int rn) { sbfm(rd, rn, 0, sz * 8 - 1); }
static void rori(int rd, int rn, int s) { ins(0x13800000 | (rn << 16) | (s << 10) | (rn << 5) | rd); }
static void cset(int rd, int cond) { ins(0x1a9f07e0 | ((cond ^ 1) << 12) | rd); }
static void mrs_nzcv(int rt) { ins(0xd53b4200 | rt); }

/* loads and stores: [xn + off], off a multiple of the size */
static void ldr_sz(int sz, int rt, int rn, uae_u32 off)
{
	if (sz == 1)
		ins(0x39400000 | (off << 10) | (rn << 5) | rt);
	else if (sz == 2)
		ins(0x79400000 | ((off >> 1) << 10) | (rn << 5) | rt);
	else
		ins(0xb9400000 | ((off >> 2) << 10) | (rn << 5) | rt);
}
static void ldrs_sz(int sz, int rt, int rn, uae_u32 off)
{
	if (sz == 1)
		ins(0x39c00000 | (off << 10) | (rn << 5) | rt);
	else
		ins(0x79c00000 | ((off >> 1) << 10) | (rn << 5) | rt);
}
static void str_sz(int sz, int rt, int rn, uae_u32 off)
{
	if (sz == 1)
		ins(0x39000000 | (off << 10) | (rn << 5) | rt);
	else if (sz == 2)
		ins(0x79000000 | ((off >> 1) << 10) | (rn << 5) | rt);
	else
		ins(0xb9000000 | ((off >> 2) << 10) | (rn << 5) | rt);
}
static void ldrx(int rt, int rn, uae_u32 off) { ins(0xf9400000 | ((off >> 3) << 10) | (rn << 5) | rt); }
static void strx(int rt, int rn, uae_u32 off) { ins(0xf9000000 | ((off >> 3) << 10) | (rn << 5) | rt); }

/* the base and offset of a field */
static void field(int f, int *base, uae_u32 *off)
{
	if (f < 16) {
		*base = XB;
		*off = o_regs + 4 * f;
	} else if (f == F_PC) {
		*base = XB;
		*off = o_pc;
	} else {
		*base = XF;
		*off = f == F_CZNV ? o_cznv : o_x;
	}
}
static void ldf(int sz, int rt, int f) { int b; uae_u32 o; field(f, &b, &o); ldr_sz(sz, rt, b, o); }
static void ldfs(int sz, int rt, int f) { int b; uae_u32 o; field(f, &b, &o); if (sz == 4) ldr_sz(4, rt, b, o); else ldrs_sz(sz, rt, b, o); }
static void stf(int sz, int rt, int f) { int b; uae_u32 o; field(f, &b, &o); str_sz(sz, rt, b, o); }

/* branches */
static uae_u8 *b_cond(int cond) { ins(0x54000000 | cond); return cp - 4; }
static uae_u8 *b_(void) { ins(0x14000000); return cp - 4; }
static uae_u8 *cbz(int rt) { ins(0x34000000 | rt); return cp - 4; }
static uae_u8 *cbnz(int rt) { ins(0x35000000 | rt); return cp - 4; }
static uae_u8 *tbnz(int rt, int bit) { ins(0x37000000 | (bit << 19) | rt); return cp - 4; }
/* code that may be skipped, while nothing jumps to where cp is: the last
 * cycle count added (merged with the next one), regflags still in w9 */
static uae_u8 *cyc_start, *cyc_end, *cznv_in_w9;
static int cyc_n;

/* something jumps to cp */
static void be_target(void) { cyc_end = cznv_in_w9 = rc_at = NULL; }

static void patch(uae_u8 *at, uae_u8 *to)
{
	uae_u32 i;
	int32_t d = (int32_t)(to - at) >> 2;

	memcpy(&i, at, 4);
	if ((i & 0xfc000000) == 0x14000000)
		i = (i & 0xfc000000) | (d & 0x3ffffff);
	else if ((i & 0x7e000000) == 0x36000000)
		i = (i & ~(0x3fffu << 5)) | ((d & 0x3fff) << 5);
	else
		i = (i & ~(0x7ffffu << 5)) | ((d & 0x7ffff) << 5);
	memcpy(at, &i, 4);
	if (to == cp)
		be_target();
}

/* Flags: the host's NZCV into regflags (C inverted after a subtraction),
 * into w9; stored, and into X too if asked */
static void nzcv_to_w9(int sub)
{
	mrs_nzcv(W9);
	lsri(W9, W9, 28);
	if (sub)
		orri(W9, W9, 16);
	ins(0x78607800 | (W9 << 16) | (XD << 5) | W9);   /* ldrh w9, [x24, x9, lsl #1] */
}
static void capture(int sub, int x)
{
	nzcv_to_w9(sub);
	str_sz(4, W9, XF, o_cznv);
	if (x)
		str_sz(4, W9, XF, o_x);
	cznv_in_w9 = cp;
}
/* N and Z of the low sz bytes of r, V and C cleared, into the host's flags */
static void test_sz(int sz, int r)
{
	if (sz == 4) {
		tst_rr(r, r);
	} else {
		lsli(W16, r, 32 - 8 * sz);
		tst_rr(W16, W16);
	}
}

/* The operations */

static void be_prologue(struct jit040_block *b)
{
	ins(0xa9ba7bfd);                          /* stp x29, x30, [sp, #-96]! */
	ins(0x910003fd);                          /* mov x29, sp */
	ins(0xa90153f3);                          /* stp x19, x20, [sp, #16] */
	ins(0xa9025bf5);                          /* stp x21, x22, [sp, #32] */
	ins(0xa90363f7);                          /* stp x23, x24, [sp, #48] */
	movx(XD, (uintptr_t)&arm);
	ldrx(XB, XD, D_REGS);
	ldrx(XF, XD, D_REGFLAGS);
	ldrx(XT, XD, D_TLB);
	ldrx(XS, XD, D_STOP);
	movz(XC, 0, 0);
	b->chain = cp;                            /* where other blocks go on into it */
}
static void be_epilogue(void)
{
	mov_rr(0, XC);
	ins(0xa94363f7);                          /* ldp x23, x24, [sp, #48] */
	ins(0xa9425bf5);                          /* ldp x21, x22, [sp, #32] */
	ins(0xa94153f3);                          /* ldp x19, x20, [sp, #16] */
	ins(0xa8c67bfd);                          /* ldp x29, x30, [sp], #96 */
	ins(0xd65f03c0);                          /* ret */
}
static void be_add_cycles(int n)
{
	bool keep = cp == cznv_in_w9;

	rc_begin();
	if (cp == cyc_end) {
		cp = cyc_start;
		n += cyc_n;
	}
	cyc_start = cp;
	addi_any(XC, XC, n);
	cyc_end = cp;
	cyc_n = n;
	if (keep)
		cznv_in_w9 = cp;
	rc_end();
}

/* (the operations up to be_set_nz keep track of what the virtual registers
 * hold, see rc_f) */
static void be_ld(int sz, int v, int f)
{
	rc_begin();
	if (sz == 4 && f < 16) {
		int u = rc_find(f);
		if (u != v) {
			if (u >= 0)
				mov_rr(vreg[v], vreg[u]);
			else
				ldf(4, vreg[v], f);
			rc_f[v] = f;
		}
	} else {
		ldf(sz, vreg[v], f);
		rc_f[v] = -1;
	}
	rc_end();
}
static void be_ldsx(int sz, int v, int f)
{
	if (sz == 4) {
		be_ld(4, v, f);
		return;
	}
	rc_begin();
	ldfs(sz, vreg[v], f);
	rc_f[v] = -1;
	rc_end();
}
static void be_st(int sz, int f, int v)
{
	rc_begin();
	stf(sz, vreg[v], f);
	rc_kill(f);
	if (sz == 4 && f < 16)
		rc_f[v] = f;
	rc_end();
}
static void be_sti(int sz, int f, uae_u32 v)
{
	rc_begin();
	if (v == 0) {
		stf(sz, WZR, f);
	} else {
		movw(W9, v);
		stf(sz, W9, f);
	}
	rc_kill(f);
	rc_end();
}
/* v is written */
static void rc_set(int v)
{
	rc_f[v] = -1;
	rc_end();
}
static void be_movi(int v, uae_u32 x) { rc_begin(); movw(vreg[v], x); rc_set(v); }
static void be_mov(int v, int s) { rc_begin(); mov_rr(vreg[v], vreg[s]); rc_f[v] = rc_f[s]; rc_end(); }
static void be_sext(int sz, int v) { rc_begin(); sxt(sz, vreg[v], vreg[v]); rc_set(v); }
static void be_add(int v, int s) { rc_begin(); SR(I_ADD, vreg[v], vreg[v], vreg[s], 0, 0); rc_set(v); }
static void be_addi(int v, uae_s32 x) { rc_begin(); if (x) addi_any(vreg[v], vreg[v], x); rc_set(v); }
static void be_shli(int v, int n) { rc_begin(); lsli(vreg[v], vreg[v], n); rc_set(v); }
static void be_field_addi(int f, uae_s32 x)
{
	rc_begin();
	ldf(4, W9, f);
	addi_any(W9, W9, x);
	stf(4, W9, f);
	rc_kill(f);
	rc_end();
}

/* rd op= rs on sz bytes, with the 68k's flags as fl asks */
static void alu(int sz, int op, int rd, int rs, int fl)
{
	int sh = 32 - 8 * sz;

	fl = fl_needed(fl);
	if (fl == FL_NONE && op == OP_CMP)
		return;   /* nothing it sets is needed */
	if (fl == FL_X) {
		/* only X (= C, inverted after a subtraction) */
		uae_u32 base = op == OP_ADD ? I_ADDS : I_SUBS;
		if (sz == 4) {
			SR(base, rd, rd, rs, 0, 0);
		} else {
			lsli(W16, rd, sh);
			SR(base, W16, W16, rs, 0, sh);
			lsri(rd, W16, sh);
		}
		cset(W9, op == OP_ADD ? A_HS : A_LO);
		str_sz(4, W9, XF, o_x);
		return;
	}
	if (fl == FL_NONE) {
		static const uae_u32 base[] = { I_ADD, I_SUB, I_AND, I_ORR, I_EOR };
		SR(base[op], rd, rd, rs, 0, 0);
		return;
	}
	switch (op) {
	case OP_ADD: case OP_SUB: case OP_CMP: {
		uae_u32 base = op == OP_ADD ? I_ADDS : I_SUBS;
		if (sz == 4) {
			SR(base, op == OP_CMP ? WZR : rd, rd, rs, 0, 0);
		} else {
			lsli(W16, rd, sh);
			SR(base, W16, W16, rs, 0, sh);
			if (op != OP_CMP)
				lsri(rd, W16, sh);
		}
		capture(op != OP_ADD, fl == FL_ARITH_X);
		break;
	}
	default: {
		static const uae_u32 base[] = { 0, 0, I_AND, I_ORR, I_EOR };
		SR(base[op], rd, rd, rs, 0, 0);
		test_sz(sz, rd);
		capture(0, 0);
		break;
	}
	}
}

static void be_alu(int sz, int op, int v, int s, int fl) { rc_begin(); alu(sz, op, vreg[v], vreg[s], fl); rc_set(v); }
static void be_alui(int sz, int op, int v, uae_u32 x, int fl)
{
	rc_begin();
	movw(W17, x);
	alu(sz, op, vreg[v], W17, fl);
	rc_set(v);
}
static void be_alu_field(int sz, int op, int v, int f, int fl)
{
	rc_begin();
	ldf(sz, W17, f);
	alu(sz, op, vreg[v], W17, fl);
	rc_set(v);
}
static void be_test(int sz, int v)
{
	rc_begin();
	if (need & F_NZVC) {
		test_sz(sz, vreg[v]);
		capture(0, 0);
	}
	rc_end();
}
static void be_set_nz(int n, int z)
{
	if (need & F_NZVC)
		be_sti(4, F_CZNV, (n ? HOST_F_N : 0) | (z ? HOST_F_Z : 0));
}
static void be_neg(int sz, int v)
{
	int r = vreg[v], sh = 32 - 8 * sz;
	if (sz == 4) {
		SR(I_SUBS, r, WZR, r, 0, 0);
	} else {
		lsli(W16, r, sh);
		SR(I_SUBS, W16, WZR, W16, 0, 0);
		lsri(r, W16, sh);
	}
	capture(1, 1);
}
static void be_not(int sz, int v)
{
	int r = vreg[v];
	ins(0x2a2003e0 | (r << 16) | r);          /* mvn */
	be_test(sz, v);
}
/* v = v * s on their low words, signed or not, 32 bit result: N, Z, V and C
 * cleared (MULU.W, MULS.W) */
static void be_mul16(int is_signed, int v, int s)
{
	int r = vreg[v], b = vreg[s];
	if (is_signed) {
		sxt(2, r, r);
		sxt(2, W17, b);
	} else {
		ubfx(r, r, 0, 16);
		ubfx(W17, b, 0, 16);
	}
	ins(0x1b007c00 | (W17 << 16) | (r << 5) | r);   /* mul r, r, w17 */
	be_test(4, v);
}
static void be_swap(int v)
{
	rori(vreg[v], vreg[v], 16);
	be_test(4, v);
}

/* Shifts and rotates by n (1-8, less than the size in bits) with the 68k's
 * flags: kind 0 AS, 1 LS, 3 RO; the result is stored too */
static void be_shift(int sz, int kind, int left, int v, int n, int f)
{
	int r = vreg[v], nbits = sz * 8;

	if (kind == 3) {
		/* ROL, ROR: C is the last bit rotated, X is kept */
		if (left) {
			lsli(W16, r, n);
			lsri(W17, r, nbits - n);
			SR(I_ORR, r, W16, W17, 0, 0);
			andi(W17, r, 1);
		} else {
			lsri(W16, r, n);
			lsli(W17, r, nbits - n);
			SR(I_ORR, r, W16, W17, 0, 0);
			ubfx(W17, r, nbits - 1, 1);
		}
		stf(sz, r, f);
		test_sz(sz, r);
		nzcv_to_w9(0);
		SR(I_ORR, W9, W9, W17, 0, 0);
		str_sz(4, W9, XF, o_cznv);
		return;
	}
	if (!left) {
		if (kind == 0 && sz != 4)
			sxt(sz, r, r);
		ubfx(W17, r, n - 1, 1);                /* C */
		if (kind == 0)
			asri(r, r, n);
		else
			lsri(r, r, n);
	} else {
		ubfx(W17, r, nbits - n, 1);            /* C */
		if (kind == 0) {
			/* ASL: V if the bits shifted through the sign bit differ */
			if (sz == 4)
				mov_rr(W15, r);
			else
				sxt(sz, W15, r);
			asri(W15, W15, nbits - 1 - n);
			addi(W15, W15, 1);
			cmpi(W15, 1);
			cset(W15, A_HI);
		}
		lsli(r, r, n);
	}
	stf(sz, r, f);
	test_sz(sz, r);
	nzcv_to_w9(0);
	SR(I_ORR, W9, W9, W17, 0, 0);
	if (left && kind == 0)
		SR(I_ORR, W9, W9, W15, 0, 11);
	str_sz(4, W9, XF, o_cznv);
	str_sz(4, W9, XF, o_x);
}

/* 64-bit shifts and extensions */
static void lsl64i(int rd, int rn, int s) { ins(0xd3400000 | (((64 - s) & 63) << 16) | ((63 - s) << 10) | (rn << 5) | rd); }
static void lsr64i(int rd, int rn, int s) { ins(0xd3400000 | (s << 16) | (63 << 10) | (rn << 5) | rd); }
static void shiftv64(int kind, int rd, int rn, int rm)   /* 0 lsl, 1 lsr, 2 asr */
{
	ins(0x9ac02000 | (rm << 16) | (kind << 10) | (rn << 5) | rd);
}
static void sxt64(int sz, int rd, int rn) { ins(0x93400000 | ((sz * 8 - 1) << 10) | (rn << 5) | rd); }

/* LSL, LSR, ASL, ASR of Dy by the count in Dx (mod 64), as 64-bit shifts,
 * with the value in the high half for those to the right: the result and
 * the last bit shifted out (C and X, none past the size) come out for every
 * count. ASL's V: the value shifted by at most the size no longer fits. A
 * count of 0 only sets N and Z, C and V cleared, X kept. */
static void be_shift_reg(int sz, int kind, int left, int fc, int fv)
{
	int n = sz * 8, c = vreg[VA], r = vreg[V0];
	uae_u8 *shift, *done;

	ldf(4, c, fc);
	andi(c, c, 63);
	if (kind == 0 && !left)
		ldfs(sz, r, fv);
	else
		ldf(sz, r, fv);
	shift = cbnz(c);
	test_sz(sz, r);
	nzcv_to_w9(0);
	str_sz(4, W9, XF, o_cznv);
	done = b_();
	patch(shift, cp);
	if (left) {
		if (kind == 0) {
			/* w15 = V: the value, sign extended, << min(count, size)
			 * differs from its low size bits sign extended */
			sxt64(sz, W17, r);
			movw(W9, n);
			cmp_rr(c, W9);
			ins(0x1a800000 | (W9 << 16) | (A_LS << 12) | (c << 5) | W9);   /* csel w9, c, w9, ls */
			shiftv64(0, W17, W17, W9);
			sxt64(sz, W15, W17);
			ins(0xeb00001f | (W17 << 16) | (W15 << 5));   /* cmp x15, x17 */
			cset(W15, A_NE);
		}
		shiftv64(0, r, r, c);
		lsr64i(W17, r, n);
	} else {
		lsl64i(r, r, 32);
		shiftv64(kind == 0 ? 2 : 1, r, r, c);
		lsr64i(W17, r, 31);
		lsr64i(r, r, 32);
	}
	andi(W17, W17, 1);                        /* C (test_sz uses w16) */
	stf(sz, r, fv);
	test_sz(sz, r);
	nzcv_to_w9(0);
	SR(I_ORR, W9, W9, W17, 0, 0);
	if (left && kind == 0)
		SR(I_ORR, W9, W9, W15, 0, 11);
	str_sz(4, W9, XF, o_cznv);
	str_sz(4, W9, XF, o_x);
	patch(done, cp);
}

/* ROXL, ROXR of Dn by n (1-8): X above the value, rotated as size + 1 bits;
 * C and X are then the bit above the result, V is cleared */
static void orr64(int rd, int rn, int rm) { ins(0xaa000000 | (rm << 16) | (rn << 5) | rd); }
static void be_rox(int sz, int left, int n, int f)
{
	int nbits = sz * 8, r = vreg[V0];

	ldf(sz, r, f);
	ldr_sz(4, W9, XF, o_x);
	andi(W9, W9, 1);
	lsl64i(W9, W9, nbits);
	orr64(r, r, W9);
	if (left) {
		lsl64i(W16, r, n);
		lsr64i(W17, r, nbits + 1 - n);
	} else {
		lsr64i(W16, r, n);
		lsl64i(W17, r, nbits + 1 - n);
	}
	orr64(r, W16, W17);
	lsr64i(W17, r, nbits);
	andi(W17, W17, 1);                        /* C (test_sz uses w16) */
	stf(sz, r, f);
	test_sz(sz, r);
	nzcv_to_w9(0);
	SR(I_ORR, W9, W9, W17, 0, 0);
	str_sz(4, W9, XF, o_cznv);
	str_sz(4, W9, XF, o_x);
}

/* V0 * V1, 32 x 32 bits as one 64-bit product: into fl (V if it does not
 * fit in 32 bits, signed or not) or fh:fl (V cleared); C cleared */
static void be_mull(int is_signed, int wide, int fl, int fh)
{
	int r = vreg[V0], s = vreg[V1];

	ins((is_signed ? 0x9b207c00 : 0x9ba07c00) | (s << 16) | (r << 5) | r);   /* smull, umull */
	stf(4, r, fl);
	if (wide) {
		lsr64i(W16, r, 32);
		stf(4, W16, fh);
		ins(0xea00001f | (r << 16) | (r << 5));     /* tst x, x */
		nzcv_to_w9(0);
	} else {
		if (is_signed)
			ins(0xeb20c01f | (r << 16) | (r << 5));  /* cmp x, w, sxtw */
		else
			ins(0xeb20401f | (r << 16) | (r << 5));  /* cmp x, w, uxtw */
		cset(W15, A_NE);                          /* V */
		test_sz(4, r);
		nzcv_to_w9(0);
		SR(I_ORR, W9, W9, W15, 0, 11);
	}
	str_sz(4, W9, XF, o_cznv);
}

/* Z = !(bit s & mask of v), the other flags kept */
static void be_btst_v(int v, int s, int mask)
{
	andi(W17, vreg[s], mask);
	ins(0x1ac02400 | (W17 << 16) | (vreg[v] << 5) | W9);   /* lsr w9, v, w17 */
	andi(W9, W9, 1);
	eori(W9, W9, 1);
	ldr_sz(4, W16, XF, o_cznv);
	andi(W16, W16, ~(uae_u32)HOST_F_Z);
	SR(I_ORR, W16, W16, W9, 0, 6);
	str_sz(4, W16, XF, o_cznv);
}

/* Z = !(Dn bit), the other flags kept */
static void be_btst(int f, int bit)
{
	ldf(4, W9, f);
	ldr_sz(4, W16, XF, o_cznv);
	andi(W16, W16, ~(uae_u32)HOST_F_Z);
	ubfx(W9, W9, bit, 1);
	eori(W9, W9, 1);
	SR(I_ORR, W16, W16, W9, 0, 6);
	str_sz(4, W16, XF, o_cznv);
}

/* Branches. A label is where to patch, bound with be_bind. */

static void be_bind(uae_u8 *at) { patch(at, cp); }

/* Jumps if the 68k condition cc (2..15) is true (when_true) or false */
static uae_u8 *be_jcc68(int cc, int when_true)
{
	int holds;

	if (cp != cznv_in_w9)
		ldr_sz(4, W9, XF, o_cznv);
	switch (cc) {
	case 2: case 3:
		tsti(W9, HOST_F_C | HOST_F_Z);
		break;
	case 4: case 5:
		tsti(W9, HOST_F_C);
		break;
	case 6: case 7:
		tsti(W9, HOST_F_Z);
		break;
	case 8: case 9:
		tsti(W9, HOST_F_V);
		break;
	case 10: case 11:
		tsti(W9, HOST_F_N);
		break;
	default:
		/* GE, LT: N ^ V; GT, LE: (N ^ V) | Z */
		SR(I_EOR, W16, W9, W9, 1, 4);         /* V to N's bit */
		if (cc >= 14) {
			andi(W16, W16, HOST_F_N);
			andi(W17, W9, HOST_F_Z);
			SR(I_ORR, W16, W16, W17, 0, 0);
			tst_rr(W16, W16);
		} else {
			tsti(W16, HOST_F_N);
		}
		break;
	}
	/* even conditions (HI, CC, NE, VC, PL, GE, GT) hold when the test gave 0 */
	holds = (cc & 1) ? A_NE : A_EQ;
	return b_cond(when_true ? holds : holds ^ 1);
}

/* No guest registers are kept in host registers here yet */
#define BE_CACHED 0
static const int cache_host[1] = { 0 };
static void be_cache_load(void) { }

/* Dn.w -= 1; jumps if it was 0 */
static uae_u8 *be_dbcc_dec(int f)
{
	ldf(2, W9, f);
	subi(W16, W9, 1);
	stf(2, W16, f);
	return cbz(W9);
}

/* Leaving the block */

static void be_exit(struct ctx *c) { exit_add(c, b_()); }

static void be_exit_if_attention(struct ctx *c)
{
	ldr_sz(4, W9, XB, o_spcflags);
	ldr_sz(4, W16, XS, 0);
	SR(I_ORR, W9, W9, W16, 0, 0);
	exit_add(c, cbnz(W9));
}

static void be_store_pc(uaecptr pc) { movw(W9, pc); str_sz(4, W9, XB, o_pc); }
static void be_store_pc_reg(int v) { str_sz(4, vreg[v], XB, o_pc); }

/* Going on to the block at target, in the same page, once it has been
 * compiled (jit040_link): a branch to here, patched later. The PC is only
 * stored when the block is left. */
static void be_exit_linkable(struct ctx *c, uaecptr target)
{
	uae_u8 *site, *busy, *spent;
	int32_t d;

	ldr_sz(4, W9, XB, o_spcflags);
	ldr_sz(4, W16, XS, 0);
	SR(I_ORR, W9, W9, W16, 0, 0);
	busy = cbnz(W9);
	cmpi(XC, JIT040_CHAIN_CYCLES);
	spent = b_cond(A_HS);
	site = b_();
	patch(site, cp);
	/* not linked yet: tell jit040_link where to */
	d = (int32_t)(site - cp);
	ins(0x10000000 | ((d & 3) << 29) | (((d >> 2) & 0x7ffff) << 5) | W9);   /* adr x9, site */
	ldrx(W16, XD, D_LINK);
	strx(W9, W16, 0);
	patch(busy, cp);
	patch(spent, cp);
	be_store_pc(target);
	be_exit(c);
}
static void be_link(uae_u8 *site, uae_u8 *to)
{
	patch(site, to);
}

/* A call to the instruction's handler, as m68k_run_jit040 makes it, and the
 * checks after it */
static void be_call(struct ctx *c, struct jit040_ins *in, uaecptr pc, bool last)
{
	ldr_sz(4, W9, XF, o_cznv);
	ldrx(W16, XD, D_F);
	str_sz(4, W9, W16, o_f_cznv);
	ldr_sz(4, W9, XF, o_x);
	str_sz(4, W9, W16, o_f_x);
	ldrx(W16, XD, D_RESTART);
	movz(W9, 1, 0);
	str_sz(1, W9, W16, 0);
	movw(W9, pc);
	str_sz(4, W9, XB, o_instruction_pc);
	str_sz(4, W9, XB, o_pc);
	movw(W9, in->opcode);
	ldrx(W16, XD, D_OPCODE);
	str_sz(2, W9, W16, 0);
	str_sz(2, W9, XB, o_opcode);
	movz(0, in->opcode, 0);
	movx(W9, (uintptr_t)in->fn);
	ins(0xd63f0000 | (W9 << 5));              /* blr x9 */
	SR(I_ADD, XC, XC, 0, 0, 0);               /* add w20, w20, w0 */
	if (last)
		return;
	be_exit_if_attention(c);
	ldr_sz(4, W9, XB, o_pc);
	movw(W16, pc + in->len);
	cmp_rr(W9, W16);
	exit_add(c, b_cond(A_NE));
	ldr_sz(1, W9, XB, o_s);
	cmpi(W9, c->b->s);
	exit_add(c, b_cond(A_NE));
}

/* Where the instruction's stub goes back to after the handler: the code
 * after it knows where the flags are and what the virtual registers hold, so
 * the stub puts them there too. The jump is to before the instruction's
 * cycles if they were the last thing emitted, as the cycles of the next
 * instruction may be added to them. */
static void be_join(struct stub *st, int cycles)
{
	rc_begin();
	memcpy(st->f, rc_f, sizeof(rc_f));
	st->flags = cp == cznv_in_w9;
	if (cp == cyc_end && cyc_n == cycles) {
		st->cont = cyc_start;
		st->cycles = cycles;
	} else {
		st->cont = cp;
		st->cycles = 0;
	}
	rc_end();
}
static void be_rejoin(struct stub *st)
{
	int v;

	if (st->cycles)
		addi_any(XC, XC, -st->cycles);
	for (v = 0; v < 5; v++) {
		if (st->f[v] >= 0)
			ldf(4, vreg[v], st->f[v]);
	}
	if (st->flags)
		ldr_sz(4, W9, XF, o_cznv);
	patch(b_(), st->cont);
}

/* Memory */

/* VA is a guest address for an access of the given size and kind: on the
 * fast path x15 is its address in the host's memory. The tag is that of the
 * page of the access's last byte, so an access across two pages, which have
 * different entries, does not match. */
static void be_tlb_check(struct ctx *c, int type, int size)
{
	int a = vreg[VA];

	rc_begin();
	lsri(W9, a, 12);
	andi(W9, W9, PT_TLB_SIZE - 1);
	ins(0x8b000000 | (W9 << 16) | (5 << 10) | (XT << 5) | W9);   /* add x9, x22, x9, lsl #5 */
	if (type)
		ins(0x91400000 | (((type * PT_TLB_SIZE * 32) >> 12) << 10) | (W9 << 5) | W9);   /* add x9, x9, #n, lsl #12 */
	ldr_sz(4, W15, W9, 20);
	if (size > 1) {
		addi(W16, a, size - 1);
		andi(W16, W16, 0xfffff000);
	} else {
		andi(W16, a, 0xfffff000);
	}
	orri(W16, W16, c->b->s ? 3 : 2);
	cmp_rr(W15, W16);
	slow_add(c, force_slow ? b_() : b_cond(A_NE));
	ldrx(W15, W9, 24);
	ins(0x8b204000 | (a << 16) | (W15 << 5) | W15);   /* add x15, x15, a, uxtw */
	rc_end();
}

/* v = the value at [x15], zero extended, from the 68k's byte order */
static void be_host_load(int sz, int v)
{
	int r = vreg[v];

	rc_begin();
	ldr_sz(sz, r, W15, 0);
	if (sz == 2)
		ins(0x5ac00400 | (r << 5) | r);       /* rev16 */
	else if (sz == 4)
		ins(0x5ac00800 | (r << 5) | r);       /* rev */
	rc_set(v);
}
/* [x15] = v, in the 68k's byte order (v is kept) */
static void be_host_store(int sz, int v)
{
	int r = vreg[v];

	rc_begin();
	if (sz == 2) {
		ins(0x5ac00400 | (r << 5) | W9);      /* rev16 w9, r */
		r = W9;
	} else if (sz == 4) {
		ins(0x5ac00800 | (r << 5) | W9);      /* rev w9, r */
		r = W9;
	}
	str_sz(sz, r, W15, 0);
	rc_end();
}

/* the same at [x15 + off], off a multiple of the size */
static void be_host_load_off(int sz, int v, int off)
{
	int r = vreg[v];

	rc_begin();
	ldr_sz(sz, r, W15, off);
	ins((sz == 2 ? 0x5ac00400 : 0x5ac00800) | (r << 5) | r);   /* rev16, rev */
	rc_set(v);
}
static void be_host_store_off(int sz, int v, int off)
{
	rc_begin();
	ins((sz == 2 ? 0x5ac00400 : 0x5ac00800) | (vreg[v] << 5) | W9);
	str_sz(sz, W9, W15, off);
	rc_end();
}

/* the handler, if v is odd */
static void be_slow_if_odd(struct ctx *c, int v)
{
	rc_begin();
	slow_add(c, force_slow ? b_() : tbnz(vreg[v], 0));
	rc_end();
}
