/*
  PreviousTurbo - jit040_x86.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The x86-64 code of jit040_native.c (System V and Windows).

  rbx holds &regs, and everything the code uses is reached from it (it is
  all in the program's data, close by); r15d counts the cycles. The virtual
  registers are r8d (V0), r10d (V1), ecx (VA, addresses), r11d (VJ, jump
  targets) and r9d (VI, indexes); rax and rdx are the code's own. r12d,
  r13d, r14d and ebp hold the guest registers the block uses most (see
  cache_reg), written through to regs as well.

  regflags.cznv here has the layout of machdep/m68k.h for x86 hosts
  (SAHF_SETO_PROFITABLE): LAHF's flags in bits 8-15 and SETO's in bit 0, so
  the flags of an x86 instruction like the 68k one are taken as they are.
*/

#define HOST_F_C 0x0100
#define HOST_F_Z 0x4000
#define HOST_F_N 0x8000
#define HOST_F_V 0x0001
#define HOST_CZNV (HOST_F_C | HOST_F_Z | HOST_F_N | HOST_F_V)

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { X_ADD = 0x00, X_OR = 0x08, X_AND = 0x20, X_SUB = 0x28, X_XOR = 0x30, X_CMP = 0x38 };
enum { X_ROL = 0, X_ROR = 1, X_SHL = 4, X_SHR = 5, X_SAR = 7 };
enum { CC_O = 0, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };

static const int vreg[] = { R8, R10, RCX, R11, R9 };   /* V0, V1, VA, VJ, VI */
static const int xop_of[] = { X_ADD, X_SUB, X_AND, X_OR, X_XOR, X_CMP };

#ifdef _WIN32
#define ARG0 RCX
#else
#define ARG0 RDI
#endif

/* Offsets from regs of everything the code uses */
static int32_t o_regs, o_pc, o_instruction_pc, o_opcode, o_spcflags, o_s;
static int32_t o_cznv, o_x, o_f_cznv, o_f_x, o_mmu_restart, o_mmu_opcode, o_stop;
static int32_t o_tlb, o_link, o_link_from, o_usp, o_ccr;

/* regflags.cznv for each value of the 68k's N, Z, V and C (bits 3-0) */
static uae_u32 ccr_cznv[16];

static bool offset_of(const volatile void *p, int32_t *o)
{
	intptr_t d = (const volatile uae_u8 *)p - (const uae_u8 *)&regs;
	if (d < -0x40000000 || d > 0x40000000)
		return false;
	*o = (int32_t)d;
	return true;
}

static int32_t field(int f)
{
	if (f < 16)
		return o_regs + 4 * f;
	switch (f) {
	case F_PC: return o_pc;
	case F_CZNV: return o_cznv;
	case F_X: return o_x;
	default: return o_usp;
	}
}

/* The host registers that may hold guest registers, and which guest
 * register each holds (cache_reg[f], -1 for none): set by the front end
 * for each block. They are loaded when the block is entered and after a
 * handler is called, and every write to the guest register is made to both. */
#define BE_CACHED 4
static const int cache_host[BE_CACHED] = { R12, R13, R14, RBP };

static bool be_init(bool *mem_ok)
{
	int i;

	if (sizeof(regflags.cznv) != 4 || sizeof(regflags.x) != 4 || sizeof(mmu_restart) != 1 ||
	    sizeof(regs.s) != 1 || sizeof(regs.opcode) != 2 || sizeof(mmu_opcode) != 2 ||
	    sizeof(regs.usp) != 4)
		return false;
	for (i = 0; i < 16; i++) {
		ccr_cznv[i] = ((i & 8) ? HOST_F_N : 0) | ((i & 4) ? HOST_F_Z : 0) |
			((i & 2) ? HOST_F_V : 0) | ((i & 1) ? HOST_F_C : 0);
	}
	if (!offset_of(&regs.regs[0], &o_regs) || !offset_of(&regs.pc, &o_pc) ||
	    !offset_of(&regs.instruction_pc, &o_instruction_pc) || !offset_of(&regs.opcode, &o_opcode) ||
	    !offset_of(&regs.spcflags, &o_spcflags) || !offset_of(&regs.s, &o_s) ||
	    !offset_of(&regflags.cznv, &o_cznv) || !offset_of(&regflags.x, &o_x) ||
	    !offset_of(&jit040_f.cznv, &o_f_cznv) || !offset_of(&jit040_f.x, &o_f_x) ||
	    !offset_of(&mmu_restart, &o_mmu_restart) || !offset_of(&mmu_opcode, &o_mmu_opcode) ||
	    !offset_of(&jit040_stop, &o_stop) || !offset_of(&jit040_link_site, &o_link) ||
	    !offset_of(&jit040_link_from, &o_link_from) ||
	    !offset_of(&regs.usp, &o_usp) || !offset_of(ccr_cznv, &o_ccr))
		return false;
	*mem_ok = offset_of(&pt_tlb[0][0], &o_tlb) && sizeof(struct pt_tlb_entry) == 32 &&
		offsetof(struct pt_tlb_entry, jit_tag) == 20 && offsetof(struct pt_tlb_entry, jit_host) == 24;
	return true;
}

#ifdef _WIN32
#define CODE_START 64   /* after the unwind information */

/* A fault in a handler called from the native code is taken with longjmp,
 * for which Windows unwinds the stack: it has to know the native code's
 * frame. Every block sets it up the same way (push rbx, r15, r12, r13, r14,
 * rbp, sub rsp,40), so one entry, with its unwind information at the start of
 * the code buffer, describes all of them. */
static bool be_register_code(uae_u8 *buf, size_t size)
{
	static RUNTIME_FUNCTION rf;
	uae_u8 *ui = buf;

	ui[0] = 1;                   /* version 1, no handler */
	ui[1] = 14;                  /* size of the prologue */
	ui[2] = 7;                   /* unwind codes, last first: */
	ui[3] = 0;                   /* no frame register */
	ui[4] = 14; ui[5] = 2 | (4 << 4);   /* sub rsp, 40 (UWOP_ALLOC_SMALL, 5 * 8) */
	ui[6] = 10; ui[7] = 0 | (5 << 4);   /* push rbp (UWOP_PUSH_NONVOL) */
	ui[8] = 9; ui[9] = 0 | (14 << 4);   /* push r14 */
	ui[10] = 7; ui[11] = 0 | (13 << 4); /* push r13 */
	ui[12] = 5; ui[13] = 0 | (12 << 4); /* push r12 */
	ui[14] = 3; ui[15] = 0 | (15 << 4); /* push r15 */
	ui[16] = 1; ui[17] = 0 | (3 << 4);  /* push rbx */
	rf.BeginAddress = CODE_START;
	rf.EndAddress = (DWORD)size;
	rf.UnwindData = 0;
	return RtlAddFunctionTable(&rf, 1, (DWORD64)(uintptr_t)buf);
}
#else
#define CODE_START 0
static bool be_register_code(uae_u8 *buf, size_t size)
{
	return true;
}
#endif

/* The code is written and run with the same mapping */
static void be_write_begin(void) { }
static void be_write_end(uae_u8 *start, uae_u8 *end) { }

/* Encoding */

static void e8(int x) { *cp++ = (uae_u8)x; }
static void e16(int x) { e8(x); e8(x >> 8); }
static void e32(uae_u32 x) { e8(x); e8(x >> 8); e8(x >> 16); e8(x >> 24); }
static void e64(uae_u64 x) { e32((uae_u32)x); e32((uae_u32)(x >> 32)); }

static void rex(int w, int r, int b)
{
	int v = 0x40 | (w << 3) | ((r >> 3) << 2) | (b >> 3);
	if (v != 0x40)
		e8(v);
}
/* the memory operand [rbx + d] */
static void mem(int r, int32_t d)
{
	e8(0x80 | ((r & 7) << 3) | RBX);
	e32(d);
}
static void modrr(int r, int b) { e8(0xc0 | ((r & 7) << 3) | (b & 7)); }

static void ld32(int r, int32_t d) { rex(0, r, 0); e8(0x8b); mem(r, d); }
static void st32(int32_t d, int r) { rex(0, r, 0); e8(0x89); mem(r, d); }
static void st32i(int32_t d, uae_u32 v) { e8(0xc7); mem(0, d); e32(v); }
static void st16i(int32_t d, int v) { e8(0x66); e8(0xc7); mem(0, d); e16(v); }
static void st8i(int32_t d, int v) { e8(0xc6); mem(0, d); e8(v); }

/* zero extended loads of size 1, 2 or 4 */
static void ldz(int sz, int r, int32_t d)
{
	rex(0, r, 0);
	if (sz == 1) {
		e8(0x0f); e8(0xb6);
	} else if (sz == 2) {
		e8(0x0f); e8(0xb7);
	} else {
		e8(0x8b);
	}
	mem(r, d);
}
static void movsx_m(int sz, int r, int32_t d) { rex(0, r, 0); e8(0x0f); e8(sz == 1 ? 0xbe : 0xbf); mem(r, d); }
static void movsx_rr(int sz, int r, int b) { rex(0, r, b); e8(0x0f); e8(sz == 1 ? 0xbe : 0xbf); modrr(r, b); }
static void movzx_rr8(int r, int b) { rex(0, r, b); e8(0x0f); e8(0xb6); modrr(r, b); }
static void st(int sz, int32_t d, int r)
{
	if (sz == 2)
		e8(0x66);
	rex(0, r, 0);
	e8(sz == 1 ? 0x88 : 0x89);
	mem(r, d);
}
/* r op= [rbx + d] */
static void alu_rm(int sz, int op, int r, int32_t d)
{
	if (sz == 2)
		e8(0x66);
	rex(0, r, 0);
	e8(op + (sz == 1 ? 2 : 3));
	mem(r, d);
}
/* r op= imm */
static void alu_ri(int sz, int op, int r, uae_u32 v)
{
	if (sz == 2)
		e8(0x66);
	rex(0, 0, r);
	e8(sz == 1 ? 0x80 : 0x81);
	modrr(op >> 3, r);
	if (sz == 1)
		e8(v);
	else if (sz == 2)
		e16(v);
	else
		e32(v);
}
/* [rbx + d] op= imm (32 bit) */
static void alu_mi32(int op, int32_t d, uae_u32 v) { e8(0x81); mem(op >> 3, d); e32(v); }
/* r op= b */
static void alu_rr(int sz, int op, int r, int b)
{
	if (sz == 2)
		e8(0x66);
	rex(0, b, r);
	e8(op + (sz == 1 ? 0 : 1));
	modrr(b, r);
}
static void test_rr(int sz, int r)
{
	if (sz == 2)
		e8(0x66);
	rex(0, r, r);
	e8(sz == 1 ? 0x84 : 0x85);
	modrr(r, r);
}
static void test_ri32(int r, uae_u32 v) { rex(0, 0, r); e8(0xf7); modrr(0, r); e32(v); }
static void shift_ri(int sz, int kind, int r, int n)
{
	if (sz == 2)
		e8(0x66);
	rex(0, 0, r);
	e8(sz == 1 ? 0xc0 : 0xc1);
	modrr(kind, r);
	e8(n);
}
static void mov_ri(int r, uae_u32 v) { rex(0, 0, r); e8(0xb8 + (r & 7)); e32(v); }
static void mov_ri64(int r, uae_u64 v) { rex(1, 0, r); e8(0xb8 + (r & 7)); e64(v); }
static void mov_rr32(int r, int b) { rex(0, b, r); e8(0x89); modrr(b, r); }  /* r = b */
static void setcc(int cc, int r) { rex(0, 0, r); e8(0x0f); e8(0x90 + cc); modrr(0, r); }

/* Guest registers: in their host register if cached (see cache_reg), and
 * in regs. A write goes to both; byte operations on a host register always
 * have a REX prefix, so that 4 to 7 are spl to dil, not ah to bh. */

static int cached(int f)
{
	if (f >= 16)
		return -1;
	cache_uses[f]++;
	return cache_reg[f];
}
static void rex_h(int r, int b) { e8(0x40 | ((r >> 3) << 2) | (b >> 3)); }

/* r = f, zero extended from size sz */
static void ld_f(int sz, int r, int f)
{
	int h = cached(f);

	if (h < 0) {
		ldz(sz, r, field(f));
	} else if (sz == 4) {
		mov_rr32(r, h);
	} else {
		rex_h(r, h); e8(0x0f); e8(sz == 1 ? 0xb6 : 0xb7); modrr(r, h);
	}
}
/* r = f, sign extended from size sz (1 or 2) */
static void ldsx_f(int sz, int r, int f)
{
	int h = cached(f);

	if (h < 0) {
		movsx_m(sz, r, field(f));
	} else {
		rex_h(r, h); e8(0x0f); e8(sz == 1 ? 0xbe : 0xbf); modrr(r, h);
	}
}
/* f = r, size sz (the rest of f kept) */
static void st_f(int sz, int f, int r)
{
	int h = cached(f);

	if (h >= 0) {
		if (sz == 2)
			e8(0x66);
		rex_h(r, h); e8(sz == 1 ? 0x88 : 0x89); modrr(r, h);
	}
	st(sz, field(f), r);
}
/* f = v, size sz */
static void sti_f(int sz, int f, uae_u32 v)
{
	int h = cached(f);

	if (h >= 0) {
		if (sz == 1) {
			rex_h(0, h); e8(0xb0 + (h & 7)); e8(v);
		} else if (sz == 2) {
			e8(0x66); rex_h(0, h); e8(0xb8 + (h & 7)); e16(v);
		} else {
			mov_ri(h, v);
		}
	}
	if (sz == 1)
		st8i(field(f), v);
	else if (sz == 2)
		st16i(field(f), v);
	else
		st32i(field(f), v);
}
/* r op= f, size sz */
static void alu_rf(int sz, int op, int r, int f)
{
	int h = cached(f);

	if (h < 0) {
		alu_rm(sz, op, r, field(f));
		return;
	}
	if (sz == 2)
		e8(0x66);
	rex_h(h, r); e8(op + (sz == 1 ? 0 : 1)); modrr(h, r);
}

/* code that may be skipped, while nothing jumps to where cp is: the last
 * cycle count added (merged with the next one), regflags still in eax */
static uae_u8 *cyc_start, *cyc_end, *cznv_in_eax;
static int cyc_n;

/* something jumps to cp */
static void be_target(void) { cyc_end = cznv_in_eax = rc_at = NULL; }

static uae_u8 *jcc32(int cc) { e8(0x0f); e8(0x80 + cc); e32(0); return cp - 4; }
static uae_u8 *jmp32(void) { e8(0xe9); e32(0); return cp - 4; }
static void patch(uae_u8 *at, uae_u8 *to)
{
	int32_t rel = (int32_t)(to - (at + 4));
	memcpy(at, &rel, 4);
	if (to == cp)
		be_target();
}

/* Flags */

/* C into X, from the flags in eax */
static void x_from_eax(void)
{
	shift_ri(4, X_SHR, RAX, 8);
	alu_ri(4, X_AND, RAX, 1);
	st32(o_x, RAX);
}
/* the host's C, Z, N and V flags into regflags, and C into X if asked */
static void flags_from_host(int x)
{
	e8(0x9f);                  /* lahf */
	setcc(CC_O, RAX);          /* seto al */
	e8(0x25); e32(HOST_CZNV);  /* and eax, C|Z|N|V */
	st32(o_cznv, RAX);
	if (x)
		x_from_eax();
	else
		cznv_in_eax = cp;
}
static void flags_after(int fl)
{
	fl = fl_needed(fl);
	if (fl == FL_X) {
		setcc(CC_B, RAX);          /* X = C */
		movzx_rr8(RAX, RAX);
		st32(o_x, RAX);
	} else if (fl != FL_NONE) {
		flags_from_host(fl == FL_ARITH_X);
	}
}

/* The operations */

static void be_prologue(struct jit040_block *b)
{
	e8(0x53);                    /* push rbx */
	e8(0x41); e8(0x57);          /* push r15 */
	e8(0x41); e8(0x54);          /* push r12 */
	e8(0x41); e8(0x55);          /* push r13 */
	e8(0x41); e8(0x56);          /* push r14 */
	e8(0x55);                    /* push rbp */
#ifdef _WIN32
	e8(0x48); e8(0x83); e8(0xec); e8(0x28);   /* sub rsp, 40 */
#else
	e8(0x48); e8(0x83); e8(0xec); e8(0x08);   /* sub rsp, 8: aligned for calls */
#endif
	mov_ri64(RBX, (uintptr_t)&regs);
	e8(0x45); e8(0x31); e8(0xff);             /* xor r15d, r15d */
	b->chain = cp;                            /* where other blocks go on into it */
}
static void be_epilogue(void)
{
	e8(0x44); e8(0x89); e8(0xf8);             /* mov eax, r15d */
#ifdef _WIN32
	e8(0x48); e8(0x83); e8(0xc4); e8(0x28);   /* add rsp, 40 */
#else
	e8(0x48); e8(0x83); e8(0xc4); e8(0x08);   /* add rsp, 8 */
#endif
	e8(0x5d);                    /* pop rbp */
	e8(0x41); e8(0x5e);          /* pop r14 */
	e8(0x41); e8(0x5d);          /* pop r13 */
	e8(0x41); e8(0x5c);          /* pop r12 */
	e8(0x41); e8(0x5f);          /* pop r15 */
	e8(0x5b);                    /* pop rbx */
	e8(0xc3);                    /* ret */
}
/* The cached guest registers from regs */
static void be_cache_load(void)
{
	int f;

	for (f = 0; f < 16; f++) {
		if (cache_reg[f] >= 0)
			ld32(cache_reg[f], field(f));
	}
}

static void be_add_cycles(int n)
{
	bool keep = cp == cznv_in_eax;

	rc_begin();
	if (cp == cyc_end) {
		cp = cyc_start;
		n += cyc_n;
	}
	cyc_start = cp;
	e8(0x41); e8(0x81); e8(0xc7); e32(n);   /* add r15d, n */
	cyc_end = cp;
	cyc_n = n;
	if (keep)
		cznv_in_eax = cp;
	rc_end();
}

/* (the operations up to be_set_nz keep track of what the virtual registers
 * hold, see rc_f; the values they hold are zero extended to 64 bits) */
static void be_ld(int sz, int v, int f)
{
	rc_begin();
	if (sz == 4 && f < 16) {
		int u = rc_find(f);
		if (u != v) {
			if (u >= 0)
				mov_rr32(vreg[v], vreg[u]);
			else
				ld_f(4, vreg[v], f);
			rc_f[v] = f;
		}
	} else {
		ld_f(sz, vreg[v], f);
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
	ldsx_f(sz, vreg[v], f);
	rc_f[v] = -1;
	rc_end();
}
static void be_st(int sz, int f, int v)
{
	rc_begin();
	st_f(sz, f, vreg[v]);
	rc_kill(f);
	if (sz == 4 && f < 16)
		rc_f[v] = f;
	rc_end();
}
static void be_sti(int sz, int f, uae_u32 v)
{
	rc_begin();
	sti_f(sz, f, v);
	rc_kill(f);
	rc_end();
}
/* v is written */
static void rc_set(int v)
{
	rc_f[v] = -1;
	rc_end();
}
static void be_movi(int v, uae_u32 x) { rc_begin(); mov_ri(vreg[v], x); rc_set(v); }
static void be_mov(int v, int s) { rc_begin(); mov_rr32(vreg[v], vreg[s]); rc_f[v] = rc_f[s]; rc_end(); }
static void be_sext(int sz, int v) { rc_begin(); movsx_rr(sz, vreg[v], vreg[v]); rc_set(v); }
static void be_add(int v, int s) { rc_begin(); alu_rr(4, X_ADD, vreg[v], vreg[s]); rc_set(v); }
static void be_addi(int v, uae_s32 x) { rc_begin(); if (x) alu_ri(4, X_ADD, vreg[v], (uae_u32)x); rc_set(v); }
static void be_shli(int v, int n) { rc_begin(); shift_ri(4, X_SHL, vreg[v], n); rc_set(v); }
static void be_field_addi(int f, uae_s32 x)
{
	int h;

	rc_begin();
	h = cached(f);
	if (h < 0) {
		alu_mi32(X_ADD, field(f), (uae_u32)x);
	} else {
		alu_ri(4, X_ADD, h, (uae_u32)x);
		st32(field(f), h);
	}
	rc_kill(f);
	rc_end();
}

static void be_alu(int sz, int op, int v, int s, int fl)
{
	rc_begin();
	alu_rr(sz, xop_of[op], vreg[v], vreg[s]);
	flags_after(fl);
	rc_set(v);
}
static void be_alui(int sz, int op, int v, uae_u32 x, int fl)
{
	rc_begin();
	alu_ri(sz, xop_of[op], vreg[v], x);
	flags_after(fl);
	rc_set(v);
}
static void be_alu_field(int sz, int op, int v, int f, int fl)
{
	rc_begin();
	alu_rf(sz, xop_of[op], vreg[v], f);
	flags_after(fl);
	rc_set(v);
}
static void be_test(int sz, int v)
{
	rc_begin();
	if (need & F_NZVC) {
		test_rr(sz, vreg[v]);
		flags_from_host(0);
	}
	rc_end();
}
static void be_set_nz(int n, int z)
{
	rc_begin();
	if (need & F_NZVC)
		st32i(o_cznv, (n ? HOST_F_N : 0) | (z ? HOST_F_Z : 0));
	rc_end();
}
static void be_neg(int sz, int v)
{
	int r = vreg[v];
	if (sz == 2)
		e8(0x66);
	rex(0, 0, r);
	e8(sz == 1 ? 0xf6 : 0xf7);
	modrr(3, r);
	flags_from_host(1);
}
static void be_not(int sz, int v)
{
	alu_ri(sz, X_XOR, vreg[v], sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffff);
	if (need & F_NZVC)
		flags_from_host(0);
}
/* v = v * s on their low words, signed or not, 32 bit result: N, Z, V and C
 * cleared (MULU.W, MULS.W) */
static void be_mul16(int is_signed, int v, int s)
{
	int r = vreg[v], b = vreg[s];
	rex(0, r, r); e8(0x0f); e8(is_signed ? 0xbf : 0xb7); modrr(r, r);   /* movsx/movzx r, r16 */
	rex(0, b, b); e8(0x0f); e8(is_signed ? 0xbf : 0xb7); modrr(b, b);
	rex(0, r, b); e8(0x0f); e8(0xaf); modrr(r, b);                      /* imul r, b */
	be_test(4, v);
}
static void be_swap(int v)
{
	shift_ri(4, X_ROL, vreg[v], 16);
	be_test(4, v);
}

/* Shifts and rotates by n (1-8, less than the size in bits) with the 68k's
 * flags: kind 0 AS, 1 LS, 3 RO; the result is stored in f too, unless f < 0
 * (rdx is kept) */
static void be_shift(int sz, int kind, int left, int v, int n, int f)
{
	int r = vreg[v], nbits = sz * 8;

	if (kind == 3) {
		/* ROL, ROR: C is the last bit rotated, X is kept */
		shift_ri(sz, left ? X_ROL : X_ROR, r, n);
		if (f >= 0)
			st_f(sz, f, r);
		setcc(CC_B, R11);
		test_rr(sz, r);
		e8(0x9f);                         /* lahf */
		alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
		movzx_rr8(R11, R11);
		shift_ri(4, X_SHL, R11, 8);       /* C */
		alu_rr(4, X_OR, RAX, R11);
		st32(o_cznv, RAX);
		return;
	}
	if (left && kind == 0) {
		/* ASL: V if the bits shifted through the sign bit differ */
		if (sz == 4)
			mov_rr32(R9, r);
		else
			movsx_rr(sz, R9, r);
		shift_ri(sz, X_SHL, r, n);
		if (f >= 0)
			st_f(sz, f, r);
		e8(0x9f);                         /* lahf */
		alu_ri(4, X_AND, RAX, HOST_F_C | HOST_F_Z | HOST_F_N);
		shift_ri(4, X_SAR, R9, nbits - 1 - n);
		alu_ri(4, X_ADD, R9, 1);
		alu_ri(4, X_CMP, R9, 1);
		setcc(CC_A, R11);
		movzx_rr8(R11, R11);              /* V */
		alu_rr(4, X_OR, RAX, R11);
		st32(o_cznv, RAX);
		x_from_eax();
		return;
	}
	/* LSL, LSR, ASR: V cleared */
	shift_ri(sz, left ? X_SHL : kind == 0 ? X_SAR : X_SHR, r, n);
	if (f >= 0)
		st_f(sz, f, r);
	e8(0x9f);                             /* lahf */
	alu_ri(4, X_AND, RAX, HOST_F_C | HOST_F_Z | HOST_F_N);
	st32(o_cznv, RAX);
	x_from_eax();
}

/* LSL, LSR, ASL, ASR of Dy by the count in Dx (mod 64), as 64-bit shifts,
 * with the value in the high half for those to the right: the result and
 * the last bit shifted out (C and X, none past the size) come out for every
 * count. ASL's V: the value shifted by at most the size no longer fits. A
 * count of 0 only sets N and Z, C and V cleared, X kept. */
static void be_shift_reg(int sz, int kind, int left, int fc, int fv)
{
	int n = sz * 8;
	uae_u8 *shift, *done, *small;

	if (fc >= 0) {
		ld_f(4, RCX, fc);
		alu_ri(4, X_AND, RCX, 63);
	} else {
		mov_ri(RCX, -1 - fc);                 /* a count of -1 - fc */
	}
	if (kind == 0 && !left && sz != 4)
		ldsx_f(sz, R8, fv);
	else
		ld_f(sz, R8, fv);
	test_rr(4, RCX);
	shift = jcc32(CC_NE);
	test_rr(sz, R8);
	e8(0x9f);                                 /* lahf */
	alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
	st32(o_cznv, RAX);
	done = jmp32();
	patch(shift, cp);
	if (left) {
		if (kind == 0) {
			/* r9 = the value, sign extended, << min(count, size) */
			rex(1, R9, R8);
			if (sz == 4) {
				e8(0x63);                     /* movsxd r9, r8d */
			} else {
				e8(0x0f); e8(sz == 1 ? 0xbe : 0xbf);   /* movsx r9, r8b/r8w */
			}
			modrr(R9, R8);
			mov_rr32(RAX, RCX);
			alu_ri(4, X_CMP, RCX, n);
			small = jcc32(CC_BE);
			mov_ri(RCX, n);
			patch(small, cp);
			rex(1, 0, R9); e8(0xd3); modrr(X_SHL, R9);   /* shl r9, cl */
			mov_rr32(RCX, RAX);
			/* r11 = V: the low size bits sign extended differ */
			rex(1, R11, R9);
			if (sz == 4) {
				e8(0x63);
			} else {
				e8(0x0f); e8(sz == 1 ? 0xbe : 0xbf);
			}
			modrr(R11, R9);
			rex(1, R9, R11); e8(0x39); modrr(R9, R11);   /* cmp r11, r9 */
			setcc(CC_NE, R11);
			movzx_rr8(R11, R11);
		}
		rex(1, 0, R8); e8(0xd3); modrr(X_SHL, R8);   /* shl r8, cl */
		rex(1, R8, RDX); e8(0x89); modrr(R8, RDX);   /* mov rdx, r8 */
		rex(1, 0, RDX); e8(0xc1); modrr(X_SHR, RDX); e8(n);
	} else {
		rex(1, 0, R8); e8(0xc1); modrr(X_SHL, R8); e8(32);
		rex(1, 0, R8); e8(0xd3); modrr(kind == 0 ? X_SAR : X_SHR, R8);
		rex(1, R8, RDX); e8(0x89); modrr(R8, RDX);
		rex(1, 0, RDX); e8(0xc1); modrr(X_SHR, RDX); e8(31);
		rex(1, 0, R8); e8(0xc1); modrr(X_SHR, R8); e8(32);
	}
	alu_ri(4, X_AND, RDX, 1);                 /* C */
	st_f(sz, fv, R8);
	test_rr(sz, R8);
	e8(0x9f);                                 /* lahf */
	alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
	shift_ri(4, X_SHL, RDX, 8);
	alu_rr(4, X_OR, RAX, RDX);
	if (left && kind == 0)
		alu_rr(4, X_OR, RAX, R11);
	st32(o_cznv, RAX);
	x_from_eax();
	patch(done, cp);
}

/* ROXL, ROXR of Dn by n (1-8): X above the value, rotated as size + 1 bits;
 * C and X are then the bit above the result, V is cleared. For f < 0 the
 * value is in V0, zero extended, and the result is left there (rdx kept). */
static void be_rox(int sz, int left, int n, int f)
{
	int nbits = sz * 8;

	if (f >= 0)
		ld_f(sz, R8, f);
	ld32(R11, o_x);
	alu_ri(4, X_AND, R11, 1);
	rex(1, 0, R11); e8(0xc1); modrr(X_SHL, R11); e8(nbits);
	rex(1, R11, R8); e8(0x09); modrr(R11, R8);       /* or r8, r11 */
	rex(1, R8, R9); e8(0x89); modrr(R8, R9);         /* mov r9, r8 */
	rex(1, 0, R8); e8(0xc1); modrr(left ? X_SHL : X_SHR, R8); e8(n);
	rex(1, 0, R9); e8(0xc1); modrr(left ? X_SHR : X_SHL, R9); e8(nbits + 1 - n);
	rex(1, R9, R8); e8(0x09); modrr(R9, R8);         /* or r8, r9 */
	rex(1, R8, R11); e8(0x89); modrr(R8, R11);       /* mov r11, r8 */
	rex(1, 0, R11); e8(0xc1); modrr(X_SHR, R11); e8(nbits);
	alu_ri(4, X_AND, R11, 1);                        /* C */
	if (f >= 0)
		st_f(sz, f, R8);
	test_rr(sz, R8);
	e8(0x9f);                                        /* lahf */
	alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
	shift_ri(4, X_SHL, R11, 8);
	alu_rr(4, X_OR, RAX, R11);
	st32(o_cznv, RAX);
	x_from_eax();
}

/* V0 * V1, 32 x 32 bits as one 64-bit product: into fl (V if it does not
 * fit in 32 bits, signed or not) or fh:fl (V cleared); C cleared */
static void be_mull(int is_signed, int wide, int fl, int fh)
{
	if (is_signed) {
		rex(1, R8, R8); e8(0x63); modrr(R8, R8);      /* movsxd r8, r8d */
		rex(1, R10, R10); e8(0x63); modrr(R10, R10);
	}
	rex(1, R8, R10); e8(0x0f); e8(0xaf); modrr(R8, R10);   /* imul r8, r10 */
	st_f(4, fl, R8);
	if (wide) {
		rex(1, R8, RDX); e8(0x89); modrr(R8, RDX);   /* mov rdx, r8 */
		rex(1, 0, RDX); e8(0xc1); modrr(X_SHR, RDX); e8(32);
		st_f(4, fh, RDX);
		rex(1, R8, R8); e8(0x85); modrr(R8, R8);     /* test r8, r8 */
		e8(0x9f);                                    /* lahf */
		alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
	} else {
		if (is_signed) {
			rex(1, RDX, R8); e8(0x63); modrr(RDX, R8);   /* movsxd rdx, r8d */
		} else {
			mov_rr32(RDX, R8);
		}
		rex(1, R8, RDX); e8(0x39); modrr(R8, RDX);   /* cmp rdx, r8 */
		setcc(CC_NE, RDX);
		movzx_rr8(RDX, RDX);                         /* V */
		test_rr(4, R8);
		e8(0x9f);                                    /* lahf */
		alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
		alu_rr(4, X_OR, RAX, RDX);
	}
	st32(o_cznv, RAX);
}

/* Z = !(bit s & mask of v), the other flags kept */
static void be_btst_v(int v, int s, int mask)
{
	mov_rr32(RAX, vreg[s]);
	alu_ri(4, X_AND, RAX, mask);
	rex(0, RAX, vreg[v]); e8(0x0f); e8(0xa3); modrr(RAX, vreg[v]);   /* bt v, eax */
	setcc(CC_AE, RAX);
	movzx_rr8(RAX, RAX);
	shift_ri(4, X_SHL, RAX, 14);
	ld32(R11, o_cznv);
	alu_ri(4, X_AND, R11, ~(uae_u32)HOST_F_Z);
	alu_rr(4, X_OR, R11, RAX);
	st32(o_cznv, R11);
}

/* Z = !(Dn bit), the other flags kept */
static void be_btst(int f, int bit)
{
	ld32(RDX, o_cznv);
	alu_ri(4, X_AND, RDX, ~(uae_u32)HOST_F_Z);
	{
		int h = cached(f);
		if (h < 0) {
			e8(0xf7); mem(0, field(f)); e32(1u << bit);   /* test dword [Dn], bit */
		} else {
			test_ri32(h, 1u << bit);
		}
	}
	setcc(CC_E, RAX);
	movzx_rr8(RAX, RAX);
	shift_ri(4, X_SHL, RAX, 14);
	alu_rr(4, X_OR, RDX, RAX);
	st32(o_cznv, RDX);
}

/* Branches. A label is where to patch, bound with be_bind. */

static void be_bind(uae_u8 *at) { patch(at, cp); }

/* Jumps if the 68k condition cc (2..15) is true (when_true) or false */
static uae_u8 *be_jcc68(int cc, int when_true)
{
	int jtrue, jfalse;

	if (cp != cznv_in_eax)
		ld32(RAX, o_cznv);
	switch (cc) {
	case 2: case 3:
		test_ri32(RAX, HOST_F_C | HOST_F_Z);
		break;
	case 4: case 5:
		test_ri32(RAX, HOST_F_C);
		break;
	case 6: case 7:
		test_ri32(RAX, HOST_F_Z);
		break;
	case 8: case 9:
		test_ri32(RAX, HOST_F_V);
		break;
	case 10: case 11:
		test_ri32(RAX, HOST_F_N);
		break;
	default:
		/* GE, LT: N ^ V; GT, LE: (N ^ V) | Z */
		mov_rr32(RDX, RAX);
		shift_ri(4, X_SHL, RDX, 15);          /* V to N's bit */
		alu_rr(4, X_XOR, RDX, RAX);
		if (cc >= 14) {
			alu_ri(4, X_AND, RDX, HOST_F_N);
			alu_ri(4, X_AND, RAX, HOST_F_Z);
			alu_rr(4, X_OR, RDX, RAX);
			test_rr(4, RDX);
		} else {
			test_ri32(RDX, HOST_F_N);
		}
		break;
	}
	/* even conditions (HI, CC, NE, VC, PL, GE, GT) hold when the test gave 0 */
	jtrue = (cc & 1) ? CC_NE : CC_E;
	jfalse = (cc & 1) ? CC_E : CC_NE;
	return jcc32(when_true ? jtrue : jfalse);
}

/* Dn.w -= 1; jumps if it was 0 */
static uae_u8 *be_dbcc_dec(int f)
{
	int h = cached(f);

	if (h < 0) {
		e8(0x66); e8(0x83); mem(5, field(f)); e8(1);   /* sub word [Dn], 1 */
	} else {
		e8(0x66); rex(0, 0, h); e8(0x83); modrr(5, h); e8(1);   /* sub h16, 1 */
		st(2, field(f), h);                            /* (the flags kept) */
	}
	return jcc32(CC_B);
}

/* Leaving the block */

static void be_exit(struct ctx *c) { exit_add(c, jmp32()); }

static void be_exit_if_attention(struct ctx *c)
{
	ld32(RAX, o_spcflags);
	alu_rm(4, X_OR, RAX, o_stop);
	exit_add(c, jcc32(CC_NE));
}

static void be_store_pc(uaecptr pc) { st32i(o_pc, pc); }
static void be_store_pc_reg(int v) { st32(o_pc, vreg[v]); }

/* Going on to the block at target, in the same page, once it has been
 * compiled (jit040_link): a jump to here, patched later. The PC is only
 * stored when the block is left. */
static void be_exit_linkable(struct ctx *c, uaecptr target)
{
	uae_u8 *site, *lea_end, *busy, *spent;
	int32_t d;

	ld32(RAX, o_spcflags);
	alu_rm(4, X_OR, RAX, o_stop);
	busy = jcc32(CC_NE);
	e8(0x41); e8(0x81); e8(0xff); e32(JIT040_CHAIN_CYCLES);   /* cmp r15d, cycles */
	spent = jcc32(CC_AE);
	site = jmp32();
	patch(site, cp);
	/* not linked yet: tell jit040_link where to */
	e8(0x48); e8(0x8d); e8(0x05); e32(0);   /* lea rax, [rip + d] */
	lea_end = cp;
	d = (int32_t)(site - lea_end);
	memcpy(lea_end - 4, &d, 4);
	rex(1, RAX, 0); e8(0x89); mem(RAX, o_link);   /* mov [jit040_link_site], rax */
	mov_ri64(RAX, (uintptr_t)c->b);
	rex(1, RAX, 0); e8(0x89); mem(RAX, o_link_from);   /* mov [jit040_link_from], rax */
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
	ld32(RAX, o_cznv);
	st32(o_f_cznv, RAX);
	ld32(RAX, o_x);
	st32(o_f_x, RAX);
	st8i(o_mmu_restart, 1);
	st32i(o_instruction_pc, pc);
	st32i(o_pc, pc);
	st16i(o_mmu_opcode, in->opcode);
	st16i(o_opcode, in->opcode);
	mov_ri(ARG0, in->opcode);
	mov_ri64(RAX, (uintptr_t)in->fn);
	e8(0xff); e8(0xd0);                       /* call rax */
	e8(0x41); e8(0x01); e8(0xc7);             /* add r15d, eax */
	if (last)
		return;
	be_cache_load();                          /* the handler may have changed them */
	be_exit_if_attention(c);
	e8(0x81); mem(7, o_pc); e32(pc + in->len);  /* cmp dword [pc], next */
	exit_add(c, jcc32(CC_NE));
	e8(0x80); mem(7, o_s); e8(c->b->s);        /* cmp byte [s], s */
	exit_add(c, jcc32(CC_NE));
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
	st->flags = cp == cznv_in_eax;
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

	if (st->cycles) {
		e8(0x41); e8(0x81); e8(0xef); e32(st->cycles);   /* sub r15d, n */
	}
	be_cache_load();
	for (v = 0; v < 5; v++) {
		if (st->f[v] >= 0)
			ldz(4, vreg[v], field(st->f[v]));
	}
	if (st->flags)
		ld32(RAX, o_cznv);
	patch(jmp32(), st->cont);
}

/* Memory */

static void slow_if(struct ctx *c, int cc)
{
	slow_add(c, force_slow ? jmp32() : jcc32(cc));
}

/* VA is a guest address for an access of the given size and kind: on the
 * fast path rdx is its address in the host's memory. The tag is that of the
 * page of the access's last byte, so an access across two pages, which have
 * different entries, does not match. */
static void be_tlb_check(struct ctx *c, int type, int size)
{
	int32_t base = o_tlb + type * PT_TLB_SIZE * 32;

	rc_begin();
	mov_rr32(RAX, RCX);
	shift_ri(4, X_SHR, RAX, 12);
	alu_ri(4, X_AND, RAX, PT_TLB_SIZE - 1);
	shift_ri(4, X_SHL, RAX, 5);
	if (size > 1) {
		e8(0x8d); e8(0x91); e32(size - 1);         /* lea edx, [rcx + size - 1] */
	} else {
		mov_rr32(RDX, RCX);
	}
	alu_ri(4, X_AND, RDX, 0xfffff000);
	alu_ri(4, X_OR, RDX, c->b->s ? 3 : 2);
	e8(0x3b); e8(0x94); e8(0x03); e32(base + 20);  /* cmp edx, [rbx + rax + jit_tag] */
	slow_if(c, CC_NE);
	e8(0x48); e8(0x8b); e8(0x94); e8(0x03); e32(base + 24);  /* mov rdx, [rbx + rax + jit_host] */
	e8(0x48); e8(0x01); e8(0xca);                  /* add rdx, rcx */
	rc_end();
}

/* v = the value at [rdx], zero extended, from the 68k's byte order */
static void be_host_load(int sz, int v)
{
	int r = vreg[v];

	rc_begin();
	rex(0, r, 0);
	if (sz == 1) {
		e8(0x0f); e8(0xb6);
	} else if (sz == 2) {
		e8(0x0f); e8(0xb7);
	} else {
		e8(0x8b);
	}
	e8(((r & 7) << 3) | RDX);
	if (sz == 2) {
		shift_ri(2, X_ROL, r, 8);
	} else if (sz == 4) {
		rex(0, 0, r);
		e8(0x0f); e8(0xc8 + (r & 7));             /* bswap */
	}
	rc_set(v);
}
/* [rdx] = v, in the 68k's byte order (v is kept) */
static void be_host_store(int sz, int v)
{
	int r = vreg[v];

	rc_begin();
	if (sz != 1) {
		mov_rr32(RAX, r);
		if (sz == 2)
			shift_ri(2, X_ROL, RAX, 8);
		else {
			e8(0x0f); e8(0xc8);                   /* bswap eax */
		}
		r = RAX;
	}
	if (sz == 2)
		e8(0x66);
	rex(0, r, 0);
	e8(sz == 1 ? 0x88 : 0x89);
	e8(((r & 7) << 3) | RDX);
	rc_end();
}

/* the same at [rdx + off], 0 <= off < 128 */
static void be_host_load_off(int sz, int v, int off)
{
	int r = vreg[v];

	rc_begin();
	rex(0, r, 0);
	if (sz == 1) {
		e8(0x0f); e8(0xb6);
	} else if (sz == 2) {
		e8(0x0f); e8(0xb7);
	} else {
		e8(0x8b);
	}
	e8(0x40 | ((r & 7) << 3) | RDX); e8(off);
	if (sz == 2) {
		shift_ri(2, X_ROL, r, 8);
	} else if (sz == 4) {
		rex(0, 0, r);
		e8(0x0f); e8(0xc8 + (r & 7));             /* bswap */
	}
	rc_set(v);
}
static void be_host_store_off(int sz, int v, int off)
{
	rc_begin();
	if (sz == 1) {
		int r = vreg[v];
		rex_h(r, 0); e8(0x88); e8(0x40 | ((r & 7) << 3) | RDX); e8(off);   /* mov [rdx + off], r8b */
		rc_end();
		return;
	}
	mov_rr32(RAX, vreg[v]);
	if (sz == 2) {
		shift_ri(2, X_ROL, RAX, 8);
		e8(0x66);
	} else {
		e8(0x0f); e8(0xc8);                       /* bswap eax */
	}
	e8(0x89); e8(0x40 | RDX); e8(off);           /* mov [rdx + off], eax/ax */
	rc_end();
}

/* the handler, if v is odd */
static void be_slow_if_odd(struct ctx *c, int v)
{
	rc_begin();
	test_ri32(vreg[v], 1);
	slow_if(c, CC_NE);
	rc_end();
}

/* More operations */

static void be_shri(int v, int n) { rc_begin(); shift_ri(4, X_SHR, vreg[v], n); rc_set(v); }
static void be_sari(int v, int n) { rc_begin(); shift_ri(4, X_SAR, vreg[v], n); rc_set(v); }

/* Jumps, bound with be_bind */
static uae_u8 *be_jmp(void) { return jmp32(); }
static uae_u8 *be_jz(int v) { test_rr(4, vreg[v]); return jcc32(CC_E); }
static uae_u8 *be_jnz(int v) { test_rr(4, vreg[v]); return jcc32(CC_NE); }
static uae_u8 *be_jeq_imm(int v, uae_u32 x) { alu_ri(4, X_CMP, vreg[v], x); return jcc32(CC_E); }

/* bit (s & mask) of v changed (op 1), cleared (2) or set (3), as BCHG, BCLR
 * and BSET (the flags are be_btst_v's) */
static void be_bitop(int op, int v, int s, int mask)
{
	rc_begin();
	mov_rr32(RAX, vreg[s]);
	alu_ri(4, X_AND, RAX, mask);
	rex(0, RAX, vreg[v]); e8(0x0f); e8(op == 1 ? 0xbb : op == 2 ? 0xb3 : 0xab); modrr(RAX, vreg[v]);   /* btc, btr, bts v, eax */
	rc_set(v);
}

/* v = v + s + X, or v - s - X (sub), on sz bytes: C, V and N as the
 * operation sets them, Z cleared if the result is not 0 and kept if it is,
 * X = C (ADDX, SUBX, NEGX; rdx kept) */
static void be_addx(int sz, int sub, int v, int s)
{
	rc_begin();
	e8(0x0f); e8(0xba); mem(4, o_x); e8(0);          /* bt dword [x], 0: CF = X */
	alu_rr(sz, sub ? 0x18 : 0x10, vreg[v], vreg[s]); /* sbb, adc */
	e8(0x9f);                                         /* lahf */
	setcc(CC_O, RAX);
	alu_ri(4, X_AND, RAX, HOST_CZNV);
	ld32(R11, o_cznv);
	alu_ri(4, X_OR, R11, ~(uae_u32)HOST_F_Z);
	alu_rr(4, X_AND, RAX, R11);                      /* Z only if it was set */
	st32(o_cznv, RAX);
	x_from_eax();
	rc_set(v);
}

/* The handler, if a and b (0 for b < 0) compare as cond, as values of sz
 * bytes (also after a helper that did the work: the self-test does not force
 * this one) */
enum { SC_LT, SC_GT, SC_ULT, SC_UGT, SC_EQ, SC_NE };
static void be_slow_cmp(struct ctx *c, int sz, int cond, int a, int b)
{
	static const int cc[] = { CC_L, CC_G, CC_B, CC_A, CC_E, CC_NE };

	rc_begin();
	if (b < 0)
		test_rr(sz, vreg[a]);
	else
		alu_rr(sz, X_CMP, vreg[a], vreg[b]);
	slow_add(c, jcc32(cc[cond]));
	rc_end();
}

/* The 68k's CCR from the low 5 bits of v: regflags, X too (rdx kept) */
static void be_ccr_set(int v)
{
	rc_begin();
	mov_rr32(RAX, vreg[v]);
	alu_ri(4, X_AND, RAX, 15);
	e8(0x8b); e8(0x84); e8(0x83); e32(o_ccr);         /* mov eax, [rbx + rax * 4 + ccr_cznv] */
	st32(o_cznv, RAX);
	mov_rr32(RAX, vreg[v]);
	shift_ri(4, X_SHR, RAX, 4);
	alu_ri(4, X_AND, RAX, 1);
	st32(o_x, RAX);
	rc_end();
}

/* the host address for be_ccall: p */
static void be_set_host(const void *p) { mov_ri64(RDX, (uintptr_t)p); }

/* A call to fn(V0, V1, VJ, the host address), its result in V0: the other
 * virtual registers are lost, the host address is kept. regs_written: it
 * may change the guest's registers. */
#ifdef _WIN32
#define SPILL 32                 /* above the callee's home area */
#else
#define SPILL 0
#endif
static void be_ccall(jit040_helper fn, bool regs_written)
{
	e8(0x48); e8(0x89); e8(0x54); e8(0x24); e8(SPILL);   /* mov [rsp + SPILL], rdx */
#ifdef _WIN32
	rex(1, RDX, R9); e8(0x89); modrr(RDX, R9);         /* mov r9, rdx */
	mov_rr32(RDX, R10);
	mov_rr32(RCX, R8);
	mov_rr32(R8, R11);
#else
	rex(1, RDX, RCX); e8(0x89); modrr(RDX, RCX);       /* mov rcx, rdx */
	mov_rr32(RDX, R11);
	mov_rr32(RDI, R8);
	mov_rr32(RSI, R10);
#endif
	mov_ri64(RAX, (uintptr_t)fn);
	e8(0xff); e8(0xd0);                                /* call rax */
	mov_rr32(R8, RAX);
	e8(0x48); e8(0x8b); e8(0x54); e8(0x24); e8(SPILL);   /* mov rdx, [rsp + SPILL] */
	if (regs_written)
		be_cache_load();
	cznv_in_eax = NULL;
}

/* ROL, ROR of Dy by the count in Dx (mod 64), or by -1 - fc for fc < 0: C is
 * the last bit rotated (cleared for a count of 0), X is kept, V cleared */
static void be_rot_reg(int sz, int left, int fc, int fv)
{
	int n = sz * 8;
	uae_u8 *zero;

	if (fc >= 0) {
		ld_f(4, RCX, fc);
		alu_ri(4, X_AND, RCX, 63);
	} else {
		mov_ri(RCX, -1 - fc);
	}
	ld_f(sz, R8, fv);
	e8(0x45); e8(0x31); e8(0xdb);                       /* xor r11d, r11d: C */
	test_rr(4, RCX);
	zero = jcc32(CC_E);
	if (sz == 2)
		e8(0x66);
	rex(0, 0, R8); e8(sz == 1 ? 0xd2 : 0xd3); modrr(left ? X_ROL : X_ROR, R8);   /* rol, ror r8, cl */
	mov_rr32(R11, R8);
	if (!left)
		shift_ri(4, X_SHR, R11, n - 1);
	alu_ri(4, X_AND, R11, 1);
	st_f(sz, fv, R8);
	patch(zero, cp);
	test_rr(sz, R8);
	e8(0x9f);                                           /* lahf */
	alu_ri(4, X_AND, RAX, HOST_F_N | HOST_F_Z);
	shift_ri(4, X_SHL, R11, 8);
	alu_rr(4, X_OR, RAX, R11);
	st32(o_cznv, RAX);
}
