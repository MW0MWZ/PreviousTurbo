/*
  PreviousTurbo - jit040_native.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Native code for the blocks of the 68040 block cache (jit040.c).

  A block that is run often is compiled to one host function. The common
  instructions are translated directly; every other instruction is a call
  to its handler, prepared exactly as the interpreter prepares it (PC, flags
  kept for a restart, opcode), followed by the same checks for leaving the
  block. Between instructions the guest's registers and flags are always in
  regs and regflags, so a handler, a fault or an exception sees the same
  state as with the interpreter.

  Memory accesses are checked in the page cache (cpummu.h) first: when it has
  a direct host pointer for the page, the access is done here, otherwise the
  instruction's handler does the whole instruction, from the state before
  it, as nothing has been changed until all of its checks have passed. Host
  pointers only exist for plain RAM and ROM, so the loads done before that
  have no effect, and the RAM pages holding code have none for writes.

  The function returns the cycles of the instructions it ran, and leaves the
  PC at the next instruction to run. A block that ends at a known address in
  its own page goes straight on to the code of the block there.

  This file has the decoding of the 68k's instructions; jit040_x86.h and
  jit040_arm64.h have the host's code for the operations it uses.
*/
#include <stddef.h>

#include "sysconfig.h"
#include "sysdeps.h"

#include "main.h"
#include "hatari-glue.h"

#include "options_cpu.h"
#include "memory.h"
#include "newcpu.h"
#include "cpummu.h"
#include "fpp.h"
#include "jit040.h"

#if defined(CPU_x86_64) || (defined(CPU_AARCH64) && !defined(_WIN32))
#define JIT040_NATIVE 1
#endif

uae_u8 *jit040_link_site;
struct jit040_block *jit040_link_from;   /* the block it is in */

#ifdef JIT040_NATIVE

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define CODE_SIZE (64 << 20)   /* AArch64 branches reach 128 MB */
#define CODE_MAX_BLOCK (64 * 1024)   /* room needed to compile one block */

/* The operations' virtual registers: values, an address, a jump target and
 * an index */
enum { V0, V1, VA, VJ, VI };

/* The guest's state they use: D0-D7 are 0-7 and A0-A7 8-15 */
#define FD(n) (n)
#define FA(n) (8 + (n))
enum { F_PC = 16, F_CZNV, F_X, F_USP };

/* Operations, and the 68k flags they set: none, as a logical operation (N
 * and Z, V and C cleared), as an addition or subtraction, and X too */
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_CMP };
enum { FL_NONE, FL_LOGIC, FL_ARITH, FL_ARITH_X, FL_X };

/* The 68k flags, for working out which ones an instruction must set: those
 * that may be read before another instruction sets them again */
#define FN 1
#define FZ 2
#define FV 4
#define FC 8
#define FX 16
#define F_NZVC (FN | FZ | FV | FC)
#define F_ALL (F_NZVC | FX)

static int need = F_ALL;     /* the flags the instruction being compiled must set */

/* fl as far as the flags are needed */
static int fl_needed(int fl)
{
	switch (fl) {
	case FL_LOGIC:
	case FL_ARITH:
		return (need & F_NZVC) ? fl : FL_NONE;
	case FL_ARITH_X:
		if (need & F_NZVC)
			return (need & FX) ? FL_ARITH_X : FL_ARITH;
		return (need & FX) ? FL_X : FL_NONE;
	default:
		return fl;
	}
}

/* the way to the handler when a memory access cannot be done directly */
struct stub {
	uae_u8 *jumps[12];
	int njumps;
	struct jit040_ins *in;
	uaecptr pc;
	bool last;          /* leave the block after the handler */
	uae_u8 *cont;       /* where the code goes on after the instruction (be_join) */
	int cycles;         /* the cycles counted there, taken off first */
	bool flags;         /* the backend's register for the flags holds them there */
	signed char f[5];   /* and what the virtual registers hold (rc_f) */
};

struct ctx {
	struct jit040_block *b;
	uae_u8 *exits[JIT040_MAX_INS * 8];
	int nexits;
	bool ended;         /* the code has left the block on every path */
	bool cycles_added;  /* the instruction's cycles are already counted */
	struct stub stubs[JIT040_MAX_INS];
	int nstubs;
	struct stub *cur;   /* the current instruction's, if it has one */
};

static uae_u8 *code_buf;
static size_t code_used;
static uae_u8 *cp;           /* where the code is written */
static bool mem_ok;          /* the page cache's entries are as the code expects */
static bool force_slow;      /* self-test: every access through the handler */

/* a branch to the end of the block, made by the backend */
static void exit_add(struct ctx *c, uae_u8 *site)
{
	c->exits[c->nexits++] = site;
}
/* a branch to the instruction's stub, made by the backend */
static void slow_add(struct ctx *c, uae_u8 *site)
{
	struct stub *st = c->cur;

	if (!st) {
		st = c->cur = &c->stubs[c->nstubs++];
		st->njumps = 0;
	}
	st->jumps[st->njumps++] = site;
}

/* The guest registers (0-15) whose values the virtual registers hold, kept
 * by the backend's simple operations in straight code. It is only so while
 * nothing else has been emitted (rc_at is cp) and nothing jumps to cp. */
static signed char rc_f[5];
static uae_u8 *rc_at;

static void rc_begin(void)
{
	if (rc_at != cp)
		memset(rc_f, -1, sizeof(rc_f));
}
static void rc_end(void)
{
	rc_at = cp;
}
/* the virtual register holding f, or -1 */
static int rc_find(int f)
{
	int v;

	for (v = 0; v < 5; v++) {
		if (rc_f[v] == f)
			return v;
	}
	return -1;
}
/* f was written */
static void rc_kill(int f)
{
	int v;

	for (v = 0; v < 5; v++) {
		if (rc_f[v] == f)
			rc_f[v] = -1;
	}
}

/* The guest registers each block keeps in host registers (the backend's
 * cache_host, BE_CACHED of them), chosen by how often a first pass over the
 * block used them */
static signed char cache_reg[16];
static int cache_uses[16];

/* A C function the native code calls (be_ccall) */
typedef uae_u32 (*jit040_helper)(uae_u32, uae_u32, uae_u32, uae_u8 *);

#if defined(CPU_x86_64)
#include "jit040_x86.h"
#else
#include "jit040_arm64.h"
#endif

static uae_u8 *alloc_code(size_t size)
{
#ifdef _WIN32
	return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
	int flags = MAP_PRIVATE | MAP_ANON;
	void *p;
#ifdef MAP_JIT
	flags |= MAP_JIT;
#endif
	p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
	return p == MAP_FAILED ? NULL : p;
#endif
}

static void free_code(uae_u8 *p, size_t size)
{
#ifdef _WIN32
	VirtualFree(p, 0, MEM_RELEASE);
#else
	munmap(p, size);
#endif
}

bool jit040_native_init(void)
{
	if (code_buf)
		return true;
	if (!be_init(&mem_ok))
		return false;
	code_buf = alloc_code(CODE_SIZE);
	if (!code_buf)
		return false;
	be_write_begin();
	if (!be_register_code(code_buf, CODE_SIZE)) {
		be_write_end(code_buf, code_buf);
		free_code(code_buf, CODE_SIZE);
		code_buf = NULL;
		return false;
	}
	be_write_end(code_buf, code_buf + CODE_START);
	code_used = CODE_START;
	return true;
}

void jit040_native_flush(void)
{
	code_used = CODE_START;
}

/* Leaving the block for a known address: in the block's page, the code may
 * go straight on to the code of the block there */
static void exit_to(struct ctx *c, uaecptr target)
{
	if ((target ^ c->b->pc) & 0xfffff000) {
		be_store_pc(target);
		be_exit(c);
	} else {
		be_exit_linkable(c, target);
	}
}

/* Leaving the block for the address in v */
static void exit_to_reg(struct ctx *c, int v)
{
	be_store_pc_reg(v);
	be_exit(c);
	c->ended = true;
}

/* The same for a jump of len bytes, which may have gone to the next
 * instruction of the block when it was recorded (in->len away, not len):
 * the block then goes on when it goes there again. Returns the length, or
 * 0 for a jump into its own extension words. */
static int jump_to_reg(struct ctx *c, struct jit040_ins *in, uaecptr pc, int len, int v)
{
	uae_u8 *on;

	if (!in->len || in->len == len) {
		exit_to_reg(c, v);
		return len;
	}
	if (in->len < len)
		return 0;
	on = be_jeq_imm(v, pc + in->len);
	be_store_pc_reg(v);
	be_exit(c);
	be_bind(on);
	return in->len;
}

void jit040_link(void)
{
	uae_u8 *site = jit040_link_site;
	struct jit040_block *from = jit040_link_from, *to;
	bool same = true;
	int k;

	jit040_link_site = NULL;
	if (regs.s != from->s)
		return;
	to = jit040_find(regs.pc);
	if (to && to->code && to->s == from->s && to->host_page == from->host_page) {
		/* the registers it keeps in host registers are already there if
		 * the block the exit is in keeps them in the same ones (as it
		 * does itself) */
		for (k = 0; k < BE_CACHED; k++) {
			if (to->cached[k] >= 0 && to->cached[k] != from->cached[k])
				same = false;
		}
		be_write_begin();
		be_link(site, same ? to->chain_self : to->chain);
		be_write_end(site - 4, site + 4);
	}
}

static uae_u16 code_word(struct ctx *c, uaecptr pc, int o)
{
	return do_get_mem_word(c->b->host_page + ((pc + o) & 0xfff));
}
static uae_u32 code_long(struct ctx *c, uaecptr pc, int o)
{
	return ((uae_u32)code_word(c, pc, o) << 16) | code_word(c, pc, o + 2);
}

static int size_of(int ss)  /* the size field of most instructions */
{
	return ss == 0 ? 1 : ss == 1 ? 2 : ss == 2 ? 4 : 0;
}

/* The instructions that only use registers and immediates. Returns the
 * instruction's length, or 0 if it is not one of those, and then nothing
 * has been emitted. */
static int emit_native(struct ctx *c, struct jit040_ins *in, uaecptr pc)
{
	uae_u16 op = in->opcode;
	int x = (op >> 9) & 7, y = op & 7;
	int mode = (op >> 3) & 7;
	int sz;

	switch (op >> 12) {
	case 0x0:
		/* BTST #n,Dn */
		if ((op & 0xfff8) == 0x0800) {
			be_btst(FD(y), code_word(c, pc, 2) & 31);
			return 4;
		}
		/* BTST Dn,Dm */
		if ((op & 0xf1f8) == 0x0100) {
			be_ld(4, V0, FD(y));
			be_ld(4, V1, FD(x));
			be_btst_v(V0, V1, 31);
			return 2;
		}
		/* ORI, ANDI, SUBI, ADDI, EORI, CMPI #imm,Dn */
		if (mode == 0 && !(op & 0x100) && (sz = size_of((op >> 6) & 3)) != 0) {
			int kind = (op >> 9) & 7, aop, fl, len = sz == 4 ? 6 : 4;
			uae_u32 iv = sz == 4 ? code_long(c, pc, 2) : sz == 2 ? code_word(c, pc, 2) : (code_word(c, pc, 2) & 0xff);
			switch (kind) {
			case 0: aop = OP_OR; fl = FL_LOGIC; break;
			case 1: aop = OP_AND; fl = FL_LOGIC; break;
			case 2: aop = OP_SUB; fl = FL_ARITH_X; break;
			case 3: aop = OP_ADD; fl = FL_ARITH_X; break;
			case 5: aop = OP_XOR; fl = FL_LOGIC; break;
			case 6: aop = OP_CMP; fl = FL_ARITH; break;
			default: return 0;
			}
			be_ld(sz, V0, FD(y));
			be_alui(sz, aop, V0, iv, fl);
			if (aop != OP_CMP)
				be_st(sz, FD(y), V0);
			return len;
		}
		return 0;

	case 0x1: case 0x2: case 0x3: {
		/* MOVE, MOVEA from a register or an immediate to a register */
		int dmode = (op >> 6) & 7;
		int ss = op >> 12;
		sz = ss == 1 ? 1 : ss == 3 ? 2 : 4;
		if (dmode > 1 || (dmode == 1 && sz == 1))
			return 0;
		if (mode == 0 || (mode == 1 && sz != 1)) {
			int src = mode == 0 ? FD(y) : FA(y);
			if (dmode == 1) {
				be_ldsx(sz, V0, src);
				be_st(4, FA(x), V0);
				return 2;
			}
			be_ld(sz, V0, src);
			be_st(sz, FD(x), V0);
			be_test(sz, V0);
			return 2;
		}
		if (mode == 7 && y == 4) {
			uae_u32 iv = sz == 4 ? code_long(c, pc, 2) : code_word(c, pc, 2);
			uae_s32 v = sz == 1 ? (uae_s8)iv : sz == 2 ? (uae_s16)iv : (uae_s32)iv;
			if (dmode == 1) {
				be_sti(4, FA(x), (uae_u32)v);
			} else {
				be_sti(sz, FD(x), (uae_u32)v);
				be_set_nz(v < 0, v == 0);
			}
			return sz == 4 ? 6 : 4;
		}
		return 0;
	}

	case 0x4:
		/* CLR.sz Dn */
		if ((op & 0xff00) == 0x4200 && mode == 0 && (sz = size_of((op >> 6) & 3)) != 0) {
			be_sti(sz, FD(y), 0);
			be_set_nz(0, 1);
			return 2;
		}
		/* NEG.sz Dn, NOT.sz Dn */
		if (((op & 0xff00) == 0x4400 || (op & 0xff00) == 0x4600) && mode == 0 && (sz = size_of((op >> 6) & 3)) != 0) {
			be_ld(sz, V0, FD(y));
			if ((op & 0xff00) == 0x4400)
				be_neg(sz, V0);
			else
				be_not(sz, V0);
			be_st(sz, FD(y), V0);
			return 2;
		}
		/* TST.sz Dn, TST.W/L An */
		if ((op & 0xff00) == 0x4a00 && (sz = size_of((op >> 6) & 3)) != 0 && (mode == 0 || (mode == 1 && sz != 1))) {
			be_ld(sz, V0, mode == 0 ? FD(y) : FA(y));
			be_test(sz, V0);
			return 2;
		}
		/* NOP (it waits for the 68040's writes, which are done here) */
		if (op == 0x4e71)
			return 2;
		/* MULU.L, MULS.L of Dn or #imm into Dl, or Dh:Dl */
		if ((op & 0xffc0) == 0x4c00 && (mode == 0 || (mode == 7 && y == 4))) {
			uae_u16 ext = code_word(c, pc, 2);
			int dl = (ext >> 12) & 7, dh = ext & 7, wide = (ext >> 10) & 1;
			if ((ext & 0x83f8) || (wide && dh == dl))
				return 0;
			if (mode == 0)
				be_ld(4, V1, FD(y));
			else
				be_movi(V1, code_long(c, pc, 4));
			be_ld(4, V0, FD(dl));
			be_mull((ext >> 11) & 1, wide, FD(dl), FD(dh));
			return mode == 0 ? 4 : 8;
		}
		/* SWAP Dn */
		if ((op & 0xfff8) == 0x4840) {
			be_ld(4, V0, FD(y));
			be_swap(V0);
			be_st(4, FD(y), V0);
			return 2;
		}
		/* EXT.W, EXT.L, EXTB.L Dn */
		if ((op & 0xfff8) == 0x4880 || (op & 0xfff8) == 0x48c0 || (op & 0xfff8) == 0x49c0) {
			int to = (op & 0xfff8) == 0x4880 ? 2 : 4;
			be_ldsx((op & 0xfff8) == 0x48c0 ? 2 : 1, V0, FD(y));
			be_st(to, FD(y), V0);
			be_test(to, V0);
			return 2;
		}
		/* LEA */
		if ((op & 0xf1c0) == 0x41c0) {
			if (mode == 2) {
				be_ld(4, V0, FA(y));
				be_st(4, FA(x), V0);
				return 2;
			}
			if (mode == 5) {
				be_ld(4, V0, FA(y));
				be_addi(V0, (uae_s16)code_word(c, pc, 2));
				be_st(4, FA(x), V0);
				return 4;
			}
			if (mode == 6) {
				uae_u16 dp = code_word(c, pc, 2);
				int r = (dp >> 12) & 15;
				if (dp & 0x100)
					return 0;   /* full format */
				if (dp & 0x800)
					be_ld(4, VI, r);
				else
					be_ldsx(2, VI, r);
				if ((dp >> 9) & 3)
					be_shli(VI, (dp >> 9) & 3);
				be_ld(4, V0, FA(y));
				be_add(V0, VI);
				be_addi(V0, (uae_s8)dp);
				be_st(4, FA(x), V0);
				return 4;
			}
			if (mode == 7 && y == 0) {
				be_sti(4, FA(x), (uae_u32)(uae_s32)(uae_s16)code_word(c, pc, 2));
				return 4;
			}
			if (mode == 7 && y == 1) {
				be_sti(4, FA(x), code_long(c, pc, 2));
				return 6;
			}
			if (mode == 7 && y == 2) {
				be_sti(4, FA(x), pc + 2 + (uae_s32)(uae_s16)code_word(c, pc, 2));
				return 4;
			}
		}
		return 0;

	case 0x5: {
		int q = x ? x : 8;
		/* DBcc Dn,disp */
		if ((op & 0xf0f8) == 0x50c8) {
			int cc = (op >> 8) & 15;
			uae_s16 disp = code_word(c, pc, 2);
			uae_u8 *skip = NULL, *expired;
			if (disp & 1)
				return 0;
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			if (cc == 0)
				return 4;   /* DBT: never branches */
			if (cc != 1)
				skip = be_jcc68(cc, 1);
			expired = be_dbcc_dec(FD(y));
			exit_to(c, pc + 2 + (uae_s32)disp);
			if (skip)
				be_bind(skip);
			be_bind(expired);
			return 4;
		}
		/* ADDQ, SUBQ */
		if (((op >> 6) & 3) != 3 && (sz = size_of((op >> 6) & 3)) != 0) {
			int aop = (op & 0x100) ? OP_SUB : OP_ADD;
			if (mode == 0) {
				be_ld(sz, V0, FD(y));
				be_alui(sz, aop, V0, q, FL_ARITH_X);
				be_st(sz, FD(y), V0);
				return 2;
			}
			if (mode == 1 && sz != 1) {
				be_field_addi(FA(y), aop == OP_SUB ? -q : q);
				return 2;
			}
		}
		return 0;
	}

	case 0x6: {
		/* Bcc, BRA */
		int cc = (op >> 8) & 15;
		uae_s32 disp = (uae_s8)op;
		int len = 2;
		uae_u8 *skip = NULL;
		if (cc == 1)
			return 0;   /* BSR */
		if (disp == 0) {
			disp = (uae_s16)code_word(c, pc, 2);
			len = 4;
		} else if (disp == -1) {
			disp = (uae_s32)code_long(c, pc, 2);
			len = 6;
		}
		if (disp & 1)
			return 0;
		be_add_cycles(in->cycles);
		c->cycles_added = true;
		if (in->len && in->len != len && pc + 2 + disp == pc + in->len) {
			/* taken when recorded, to the next instruction of the block:
			 * the block goes on when it is taken, and is left when not */
			if (cc != 0) {
				skip = be_jcc68(cc, 1);
				exit_to(c, pc + len);
				be_bind(skip);
			}
			return in->len;
		}
		if (cc != 0)
			skip = be_jcc68(cc, 0);
		exit_to(c, pc + 2 + disp);
		if (skip)
			be_bind(skip);
		else
			c->ended = true;
		return len;
	}

	case 0x7:
		/* MOVEQ */
		if (!(op & 0x100)) {
			uae_s32 v = (uae_s8)op;
			be_sti(4, FD(x), (uae_u32)v);
			be_set_nz(v < 0, v == 0);
			return 2;
		}
		return 0;

	case 0x8: case 0x9: case 0xb: case 0xc: case 0xd: {
		int opmode = (op >> 6) & 7;
		int aop, fl;
		switch (op >> 12) {
		case 0x8: aop = OP_OR; fl = FL_LOGIC; break;
		case 0x9: aop = OP_SUB; fl = FL_ARITH_X; break;
		case 0xb:
			if (opmode >= 4 && opmode <= 6) {
				aop = OP_XOR; fl = FL_LOGIC;
			} else {
				aop = OP_CMP; fl = FL_ARITH;
			}
			break;
		case 0xc: aop = OP_AND; fl = FL_LOGIC; break;
		default: aop = OP_ADD; fl = FL_ARITH_X; break;
		}
		/* EXG */
		if ((op & 0xf130) == 0xc100 && (opmode == 5 || opmode == 6) && mode <= 1) {
			int r1, r2;
			if ((op & 0x1f8) == 0x140) { r1 = FD(x); r2 = FD(y); }
			else if ((op & 0x1f8) == 0x148) { r1 = FA(x); r2 = FA(y); }
			else if ((op & 0x1f8) == 0x188) { r1 = FD(x); r2 = FA(y); }
			else return 0;
			be_ld(4, V0, r1);
			be_ld(4, V1, r2);
			be_st(4, r1, V1);
			be_st(4, r2, V0);
			return 2;
		}
		/* op <ea>,Dn with a register or an immediate */
		if (opmode < 3) {
			int src;
			sz = size_of(opmode);
			if (mode == 7 && y == 4) {
				uae_u32 iv = sz == 4 ? code_long(c, pc, 2) : sz == 2 ? code_word(c, pc, 2) : (code_word(c, pc, 2) & 0xff);
				be_ld(sz, V0, FD(x));
				be_alui(sz, aop, V0, iv, fl);
				if (aop != OP_CMP)
					be_st(sz, FD(x), V0);
				return sz == 4 ? 6 : 4;
			}
			if (mode == 0)
				src = FD(y);
			else if (mode == 1 && sz != 1 && (aop == OP_ADD || aop == OP_SUB || aop == OP_CMP))
				src = FA(y);
			else
				return 0;
			be_ld(sz, V0, FD(x));
			be_alu_field(sz, aop, V0, src, fl);
			if (aop != OP_CMP)
				be_st(sz, FD(x), V0);
			return 2;
		}
		/* EOR Dn,Dn */
		if (aop == OP_XOR && mode == 0) {
			sz = size_of(opmode - 4);
			be_ld(sz, V0, FD(y));
			be_alu_field(sz, OP_XOR, V0, FD(x), FL_LOGIC);
			be_st(sz, FD(y), V0);
			return 2;
		}
		/* ADDA, SUBA, CMPA */
		if ((opmode == 3 || opmode == 7) && (aop == OP_ADD || aop == OP_SUB || aop == OP_CMP) && mode <= 1) {
			int src = mode == 0 ? FD(y) : FA(y);
			be_ldsx(opmode == 3 ? 2 : 4, V1, src);
			be_ld(4, V0, FA(x));
			be_alu(4, aop, V0, V1, aop == OP_CMP ? FL_ARITH : FL_NONE);
			if (aop != OP_CMP)
				be_st(4, FA(x), V0);
			return 2;
		}
		if ((opmode == 3 || opmode == 7) && (aop == OP_ADD || aop == OP_SUB || aop == OP_CMP) && mode == 7 && y == 4) {
			uae_u32 iv = opmode == 7 ? code_long(c, pc, 2) : (uae_u32)(uae_s32)(uae_s16)code_word(c, pc, 2);
			be_ld(4, V0, FA(x));
			be_alui(4, aop, V0, iv, aop == OP_CMP ? FL_ARITH : FL_NONE);
			if (aop != OP_CMP)
				be_st(4, FA(x), V0);
			return opmode == 7 ? 6 : 4;
		}
		return 0;
	}

	case 0xe: {
		/* shifts and rotates of a data register by an immediate count */
		int kind = (op >> 3) & 3, left = (op >> 8) & 1, n = x ? x : 8;
		sz = size_of((op >> 6) & 3);
		/* LSL, LSR, ASL, ASR of a data register by a count in one */
		if (sz && (op & 0x20) && kind <= 1) {
			be_shift_reg(sz, kind, left, FD(x), FD(y));
			return 2;
		}
		if (!sz || (op & 0x20))
			return 0;
		if (kind == 2) {
			/* ROXL, ROXR */
			be_rox(sz, left, n, FD(y));
			return 2;
		}
		if (n >= sz * 8)
			return 0;
		be_ld(sz, V0, FD(y));
		be_shift(sz, kind, left, V0, n, FD(y));
		return 2;
	}
	}
	return 0;
}

/* Memory operands */

enum { OPK_D, OPK_A, OPK_IMM, OPK_MEM };

struct opnd {
	int kind;
	int reg;            /* D or A register, the An of a memory operand */
	int mode;           /* 68k addressing mode, 7 for absolute and PC */
	uae_u32 abs;        /* immediate value, absolute or PC based address */
	uae_s32 disp;
	int index;          /* extension word's index (register, size, scale), or -1 */
	bool update;        /* (An)+ or -(An) */
	int step;           /* their size */
	/* the full extension word's: */
	bool no_base;       /* An suppressed (mode 6) */
	bool indirect;      /* the address is read from memory */
	bool post;          /* the index is added after that */
	uae_s32 outer;      /* and the outer displacement */
};

/* The index and displacements of a brief or full extension word dp at
 * pc + off (as get_disp_ea_020 takes them); returns their length */
static int decode_index(struct ctx *c, uaecptr pc, int off, uae_u16 dp, uae_s32 *bd, struct opnd *o)
{
	int n = 2;

	if (!(dp & 0x100)) {
		*bd = (uae_s8)dp;
		o->index = dp & 0xfe00;
		return 2;
	}
	o->no_base = (dp & 0x80) != 0;
	o->index = (dp & 0x40) ? -1 : (dp & 0xfe00);
	*bd = 0;
	if ((dp & 0x30) == 0x20) {
		*bd = (uae_s16)code_word(c, pc, off + n);
		n += 2;
	} else if ((dp & 0x30) == 0x30) {
		*bd = (uae_s32)code_long(c, pc, off + n);
		n += 4;
	}
	if ((dp & 3) == 2) {
		o->outer = (uae_s16)code_word(c, pc, off + n);
		n += 2;
	} else if ((dp & 3) == 3) {
		o->outer = (uae_s32)code_long(c, pc, off + n);
		n += 4;
	}
	o->indirect = (dp & 3) != 0;
	o->post = (dp & 4) != 0;
	return n;
}

/* Decodes an effective address whose extension words start at pc + off;
 * returns their length, or -1 if it is not one that is translated */
static int decode_ea(struct ctx *c, int mode, int reg, int sz, uaecptr pc, int off, struct opnd *o)
{
	o->kind = OPK_MEM;
	o->reg = reg;
	o->mode = mode;
	o->disp = 0;
	o->index = -1;
	o->update = false;
	o->step = 0;
	o->no_base = o->indirect = o->post = false;
	o->outer = 0;
	switch (mode) {
	case 0:
		o->kind = OPK_D;
		return 0;
	case 1:
		o->kind = OPK_A;
		return 0;
	case 2:
		return 0;
	case 3: case 4:
		o->update = true;
		o->step = (sz == 1 && reg == 7) ? 2 : sz;
		return 0;
	case 5:
		o->disp = (uae_s16)code_word(c, pc, off);
		return 2;
	case 6:
		return decode_index(c, pc, off, code_word(c, pc, off), &o->disp, o);
	case 7:
		switch (reg) {
		case 0:
			o->abs = (uae_s32)(uae_s16)code_word(c, pc, off);
			return 2;
		case 1:
			o->abs = code_long(c, pc, off);
			return 4;
		case 2:
			o->abs = pc + off + (uae_s32)(uae_s16)code_word(c, pc, off);
			return 2;
		case 3: {
			uae_s32 bd;
			int n = decode_index(c, pc, off, code_word(c, pc, off), &bd, o);
			o->abs = (o->no_base ? 0 : pc + off) + bd;
			return n;
		}
		case 4:
			o->kind = OPK_IMM;
			if (sz == 4) {
				o->abs = code_long(c, pc, off);
				return 4;
			}
			o->abs = code_word(c, pc, off);
			if (sz == 1)
				o->abs &= 0xff;
			return 2;
		}
	}
	return -1;
}

/* a memory operand that can be written */
static bool writable(const struct opnd *o)
{
	return o->kind == OPK_MEM && !(o->mode == 7 && o->reg >= 2);
}

/* does b use the address register that a changes? */
static bool aliased(const struct opnd *a, const struct opnd *b)
{
	if (!a->update)
		return false;
	if (b->kind == OPK_A && b->reg == a->reg)
		return true;
	if (b->kind == OPK_MEM && b->mode >= 2 && b->mode <= 6 && b->reg == a->reg)
		return true;
	if (b->index >= 0 && ((b->index >> 12) & 15) == 8 + a->reg)
		return true;
	return false;
}

/* VA += the index register, scaled */
static void emit_index(int index)
{
	int r = (index >> 12) & 15;

	if (index & 0x800)
		be_ld(4, VI, r);
	else
		be_ldsx(2, VI, r);
	if ((index >> 9) & 3)
		be_shli(VI, (index >> 9) & 3);
	be_add(VA, VI);
}

/* the operand's address into VA (uses VI); a memory indirect one is read,
 * checked as any other access */
static void emit_addr(struct ctx *c, const struct opnd *o)
{
	if (o->mode == 7) {
		be_movi(VA, o->abs);
	} else if (o->no_base) {
		be_movi(VA, o->disp);
	} else {
		be_ld(4, VA, FA(o->reg));
		if (o->mode == 4)
			be_addi(VA, -o->step);
		else
			be_addi(VA, o->disp);
	}
	if (o->index >= 0 && !(o->indirect && o->post))
		emit_index(o->index);
	if (o->indirect) {
		be_tlb_check(c, PT_TLB_READ, 4);
		be_host_load(4, VA);
		if (o->index >= 0 && o->post)
			emit_index(o->index);
		be_addi(VA, o->outer);
	}
}

/* (An)+ and -(An), once the instruction can no longer fail */
static void emit_update(const struct opnd *o)
{
	if (o->update)
		be_field_addi(FA(o->reg), o->mode == 3 ? o->step : -o->step);
}

/* the value of a source operand into v (zero extended for memory and
 * immediates), with its access checked */
static void load_operand(struct ctx *c, const struct opnd *o, int sz, int v)
{
	switch (o->kind) {
	case OPK_D:
		be_ld(sz, v, FD(o->reg));
		break;
	case OPK_A:
		be_ld(sz, v, FA(o->reg));
		break;
	case OPK_IMM:
		be_movi(v, o->abs);
		break;
	default:
		emit_addr(c, o);
		be_tlb_check(c, PT_TLB_READ, sz);
		be_host_load(sz, v);
		break;
	}
}

/* the operation and flags of the opcodes 8, 9, B (CMP, and EOR for Dn,<ea>),
 * C and D */
static int alu_kind(uae_u16 op, int *aop, int *fl)
{
	switch (op >> 12) {
	case 0x8: *aop = OP_OR; *fl = FL_LOGIC; break;
	case 0x9: *aop = OP_SUB; *fl = FL_ARITH_X; break;
	case 0xb: *aop = OP_CMP; *fl = FL_ARITH; break;
	case 0xc: *aop = OP_AND; *fl = FL_LOGIC; break;
	case 0xd: *aop = OP_ADD; *fl = FL_ARITH_X; break;
	default: return 0;
	}
	return 1;
}

/* a push of 32 bits: VA = A7 - 4, checked */
static void push_check(struct ctx *c)
{
	be_ld(4, VA, FA(7));
	be_addi(VA, -4);
	be_tlb_check(c, PT_TLB_WRITE, 4);
}

/* The instructions with memory operands that are translated, as for
 * emit_native */
static int emit_mem(struct ctx *c, struct jit040_ins *in, uaecptr pc)
{
	uae_u16 op = in->opcode;
	int x = (op >> 9) & 7, y = op & 7, mode = (op >> 3) & 7;
	struct opnd so, d;
	int n, m, sz;

	if (!mem_ok)
		return 0;

	switch (op >> 12) {
	case 0x0: {
		/* ORI, ANDI, SUBI, ADDI, EORI, CMPI #imm,<ea> */
		int kind = (op >> 9) & 7, aop, fl;
		/* BTST #n,<ea> and BTST Dn,<ea>: a byte */
		if (((op & 0xffc0) == 0x0800 || (op & 0xf1c0) == 0x0100) && mode >= 2) {
			bool fixed = (op & 0xffc0) == 0x0800;
			m = decode_ea(c, mode, y, 1, pc, fixed ? 4 : 2, &d);
			if (m < 0 || d.kind != OPK_MEM)
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_READ, 1);
			be_host_load(1, V0);
			if (fixed) {
				be_movi(V1, code_word(c, pc, 2) & 7);
			} else {
				be_ld(4, V1, FD(x));
			}
			be_btst_v(V0, V1, 7);
			emit_update(&d);
			return (fixed ? 4 : 2) + m;
		}
		if ((op & 0x100) || mode < 2 || !(sz = size_of((op >> 6) & 3)))
			return 0;
		switch (kind) {
		case 0: aop = OP_OR; fl = FL_LOGIC; break;
		case 1: aop = OP_AND; fl = FL_LOGIC; break;
		case 2: aop = OP_SUB; fl = FL_ARITH_X; break;
		case 3: aop = OP_ADD; fl = FL_ARITH_X; break;
		case 5: aop = OP_XOR; fl = FL_LOGIC; break;
		case 6: aop = OP_CMP; fl = FL_ARITH; break;
		default: return 0;
		}
		n = decode_ea(c, 7, 4, sz, pc, 2, &so);
		m = decode_ea(c, mode, y, sz, pc, 2 + n, &d);
		if (m < 0 || d.kind != OPK_MEM || (aop != OP_CMP && !writable(&d)))
			return 0;
		emit_addr(c, &d);
		be_tlb_check(c, aop == OP_CMP ? PT_TLB_READ : PT_TLB_WRITE, sz);
		be_host_load(sz, V0);
		be_alui(sz, aop, V0, so.abs, fl);
		if (aop != OP_CMP)
			be_host_store(sz, V0);
		emit_update(&d);
		return 2 + n + m;
	}

	case 0x1: case 0x2: case 0x3: {
		/* MOVE, MOVEA */
		int dmode = (op >> 6) & 7, adj = 0;
		sz = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
		n = decode_ea(c, mode, y, sz, pc, 2, &so);
		if (n < 0)
			return 0;
		m = decode_ea(c, dmode, x, sz, pc, 2 + n, &d);
		if (m < 0 || (so.kind != OPK_MEM && d.kind != OPK_MEM))
			return 0;
		if (d.kind == OPK_IMM || (d.kind == OPK_MEM && !writable(&d)))
			return 0;
		if (sz == 1 && (so.kind == OPK_A || d.kind == OPK_A))
			return 0;
		if (aliased(&so, &d) && d.kind != OPK_A) {
			/* the destination's An, as the source's (An)+ or -(An) left it */
			if (d.index >= 0 && ((d.index >> 12) & 15) == FA(so.reg))
				return 0;
			if (d.no_base || d.indirect)
				return 0;
			adj = so.mode == 3 ? so.step : -so.step;
		}
		load_operand(c, &so, sz, V0);
		if (d.kind == OPK_MEM) {
			emit_addr(c, &d);
			be_addi(VA, adj);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_host_store(sz, V0);
		}
		emit_update(&so);
		emit_update(&d);
		if (d.kind == OPK_A) {
			if (sz == 2)
				be_sext(2, V0);
			be_st(4, FA(d.reg), V0);
			return 2 + n + m;
		}
		if (d.kind == OPK_D)
			be_st(sz, FD(d.reg), V0);
		be_test(sz, V0);
		return 2 + n + m;
	}

	case 0x4:
		/* TST <ea> */
		if ((op & 0xff00) == 0x4a00 && mode >= 2 && (sz = size_of((op >> 6) & 3)) != 0) {
			n = decode_ea(c, mode, y, sz, pc, 2, &so);
			if (n < 0 || so.kind != OPK_MEM)
				return 0;
			load_operand(c, &so, sz, V0);
			emit_update(&so);
			be_test(sz, V0);
			return 2 + n;
		}
		/* CLR <ea> */
		if ((op & 0xff00) == 0x4200 && mode >= 2 && (sz = size_of((op >> 6) & 3)) != 0) {
			n = decode_ea(c, mode, y, sz, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_movi(V0, 0);
			be_host_store(sz, V0);
			emit_update(&d);
			be_set_nz(0, 1);
			return 2 + n;
		}
		/* MOVEM registers,-(An) */
		if ((op & 0xffb8) == 0x48a0) {
			uae_u16 mask = code_word(c, pc, 2);
			int msz = (op & 0x40) ? 4 : 2, count = __builtin_popcount(mask), o = 0, r;
			if (mask & (1 << (7 - y)))
				return 0;   /* An itself */
			if (count) {
				be_ld(4, VA, FA(y));
				be_addi(VA, -count * msz);
				be_tlb_check(c, PT_TLB_WRITE, count * msz);
				for (r = 0; r < 16; r++) {
					if (mask & (0x8000 >> r)) {
						be_ld(msz, V0, r);
						be_host_store_off(msz, V0, o);
						o += msz;
					}
				}
				be_st(4, FA(y), VA);
			}
			return 4;
		}
		/* MOVEM <ea>,registers */
		if ((op & 0xff80) == 0x4c80 && (mode == 2 || mode == 3 || mode == 5 || mode == 6 || (mode == 7 && y <= 3))) {
			uae_u16 mask = code_word(c, pc, 2);
			int msz = (op & 0x40) ? 4 : 2, count = __builtin_popcount(mask), o = 0, r;
			n = decode_ea(c, mode, y, msz, pc, 4, &so);
			if (n < 0)
				return 0;
			if (count) {
				emit_addr(c, &so);
				be_tlb_check(c, PT_TLB_READ, count * msz);
				for (r = 0; r < 16; r++) {
					if (mask & (1 << r)) {
						be_host_load_off(msz, V0, o);
						if (msz == 2)
							be_sext(2, V0);
						be_st(4, r, V0);
						o += msz;
					}
				}
				if (mode == 3) {
					be_addi(VA, count * msz);
					be_st(4, FA(y), VA);
				}
			}
			return 4 + n;
		}
		/* PEA <ea> */
		if ((op & 0xffc0) == 0x4840 && (mode == 2 || mode == 5 || mode == 6 || (mode == 7 && y <= 3))) {
			n = decode_ea(c, mode, y, 4, pc, 2, &so);
			if (n < 0)
				return 0;
			emit_addr(c, &so);
			be_mov(V0, VA);
			push_check(c);
			be_host_store(4, V0);
			be_field_addi(FA(7), -4);
			return 2 + n;
		}
		/* LINK An,#d16 */
		if ((op & 0xfff8) == 0x4e50 && y != 7) {
			uae_s32 disp = (uae_s16)code_word(c, pc, 2);
			push_check(c);
			be_ld(4, V0, FA(y));
			be_host_store(4, V0);
			be_st(4, FA(y), VA);                   /* An = A7 - 4 */
			be_addi(VA, disp);
			be_st(4, FA(7), VA);
			return 4;
		}
		/* UNLK An */
		if ((op & 0xfff8) == 0x4e58) {
			be_ld(4, VA, FA(y));
			be_tlb_check(c, PT_TLB_READ, 4);
			be_host_load(4, V0);
			be_ld(4, VA, FA(y));
			be_addi(VA, 4);
			be_st(4, FA(7), VA);
			be_st(4, FA(y), V0);
			return 2;
		}
		/* RTS */
		if (op == 0x4e75) {
			be_ld(4, VA, FA(7));
			be_tlb_check(c, PT_TLB_READ, 4);
			be_host_load(4, V0);
			be_slow_if_odd(c, V0);                 /* the handler's exception */
			be_field_addi(FA(7), 4);
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			return jump_to_reg(c, in, pc, 2, V0);
		}
		/* JSR, JMP <ea> */
		if (((op & 0xffc0) == 0x4e80 || (op & 0xffc0) == 0x4ec0) &&
		    (mode == 2 || mode == 5 || mode == 6 || (mode == 7 && y <= 3))) {
			bool jsr = (op & 0xffc0) == 0x4e80;
			n = decode_ea(c, mode, y, 4, pc, 2, &so);
			if (n < 0)
				return 0;
			emit_addr(c, &so);
			be_mov(VJ, VA);
			be_slow_if_odd(c, VJ);
			if (jsr) {
				push_check(c);
				be_movi(V0, pc + 2 + n);
				be_host_store(4, V0);
				be_field_addi(FA(7), -4);
			}
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			return jump_to_reg(c, in, pc, 2 + n, VJ);
		}
		return 0;

	case 0x5:
		/* ADDQ, SUBQ #q,<ea> */
		if (((op >> 6) & 3) != 3 && mode >= 2 && (sz = size_of((op >> 6) & 3)) != 0) {
			int q = x ? x : 8;
			n = decode_ea(c, mode, y, sz, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_host_load(sz, V0);
			be_alui(sz, (op & 0x100) ? OP_SUB : OP_ADD, V0, q, FL_ARITH_X);
			be_host_store(sz, V0);
			emit_update(&d);
			return 2 + n;
		}
		return 0;

	case 0x6: {
		/* BSR */
		int cc = (op >> 8) & 15;
		uae_s32 disp = (uae_s8)op;
		int len = 2;
		if (cc != 1)
			return 0;
		if (disp == 0) {
			disp = (uae_s16)code_word(c, pc, 2);
			len = 4;
		} else if (disp == -1) {
			disp = (uae_s32)code_long(c, pc, 2);
			len = 6;
		}
		if (disp & 1)
			return 0;
		push_check(c);
		be_movi(V0, pc + len);
		be_host_store(4, V0);
		be_field_addi(FA(7), -4);
		if (in->len && in->len != len && pc + 2 + disp == pc + in->len) {
			/* to a little further on in the block, as when recorded */
			return in->len;
		}
		be_add_cycles(in->cycles);
		c->cycles_added = true;
		exit_to(c, pc + 2 + disp);
		c->ended = true;
		return len;
	}

	case 0x8: case 0x9: case 0xb: case 0xc: case 0xd: {
		int opmode = (op >> 6) & 7, aop, fl;
		if (!alu_kind(op, &aop, &fl))
			return 0;
		/* CMPM (Ay)+,(Ax)+ */
		if ((op >> 12) == 0xb && opmode >= 4 && opmode <= 6 && mode == 1) {
			int sy, sx;
			sz = size_of(opmode - 4);
			sy = (sz == 1 && y == 7) ? 2 : sz;
			sx = (sz == 1 && x == 7) ? 2 : sz;
			be_ld(4, VA, FA(y));
			be_tlb_check(c, PT_TLB_READ, sz);
			be_host_load(sz, V0);
			be_ld(4, VA, FA(x));
			if (x == y)
				be_addi(VA, sy);
			be_tlb_check(c, PT_TLB_READ, sz);
			be_host_load(sz, V1);
			be_field_addi(FA(y), sy);
			be_field_addi(FA(x), sx);
			be_alu(sz, OP_CMP, V1, V0, FL_ARITH);
			return 2;
		}
		/* MULU.W, MULS.W <ea>,Dn */
		if ((op >> 12) == 0xc && (opmode == 3 || opmode == 7)) {
			n = decode_ea(c, mode, y, 2, pc, 2, &so);
			if (n < 0 || so.kind == OPK_A)
				return 0;
			load_operand(c, &so, 2, V1);
			emit_update(&so);
			be_ld(2, V0, FD(x));
			be_mul16(opmode == 7, V0, V1);
			be_st(4, FD(x), V0);
			return 2 + n;
		}
		if (opmode < 3) {
			/* op <ea>,Dn */
			sz = size_of(opmode);
			n = decode_ea(c, mode, y, sz, pc, 2, &so);
			if (n < 0 || so.kind != OPK_MEM)
				return 0;
			load_operand(c, &so, sz, V0);
			emit_update(&so);
			be_ld(sz, V1, FD(x));
			be_alu(sz, aop, V1, V0, fl);
			if (aop != OP_CMP)
				be_st(sz, FD(x), V1);
			return 2 + n;
		}
		if ((opmode == 3 || opmode == 7) && (aop == OP_ADD || aop == OP_SUB || aop == OP_CMP)) {
			/* ADDA, SUBA, CMPA <ea>,An */
			sz = opmode == 3 ? 2 : 4;
			n = decode_ea(c, mode, y, sz, pc, 2, &so);
			if (n < 0 || so.kind != OPK_MEM)
				return 0;
			load_operand(c, &so, sz, V0);
			if (sz == 2)
				be_sext(2, V0);
			emit_update(&so);
			be_ld(4, V1, FA(x));
			be_alu(4, aop, V1, V0, aop == OP_CMP ? FL_ARITH : FL_NONE);
			if (aop != OP_CMP)
				be_st(4, FA(x), V1);
			return 2 + n;
		}
		if (opmode >= 4 && opmode <= 6 && mode >= 2) {
			/* op Dn,<ea> (EOR for CMP's opcodes) */
			if (aop == OP_CMP) {
				aop = OP_XOR;
				fl = FL_LOGIC;
			}
			sz = size_of(opmode - 4);
			n = decode_ea(c, mode, y, sz, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_host_load(sz, V0);
			be_alu_field(sz, aop, V0, FD(x), fl);
			be_host_store(sz, V0);
			emit_update(&d);
			return 2 + n;
		}
		return 0;
	}
	}
	return 0;
}

/* The helpers: C functions called from the native code (be_ccall) for the
 * operations that are long to write out, doing what the instructions'
 * handlers do. Those that may need the handler (for an exception) change
 * nothing and return non-zero then. */

static uae_u32 jh_make_sr(uae_u32 a, uae_u32 b, uae_u32 cc, uae_u8 *host)
{
	MakeSR();
	return regs.sr;
}

static uae_u32 jh_divu(uae_u32 dn, uae_u32 s, uae_u32 cc, uae_u8 *host)
{
	uae_u32 src = s & 0xffff, dst = m68k_dreg(regs, dn), newv, rem;

	if (src == 0)
		return 1;
	newv = dst / src;
	rem = dst % src;
	if (newv > 0xffff) {
		setdivuflags(dst, src);
	} else {
		CLEAR_CZNV();
		SET_ZFLG(((uae_s16)newv) == 0);
		SET_NFLG(((uae_s16)newv) < 0);
		m68k_dreg(regs, dn) = (newv & 0xffff) | (rem << 16);
	}
	return 0;
}

static uae_u32 jh_divs(uae_u32 dn, uae_u32 s, uae_u32 cc, uae_u8 *host)
{
	uae_s16 src = s;
	uae_s32 dst = m68k_dreg(regs, dn);

	if (src == 0)
		return 1;
	if (dst == (uae_s32)0x80000000 && src == -1) {
		setdivsflags(dst, src);
	} else {
		uae_s32 newv = dst / (uae_s32)src;
		uae_u16 rem = dst % (uae_s32)src;
		if ((newv & 0xffff8000) != 0 && (newv & 0xffff8000) != 0xffff8000) {
			setdivsflags(dst, src);
		} else {
			if (((uae_s16)rem < 0) != (dst < 0))
				rem = -rem;
			CLEAR_CZNV();
			SET_ZFLG(((uae_s16)newv) == 0);
			SET_NFLG(((uae_s16)newv) < 0);
			m68k_dreg(regs, dn) = (newv & 0xffff) | ((uae_u32)rem << 16);
		}
	}
	return 0;
}

static uae_u32 jh_divl(uae_u32 extra, uae_u32 src, uae_u32 cc, uae_u8 *host)
{
	if (src == 0)
		return 1;
	m68k_divl(0x4c40, src, extra, 0);
	return 0;
}

/* ABCD, SBCD: dst and src; NBCD: src */
static uae_u32 jh_abcd(uae_u32 d, uae_u32 s, uae_u32 cc, uae_u8 *host)
{
	uae_s8 src = s, dst = d;
	uae_u16 newv_lo = (src & 0xF) + (dst & 0xF) + (GET_XFLG() ? 1 : 0);
	uae_u16 newv_hi = (src & 0xF0) + (dst & 0xF0);
	uae_u16 newv = newv_hi + newv_lo;
	int cflg;

	if (newv_lo > 9)
		newv += 6;
	cflg = (newv & 0x3F0) > 0x90;
	if (cflg)
		newv += 0x60;
	SET_CFLG(cflg);
	COPY_CARRY();
	SET_ZFLG(GET_ZFLG() & (((uae_s8)(newv)) == 0));
	return newv & 0xff;
}

static uae_u32 jh_sbcd(uae_u32 d, uae_u32 s, uae_u32 cc, uae_u8 *host)
{
	uae_s8 src = s, dst = d;
	uae_u16 newv_lo = (dst & 0xF) - (src & 0xF) - (GET_XFLG() ? 1 : 0);
	uae_u16 newv_hi = (dst & 0xF0) - (src & 0xF0);
	uae_u16 newv = newv_hi + newv_lo;
	int bcd = 0;

	if (newv_lo & 0xF0) {
		newv -= 6;
		bcd = 6;
	}
	if ((((dst & 0xFF) - (src & 0xFF) - (GET_XFLG() ? 1 : 0)) & 0x100) > 0xFF)
		newv -= 0x60;
	SET_CFLG((((dst & 0xFF) - (src & 0xFF) - bcd - (GET_XFLG() ? 1 : 0)) & 0x300) > 0xFF);
	COPY_CARRY();
	SET_ZFLG(GET_ZFLG() & (((uae_s8)(newv)) == 0));
	return newv & 0xff;
}

static uae_u32 jh_nbcd(uae_u32 s, uae_u32 b, uae_u32 cc, uae_u8 *host)
{
	uae_s8 src = s;
	uae_u16 newv_lo = - (src & 0xF) - (GET_XFLG() ? 1 : 0);
	uae_u16 newv_hi = - (src & 0xF0);
	uae_u16 newv;
	int cflg;

	if (newv_lo > 9)
		newv_lo -= 6;
	newv = newv_hi + newv_lo;
	cflg = (newv & 0x1F0) > 0x90;
	if (cflg)
		newv -= 0x60;
	SET_CFLG(cflg);
	COPY_CARRY();
	SET_ZFLG(GET_ZFLG() & (((uae_s8)(newv)) == 0));
	return newv & 0xff;
}

/* CHK2, CMP2: the bounds at host, extension word and size in pack */
static uae_u32 jh_chk2(uae_u32 pack, uae_u32 b, uae_u32 cc, uae_u8 *host)
{
	uae_u16 extra = pack;
	int sz = pack >> 16;
	uae_s32 lower, upper, reg = regs.regs[(extra >> 12) & 15];
	bool z, carry = false;

	if (sz == 1) {
		lower = (uae_s8)host[0];
		upper = (uae_s8)host[1];
		if (!(extra & 0x8000))
			reg = (uae_s8)reg;
	} else if (sz == 2) {
		lower = (uae_s16)do_get_mem_word(host);
		upper = (uae_s16)do_get_mem_word(host + 2);
		if (!(extra & 0x8000))
			reg = (uae_s16)reg;
	} else {
		lower = do_get_mem_long(host);
		upper = do_get_mem_long(host + 4);
	}
	z = upper == reg || lower == reg;
	if (!z) {
		if (lower <= upper && (reg < lower || reg > upper))
			carry = true;
		if (lower > upper && reg > upper && reg < lower)
			carry = true;
	}
	if ((extra & 0x800) && carry)
		return 1;   /* CHK2's exception */
	SET_ZFLG(z);
	SET_CFLG(carry);
	return 0;
}

/* BFTST, BFEXTU, BFCHG, BFEXTS, BFCLR, BFFFO, BFSET, BFINS: opcode and
 * extension word in pack; the field at host (from the byte offset / 8)
 * unless it is in Dn */
static uae_u32 jh_bitfield(uae_u32 pack, uae_u32 b, uae_u32 cc, uae_u8 *host)
{
	uae_u16 op = pack >> 16, extra = pack;
	int kind = (op >> 8) & 7, dreg = op & 7;
	bool in_reg = (op & 0x38) == 0;
	uae_s32 offset = extra & 0x800 ? m68k_dreg(regs, (extra >> 6) & 7) : (extra >> 6) & 0x1f;
	int width = (((extra & 0x20 ? m68k_dreg(regs, extra & 7) : extra) - 1) & 0x1f) + 1;
	uae_u32 offset2 = offset, tmp, bdata[2] = { 0, 0 }, mask;

	if (in_reg) {
		tmp = m68k_dreg(regs, dreg);
		offset &= 0x1f;
		if (offset)
			tmp = (tmp << offset) | (tmp >> (32 - offset));
		bdata[0] = tmp & ((1u << (32 - width)) - 1);
	} else {
		tmp = jit040_bf_get(host, bdata, offset, width);
	}
	SET_NFLG(((uae_s32)tmp) < 0 ? 1 : 0);
	if (kind == 3)
		tmp = (uae_s32)tmp >> (32 - width);
	else
		tmp >>= (32 - width);
	SET_ZFLG(tmp == 0);
	SET_VFLG(0);
	SET_CFLG(0);
	switch (kind) {
	case 0:                                   /* BFTST */
		return 0;
	case 1: case 3:                           /* BFEXTU, BFEXTS */
		m68k_dreg(regs, (extra >> 12) & 7) = tmp;
		return 0;
	case 5:                                   /* BFFFO */
		for (mask = 1u << (width - 1); mask; mask >>= 1) {
			if (tmp & mask)
				break;
			offset2++;
		}
		m68k_dreg(regs, (extra >> 12) & 7) = offset2;
		return 0;
	case 2:                                   /* BFCHG */
		tmp ^= 0xffffffffu >> (32 - width);
		break;
	case 4:                                   /* BFCLR */
		tmp = 0;
		break;
	case 6:                                   /* BFSET */
		tmp = 0xffffffffu >> (32 - width);
		break;
	default:                                  /* BFINS */
		tmp = m68k_dreg(regs, (extra >> 12) & 7) & (0xffffffffu >> (32 - width));
		SET_NFLG(tmp & (1u << (width - 1)) ? 1 : 0);
		SET_ZFLG(tmp == 0);
		break;
	}
	if (in_reg) {
		tmp = bdata[0] | (tmp << (32 - width));
		m68k_dreg(regs, dreg) = offset ? ((tmp >> offset) | (tmp << (32 - offset))) : tmp;
	} else {
		jit040_bf_put(host, bdata, tmp, offset, width);
	}
	return 0;
}

/* ROXL, ROXR of Dy by the count in Dx (the opcode in op) */
static uae_u32 jh_roxreg(uae_u32 op, uae_u32 b, uae_u32 cc, uae_u8 *host)
{
	int n = size_of((op >> 6) & 3) * 8, left = (op >> 8) & 1, cnt;
	uae_u32 mask = n == 32 ? 0xffffffffu : (1u << n) - 1;
	uae_u32 val = m68k_dreg(regs, op & 7) & mask, carry;

	cnt = (m68k_dreg(regs, (op >> 9) & 7) & 63) % (n + 1);
	CLEAR_CZNV();
	if (cnt > 0) {
		cnt--;
		if (left) {
			uae_u32 loval = val >> (n - 1 - cnt);
			carry = loval & 1;
			val = (((val << 1) | GET_XFLG()) << cnt) | (loval >> 1);
		} else {
			uae_u32 hival = ((val << 1) | GET_XFLG()) << (n - 1 - cnt);
			val >>= cnt;
			carry = val & 1;
			val = (val >> 1) | hival;
		}
		SET_XFLG(carry);
		val &= mask;
	}
	SET_CFLG(GET_XFLG());
	SET_ZFLG(val == 0);
	SET_NFLG((val >> (n - 1)) & 1);
	m68k_dreg(regs, op & 7) = (m68k_dreg(regs, op & 7) & ~mask) | val;
	return 0;
}

/* FBcc, FScc, FTRAPcc: the condition (1 true, 0 false, -1 the handler) */
static uae_u32 jh_fcc(uae_u32 cond, uae_u32 b, uae_u32 cc, uae_u8 *host)
{
	return (uae_u32)fpp_jit_cond(cond);
}

/* FDBcc: 1 if it branches, 0 if not, -1 for the handler */
static uae_u32 jh_fdbcc(uae_u32 cond, uae_u32 dn, uae_u32 cc, uae_u8 *host)
{
	int t = fpp_jit_cond(cond);

	if (t)
		return (uae_u32)(t < 0 ? -1 : 0);
	m68k_dreg(regs, dn) = (m68k_dreg(regs, dn) & 0xffff0000) | ((m68k_dreg(regs, dn) - 1) & 0xffff);
	return (m68k_dreg(regs, dn) & 0xffff) != 0xffff;
}

/* The operands of ADDX, SUBX, ABCD, SBCD -(Ay),-(Ax): the source into V1, the
 * destination into V0, its host address there for the store */
static void xmem_operands(struct ctx *c, int sz, int x, int y)
{
	int sy = (sz == 1 && y == 7) ? 2 : sz, sx = (sz == 1 && x == 7) ? 2 : sz;

	be_ld(4, VA, FA(y));
	be_addi(VA, -sy);
	be_tlb_check(c, PT_TLB_READ, sz);
	be_host_load(sz, V1);
	be_ld(4, VA, FA(x));
	be_addi(VA, -(sx + (x == y ? sy : 0)));
	be_tlb_check(c, PT_TLB_WRITE, sz);
	be_host_load(sz, V0);
}
static void xmem_update(int sz, int x, int y)
{
	be_field_addi(FA(y), -((sz == 1 && y == 7) ? 2 : sz));
	be_field_addi(FA(x), -((sz == 1 && x == 7) ? 2 : sz));
}

/* ABCD, SBCD Dy,Dx and -(Ay),-(Ax) */
static int emit_bcd(struct ctx *c, uae_u16 op, jit040_helper fn)
{
	int x = (op >> 9) & 7, y = op & 7;

	if (!(op & 8)) {
		be_ld(1, V0, FD(x));
		be_ld(1, V1, FD(y));
		be_ccall(fn, false);
		be_st(1, FD(x), V0);
		return 2;
	}
	if (!mem_ok)
		return 0;
	xmem_operands(c, 1, x, y);
	be_ccall(fn, false);
	be_host_store(1, V0);
	xmem_update(1, x, y);
	return 2;
}

/* the value of PACK (from a word) or UNPK (from a byte) in V0 */
static void emit_pack(bool pack, uae_u32 adj)
{
	if (pack) {
		be_addi(V0, adj);
		be_mov(V1, V0);
		be_shri(V1, 4);
		be_alui(4, OP_AND, V1, 0xf0, FL_NONE);
	} else {
		be_mov(V1, V0);
		be_shli(V1, 4);
		be_alui(4, OP_AND, V1, 0xf00, FL_NONE);
	}
	be_alui(4, OP_AND, V0, 0xf, FL_NONE);
	be_alu(4, OP_OR, V0, V1, FL_NONE);
	if (!pack)
		be_addi(V0, adj);
}

/* The FPU's general instructions (F-line, coprocessor 1, type 0) that the
 * 68040 does itself: the arithmetic of opclass 000 and 010, FMOVE to <ea>,
 * FMOVEM of a static list and FMOVE of one control register. The operand is
 * reached here, through the page cache, and fpp.c's fpp_jit_* do the rest as
 * the handler does; when one of them needs the handler (an exception, a
 * denormal) it has changed nothing. */

/* the opmodes the 68040's FPU does itself */
static bool fpu_hw_opmode(int v)
{
	switch (v) {
	case 0x00: case 0x04: case 0x18: case 0x1a: case 0x20: case 0x22: case 0x23: case 0x24:
	case 0x27: case 0x28: case 0x38: case 0x3a: case 0x40: case 0x41: case 0x44: case 0x45:
	case 0x58: case 0x5a: case 0x5c: case 0x5e: case 0x60: case 0x62: case 0x63: case 0x64:
	case 0x66: case 0x67: case 0x68: case 0x6c:
		return true;
	}
	return false;
}

/* the call, and the handler if it says so */
static void fpu_call(struct ctx *c, jit040_helper fn, uae_u32 opextra, uae_u32 arg, bool regs_written)
{
	be_movi(V0, opextra);
	be_movi(V1, arg);
	be_ccall(fn, regs_written);
	be_slow_cmp(c, 4, SC_NE, V0, -1);
}

static int emit_fpu(struct ctx *c, struct jit040_ins *in, uaecptr pc)
{
	/* the operand sizes of the formats L, S, X, P, W, D, B */
	static const int fsize[8] = { 4, 4, 12, 0, 2, 8, 1, 0 };
	uae_u16 op = in->opcode, extra = code_word(c, pc, 2);
	int mode = (op >> 3) & 7, y = op & 7, size = (extra >> 10) & 7, n = 0;
	uae_u32 opextra = ((uae_u32)op << 16) | extra;
	struct opnd o;

	if (!mem_ok)
		return 0;
	switch (extra >> 13) {
	case 0:
		/* FPm to FPn */
		if ((op & 0x3f) || !fpu_hw_opmode(extra & 0x7f))
			return 0;
		fpu_call(c, fpp_jit_arith, opextra, pc, false);
		return 4;

	case 2:
		/* <ea> to FPn */
		if (!fpu_hw_opmode(extra & 0x7f) || !fsize[size] || mode == 1)
			return 0;
		if (mode == 0) {
			if (size != 0 && size != 1 && size != 4 && size != 6)
				return 0;
			fpu_call(c, fpp_jit_arith, opextra, pc, false);
			return 4;
		}
		if (mode == 7 && y == 4) {
			/* an immediate, read where it is (a byte is the low one of a word) */
			n = size == 6 ? 2 : fsize[size];
			be_set_host(c->b->host_page + ((pc + 4 + (size == 6)) & 0xfff));
			fpu_call(c, fpp_jit_arith, opextra, pc, false);
			return 4 + n;
		}
		n = decode_ea(c, mode, y, fsize[size], pc, 4, &o);
		if (n < 0 || o.kind != OPK_MEM)
			return 0;
		emit_addr(c, &o);
		be_tlb_check(c, PT_TLB_READ, fsize[size]);
		fpu_call(c, fpp_jit_arith, opextra, pc, false);
		emit_update(&o);
		return 4 + n;

	case 3:
		/* FMOVE FPn to <ea> (not packed) */
		if (!fsize[size] || mode == 1)
			return 0;
		if (mode == 0) {
			if (size != 0 && size != 1 && size != 4 && size != 6)
				return 0;
			fpu_call(c, fpp_jit_fmove_out, opextra, pc, true);
			return 4;
		}
		n = decode_ea(c, mode, y, fsize[size], pc, 4, &o);
		if (n < 0 || !writable(&o))
			return 0;
		emit_addr(c, &o);
		be_tlb_check(c, PT_TLB_WRITE, fsize[size]);
		fpu_call(c, fpp_jit_fmove_out, opextra, pc, false);
		emit_update(&o);
		return 4 + n;

	case 4: case 5: {
		/* FMOVE of one control register (FPCR, FPSR, FPIAR) */
		uae_u16 which = extra & 0x1c00;
		bool to_ea = (extra & 0x2000) != 0;
		if (which != 0x1000 && which != 0x0800 && which != 0x0400)
			return 0;
		if (mode <= 1) {
			if (mode == 1 && which != 0x0400)
				return 0;
			fpu_call(c, fpp_jit_fmove_cr, opextra, 0, to_ea);
			return 4;
		}
		if (mode == 7 && y == 4) {
			if (to_ea)
				return 0;
			be_set_host(c->b->host_page + ((pc + 4) & 0xfff));
			fpu_call(c, fpp_jit_fmove_cr, opextra, 0, false);
			return 8;
		}
		n = decode_ea(c, mode, y, 4, pc, 4, &o);
		if (n < 0 || o.kind != OPK_MEM || (to_ea && !writable(&o)))
			return 0;
		emit_addr(c, &o);
		be_tlb_check(c, to_ea ? PT_TLB_WRITE : PT_TLB_READ, 4);
		fpu_call(c, fpp_jit_fmove_cr, opextra, 0, false);
		emit_update(&o);
		return 4 + n;
	}

	case 6: case 7: {
		/* FMOVEM of a static list of FPn */
		bool to_mem = (extra & 0x2000) != 0;
		int count = __builtin_popcount(extra & 0xff), bytes = count * 12;
		if ((extra & 0x0800) || (extra & 0x0700) || !count)
			return 0;
		if (mode == 4) {
			if (!to_mem)
				return 0;
			be_ld(4, VA, FA(y));
			be_addi(VA, -bytes);
			be_tlb_check(c, PT_TLB_WRITE, bytes);
			fpu_call(c, fpp_jit_fmovem, opextra, count, false);
			be_field_addi(FA(y), -bytes);
			return 4;
		}
		if (mode == 3) {
			if (to_mem)
				return 0;
			be_ld(4, VA, FA(y));
			be_tlb_check(c, PT_TLB_READ, bytes);
			fpu_call(c, fpp_jit_fmovem, opextra, count, false);
			be_field_addi(FA(y), bytes);
			return 4;
		}
		if (mode < 2 || (mode == 7 && y > (to_mem ? 1 : 3)))
			return 0;
		n = decode_ea(c, mode, y, 4, pc, 4, &o);
		if (n < 0)
			return 0;
		emit_addr(c, &o);
		be_tlb_check(c, to_mem ? PT_TLB_WRITE : PT_TLB_READ, bytes);
		fpu_call(c, fpp_jit_fmovem, opextra, count, false);
		return 4 + n;
	}
	}
	return 0;
}

/* The rest of the instructions that are translated, as for emit_native:
 * the less common ones, some with a helper */
static int emit_ext(struct ctx *c, struct jit040_ins *in, uaecptr pc)
{
	uae_u16 op = in->opcode;
	int x = (op >> 9) & 7, y = op & 7, mode = (op >> 3) & 7;
	struct opnd so, d;
	int n, sz;

	switch (op >> 12) {
	case 0x0:
		/* ORI, ANDI, EORI #imm,CCR */
		if (op == 0x003c || op == 0x023c || op == 0x0a3c) {
			uae_u32 iv = code_word(c, pc, 2) & 0xff;
			be_ccall(jh_make_sr, false);
			be_alui(4, op == 0x003c ? OP_OR : op == 0x023c ? OP_AND : OP_XOR, V0, iv, FL_NONE);
			be_ccr_set(V0);
			return 4;
		}
		/* BTST Dn,#imm */
		if ((op & 0xf1ff) == 0x013c) {
			be_movi(V0, code_word(c, pc, 2) & 0xff);
			be_ld(4, V1, FD(x));
			be_btst_v(V0, V1, 7);
			return 4;
		}
		/* BCHG, BCLR, BSET Dn,Dy and #n,Dy */
		if ((op & 0xf1f8) == 0x0140 || (op & 0xf1f8) == 0x0180 || (op & 0xf1f8) == 0x01c0 ||
		    (op & 0xfff8) == 0x0840 || (op & 0xfff8) == 0x0880 || (op & 0xfff8) == 0x08c0) {
			bool fixed = !(op & 0x100);
			be_ld(4, V0, FD(y));
			if (fixed)
				be_movi(V1, code_word(c, pc, 2) & 31);
			else
				be_ld(4, V1, FD(x));
			be_btst_v(V0, V1, 31);
			be_bitop((op >> 6) & 3, V0, V1, 31);
			be_st(4, FD(y), V0);
			return fixed ? 4 : 2;
		}
		/* the same with a byte in memory */
		if (((op & 0xf1c0) == 0x0140 || (op & 0xf1c0) == 0x0180 || (op & 0xf1c0) == 0x01c0 ||
		     (op & 0xffc0) == 0x0840 || (op & 0xffc0) == 0x0880 || (op & 0xffc0) == 0x08c0) && mode >= 2 && mem_ok) {
			bool fixed = !(op & 0x100);
			n = decode_ea(c, mode, y, 1, pc, fixed ? 4 : 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 1);
			be_host_load(1, V0);
			if (fixed)
				be_movi(V1, code_word(c, pc, 2) & 7);
			else
				be_ld(4, V1, FD(x));
			be_btst_v(V0, V1, 7);
			be_bitop((op >> 6) & 3, V0, V1, 7);
			be_host_store(1, V0);
			emit_update(&d);
			return (fixed ? 4 : 2) + n;
		}
		/* MOVEP d16(Ay),Dx and Dx,d16(Ay) */
		if ((op & 0xf138) == 0x0108 && mem_ok) {
			int opm = (op >> 6) & 7, msz = (opm & 1) ? 4 : 2, i;
			bool to_mem = opm >= 6;
			be_ld(4, VA, FA(y));
			be_addi(VA, (uae_s16)code_word(c, pc, 2));
			be_tlb_check(c, to_mem ? PT_TLB_WRITE : PT_TLB_READ, msz * 2 - 1);
			if (to_mem) {
				for (i = 0; i < msz; i++) {
					be_ld(4, V0, FD(x));
					if (i < msz - 1)
						be_shri(V0, (msz - 1 - i) * 8);
					be_host_store_off(1, V0, i * 2);
				}
			} else {
				be_host_load_off(1, V0, 0);
				for (i = 1; i < msz; i++) {
					be_shli(V0, 8);
					be_host_load_off(1, V1, i * 2);
					be_alu(4, OP_OR, V0, V1, FL_NONE);
				}
				be_st(msz, FD(x), V0);
			}
			return 4;
		}
		/* CAS Dc,Du,<ea> */
		if ((op & 0xf9c0) == 0x08c0 && ((op >> 9) & 3) && mode >= 2 && !(mode == 7 && y == 4) && mem_ok) {
			uae_u16 ext = code_word(c, pc, 2);
			int dc = ext & 7, du = (ext >> 6) & 7;
			uae_u8 *ne, *done;
			sz = 1 << (((op >> 9) & 3) - 1);
			n = decode_ea(c, mode, y, sz, pc, 4, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_host_load(sz, V0);
			be_ld(sz, V1, FD(dc));
			need |= F_NZVC;
			be_alu(sz, OP_CMP, V0, V1, FL_ARITH);
			ne = be_jcc68(7, 0);
			be_ld(sz, V1, FD(du));
			be_host_store(sz, V1);
			done = be_jmp();
			be_bind(ne);
			be_st(sz, FD(dc), V0);
			be_bind(done);
			emit_update(&d);
			return 4 + n;
		}
		/* CMP2, CHK2 <ea>,Rn */
		if ((op & 0xf9c0) == 0x00c0 && ((op >> 9) & 3) != 3 && mem_ok &&
		    (mode == 2 || mode == 5 || mode == 6 || (mode == 7 && y <= 3))) {
			uae_u16 ext = code_word(c, pc, 2);
			sz = 1 << ((op >> 9) & 3);
			n = decode_ea(c, mode, y, sz, pc, 4, &so);
			if (n < 0)
				return 0;
			emit_addr(c, &so);
			be_tlb_check(c, PT_TLB_READ, sz * 2);
			be_movi(V0, ext | (sz << 16));
			be_ccall(jh_chk2, false);
			be_slow_cmp(c, 4, SC_NE, V0, -1);
			return 4 + n;
		}
		return 0;

	case 0x4:
		/* NEGX Dn and <ea> */
		if ((op & 0xff00) == 0x4000 && (sz = size_of((op >> 6) & 3)) != 0 && mode != 1) {
			if (mode == 0) {
				be_ld(sz, V1, FD(y));
				be_movi(V0, 0);
				be_addx(sz, 1, V0, V1);
				be_st(sz, FD(y), V0);
				return 2;
			}
			if (!mem_ok)
				return 0;
			n = decode_ea(c, mode, y, sz, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_host_load(sz, V1);
			be_movi(V0, 0);
			be_addx(sz, 1, V0, V1);
			be_host_store(sz, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* MOVE SR,<ea> (in supervisor mode: in user mode it is an
		 * exception) and CCR,<ea> */
		if ((((op & 0xffc0) == 0x40c0 && c->b->s) || (op & 0xffc0) == 0x42c0) && mode != 1) {
			n = decode_ea(c, mode, y, 2, pc, 2, &d);
			if (n < 0 || (d.kind != OPK_D && !writable(&d)) || (d.kind == OPK_MEM && !mem_ok))
				return 0;
			be_ccall(jh_make_sr, false);
			if ((op & 0xffc0) == 0x42c0)
				be_alui(4, OP_AND, V0, 0xff, FL_NONE);
			if (d.kind == OPK_D) {
				be_st(2, FD(d.reg), V0);
				return 2;
			}
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 2);
			be_host_store(2, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* NEG, NOT <ea> */
		if (((op & 0xff00) == 0x4400 || (op & 0xff00) == 0x4600) && mode >= 2 &&
		    (sz = size_of((op >> 6) & 3)) != 0 && mem_ok) {
			n = decode_ea(c, mode, y, sz, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, sz);
			be_host_load(sz, V0);
			if ((op & 0xff00) == 0x4400)
				be_neg(sz, V0);
			else
				be_not(sz, V0);
			be_host_store(sz, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* MOVE <ea>,CCR */
		if ((op & 0xffc0) == 0x44c0 && mode != 1) {
			n = decode_ea(c, mode, y, 2, pc, 2, &so);
			if (n < 0 || (so.kind == OPK_MEM && !mem_ok))
				return 0;
			load_operand(c, &so, 2, V0);
			emit_update(&so);
			be_ccr_set(V0);
			return 2 + n;
		}
		/* NBCD <ea> */
		if ((op & 0xffc0) == 0x4800 && mode != 1) {
			if (mode == 0) {
				be_ld(1, V0, FD(y));
				be_ccall(jh_nbcd, false);
				be_st(1, FD(y), V0);
				return 2;
			}
			if (!mem_ok)
				return 0;
			n = decode_ea(c, mode, y, 1, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 1);
			be_host_load(1, V0);
			be_ccall(jh_nbcd, false);
			be_host_store(1, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* LINK.L An,#d32, LINK A7,#d16 */
		if (((op & 0xfff8) == 0x4808 || op == 0x4e57) && mem_ok) {
			bool l = (op & 0xfff8) == 0x4808;
			uae_s32 disp = l ? (uae_s32)code_long(c, pc, 2) : (uae_s16)code_word(c, pc, 2);
			push_check(c);                            /* VA = A7 - 4 */
			if (y == 7)
				be_mov(V0, VA);
			else
				be_ld(4, V0, FA(y));
			be_host_store(4, V0);
			be_st(4, FA(y), VA);
			be_addi(VA, disp);
			be_st(4, FA(7), VA);
			return l ? 6 : 4;
		}
		/* MOVEM registers,<ea>: the control modes, and -(An) with An in the
		 * list (its value less the size is written) */
		if ((op & 0xff80) == 0x4880 && mem_ok &&
		    (mode == 2 || mode == 4 || mode == 5 || mode == 6 || (mode == 7 && y <= 1))) {
			uae_u16 mask = code_word(c, pc, 2);
			int msz = (op & 0x40) ? 4 : 2, count = __builtin_popcount(mask), o = 0, r;
			if (mode == 4) {
				if (!(mask & (1 << (7 - y))))
					return 0;
				be_ld(4, VA, FA(y));
				be_addi(VA, -count * msz);
				be_tlb_check(c, PT_TLB_WRITE, count * msz);
				for (r = 0; r < 16; r++) {
					if (mask & (0x8000 >> r)) {
						be_ld(msz, V0, r);
						if (r == FA(y))
							be_addi(V0, -msz);
						be_host_store_off(msz, V0, o);
						o += msz;
					}
				}
				be_st(4, FA(y), VA);
				return 4;
			}
			n = decode_ea(c, mode, y, msz, pc, 4, &d);
			if (n < 0)
				return 0;
			if (count) {
				emit_addr(c, &d);
				be_tlb_check(c, PT_TLB_WRITE, count * msz);
				for (r = 0; r < 16; r++) {
					if (mask & (1 << r)) {
						be_ld(msz, V0, r);
						be_host_store_off(msz, V0, o);
						o += msz;
					}
				}
			}
			return 4 + n;
		}
		/* TST #imm */
		if ((op & 0xff3f) == 0x4a3c && (sz = size_of((op >> 6) & 3)) != 0) {
			uae_u32 iv = sz == 4 ? code_long(c, pc, 2) : sz == 2 ? code_word(c, pc, 2) : (code_word(c, pc, 2) & 0xff);
			be_movi(V0, iv);
			be_test(sz, V0);
			return sz == 4 ? 6 : 4;
		}
		/* TAS <ea> */
		if ((op & 0xffc0) == 0x4ac0 && op != 0x4afc && mode != 1) {
			if (mode == 0) {
				be_ld(1, V0, FD(y));
				be_test(1, V0);
				be_alui(1, OP_OR, V0, 0x80, FL_NONE);
				be_st(1, FD(y), V0);
				return 2;
			}
			if (!mem_ok)
				return 0;
			n = decode_ea(c, mode, y, 1, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 1);
			be_host_load(1, V0);
			be_test(1, V0);
			be_alui(1, OP_OR, V0, 0x80, FL_NONE);
			be_host_store(1, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* MULU.L, MULS.L <ea> */
		if ((op & 0xffc0) == 0x4c00 && mode >= 2 && mem_ok) {
			uae_u16 ext = code_word(c, pc, 2);
			int dl = (ext >> 12) & 7, dh = ext & 7, wide = (ext >> 10) & 1;
			if ((ext & 0x83f8) || (wide && dh == dl))
				return 0;
			n = decode_ea(c, mode, y, 4, pc, 4, &so);
			if (n < 0 || so.kind != OPK_MEM)
				return 0;
			load_operand(c, &so, 4, V1);
			emit_update(&so);
			be_ld(4, V0, FD(dl));
			be_mull((ext >> 11) & 1, wide, FD(dl), FD(dh));
			return 4 + n;
		}
		/* DIVU.L, DIVS.L <ea> */
		if ((op & 0xffc0) == 0x4c40 && mode != 1) {
			uae_u16 ext = code_word(c, pc, 2);
			if (ext & 0x83f8)
				return 0;
			n = decode_ea(c, mode, y, 4, pc, 4, &so);
			if (n < 0 || (so.kind == OPK_MEM && !mem_ok))
				return 0;
			load_operand(c, &so, 4, V1);
			be_movi(V0, ext);
			be_ccall(jh_divl, true);
			be_slow_cmp(c, 4, SC_NE, V0, -1);
			emit_update(&so);
			return 4 + n;
		}
		/* CHK.W, CHK.L <ea>,Dn: the handler for the exception */
		if (((op & 0xf1c0) == 0x4180 || (op & 0xf1c0) == 0x4100) && mode != 1) {
			sz = (op & 0x80) ? 2 : 4;
			n = decode_ea(c, mode, y, sz, pc, 2, &so);
			if (n < 0 || (so.kind == OPK_MEM && !mem_ok))
				return 0;
			load_operand(c, &so, sz, V1);
			be_ld(sz, V0, FD(x));
			be_slow_cmp(c, sz, SC_LT, V0, -1);
			be_slow_cmp(c, sz, SC_GT, V0, V1);
			emit_update(&so);
			be_ld(4, VJ, F_CZNV);                   /* C and N cleared */
			be_alui(4, OP_AND, VJ, ~(uae_u32)(HOST_F_C | HOST_F_N), FL_NONE);
			be_st(4, F_CZNV, VJ);
			return 2 + n;
		}
		/* LEA with a full extension word, (d8,PC,Xn) */
		if ((op & 0xf1c0) == 0x41c0 && (mode == 6 || (mode == 7 && y == 3))) {
			n = decode_ea(c, mode, y, 4, pc, 2, &so);
			if (n < 0 || (so.indirect && !mem_ok))
				return 0;
			emit_addr(c, &so);
			be_st(4, FA(x), VA);
			return 2 + n;
		}
		/* MOVE An,USP and USP,An (in supervisor mode) */
		if ((op & 0xfff0) == 0x4e60 && c->b->s) {
			if (op & 8) {
				be_ld(4, V0, F_USP);
				be_st(4, FA(y), V0);
			} else {
				be_ld(4, V0, FA(y));
				be_st(4, F_USP, V0);
			}
			return 2;
		}
		/* RTD #d16 */
		if (op == 0x4e74 && mem_ok) {
			uae_s32 disp = (uae_s16)code_word(c, pc, 2);
			be_ld(4, VA, FA(7));
			be_tlb_check(c, PT_TLB_READ, 4);
			be_host_load(4, V0);
			be_slow_if_odd(c, V0);
			be_field_addi(FA(7), 4 + disp);
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			return jump_to_reg(c, in, pc, 4, V0);
		}
		/* TRAPV: the handler for the exception */
		if (op == 0x4e76) {
			slow_add(c, be_jcc68(9, 1));
			return 2;
		}
		/* RTR */
		if (op == 0x4e77 && mem_ok) {
			be_ld(4, VA, FA(7));
			be_tlb_check(c, PT_TLB_READ, 6);
			be_host_load_off(2, V0, 0);
			be_host_load_off(4, VJ, 2);
			be_slow_if_odd(c, VJ);
			be_ccr_set(V0);
			be_field_addi(FA(7), 6);
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			return jump_to_reg(c, in, pc, 2, VJ);
		}
		return 0;

	case 0x5: {
		int cc = (op >> 8) & 15;
		/* Scc <ea> */
		if ((op & 0xf0c0) == 0x50c0 && mode != 1 && !(mode == 7 && y >= 2)) {
			uae_u8 *skip;
			n = 0;
			if (mode != 0) {
				if (!mem_ok)
					return 0;
				n = decode_ea(c, mode, y, 1, pc, 2, &d);
				if (n < 0 || !writable(&d))
					return 0;
			}
			if (cc <= 1) {
				be_movi(V0, cc == 0 ? 0xff : 0);
			} else {
				be_movi(V0, 0xff);
				skip = be_jcc68(cc, 1);
				be_movi(V0, 0);
				be_bind(skip);
			}
			if (mode == 0) {
				be_st(1, FD(y), V0);
				return 2;
			}
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 1);
			be_host_store(1, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* TRAPcc: the handler for the exception */
		if ((op & 0xf0ff) == 0x50fa || (op & 0xf0ff) == 0x50fb || (op & 0xf0ff) == 0x50fc) {
			if (cc == 0)
				return 0;
			if (cc != 1)
				slow_add(c, be_jcc68(cc, 1));
			return (op & 7) == 2 ? 4 : (op & 7) == 3 ? 6 : 2;
		}
		return 0;
	}

	case 0x8: {
		int opmode = (op >> 6) & 7;
		/* DIVU.W, DIVS.W <ea>,Dn */
		if ((opmode == 3 || opmode == 7) && mode != 1) {
			n = decode_ea(c, mode, y, 2, pc, 2, &so);
			if (n < 0 || (so.kind == OPK_MEM && !mem_ok))
				return 0;
			load_operand(c, &so, 2, V1);
			be_movi(V0, x);
			be_ccall(opmode == 7 ? jh_divs : jh_divu, true);
			be_slow_cmp(c, 4, SC_NE, V0, -1);
			emit_update(&so);
			return 2 + n;
		}
		/* SBCD */
		if ((op & 0xf1f0) == 0x8100)
			return emit_bcd(c, op, jh_sbcd);
		/* PACK, UNPK Dy,Dx,#adj and -(Ay),-(Ax),#adj */
		if ((op & 0xf1f0) == 0x8140 || (op & 0xf1f0) == 0x8180) {
			bool pack = (op & 0xf1f0) == 0x8140;
			uae_u32 adj = code_word(c, pc, 2);
			if (!(op & 8)) {
				be_ld(2, V0, FD(y));
				emit_pack(pack, adj);
				be_st(pack ? 1 : 2, FD(x), V0);
				return 4;
			}
			if (!mem_ok)
				return 0;
			{
				int ssz = pack ? 2 : 1, dsz = pack ? 1 : 2;
				int sy = (ssz == 1 && y == 7) ? 2 : ssz, sx = (dsz == 1 && x == 7) ? 2 : dsz;
				be_ld(4, VA, FA(y));
				be_addi(VA, -sy);
				be_tlb_check(c, PT_TLB_READ, ssz);
				be_host_load(ssz, V0);
				emit_pack(pack, adj);
				be_ld(4, VA, FA(x));
				be_addi(VA, -(sx + (x == y ? sy : 0)));
				be_tlb_check(c, PT_TLB_WRITE, dsz);
				be_host_store(dsz, V0);
				be_field_addi(FA(y), -sy);
				be_field_addi(FA(x), -sx);
				return 4;
			}
		}
		return 0;
	}

	case 0x9: case 0xd:
		/* ADDX, SUBX Dy,Dx and -(Ay),-(Ax) */
		if ((op & 0x130) == 0x100 && (sz = size_of((op >> 6) & 3)) != 0) {
			int sub = (op >> 12) == 0x9;
			if (!(op & 8)) {
				be_ld(sz, V0, FD(x));
				be_ld(sz, V1, FD(y));
				be_addx(sz, sub, V0, V1);
				be_st(sz, FD(x), V0);
				return 2;
			}
			if (!mem_ok)
				return 0;
			xmem_operands(c, sz, x, y);
			be_addx(sz, sub, V0, V1);
			be_host_store(sz, V0);
			xmem_update(sz, x, y);
			return 2;
		}
		return 0;

	case 0xc:
		/* ABCD */
		if ((op & 0xf1f0) == 0xc100)
			return emit_bcd(c, op, jh_abcd);
		return 0;

	case 0xe: {
		int kind = (op >> 3) & 3, left = (op >> 8) & 1;
		sz = size_of((op >> 6) & 3);
		if (sz && (op & 0x20)) {
			/* ROL, ROR, ROXL, ROXR by a register */
			if (kind == 3) {
				be_rot_reg(sz, left, FD(x), FD(y));
				return 2;
			}
			if (kind == 2) {
				be_movi(V0, op);
				be_ccall(jh_roxreg, true);
				return 2;
			}
			return 0;
		}
		if (sz == 1 && x == 0) {
			/* bytes shifted and rotated by 8 */
			if (kind <= 1) {
				be_shift_reg(1, kind, left, -1 - 8, FD(y));
				return 2;
			}
			if (kind == 3) {
				be_rot_reg(1, left, -1 - 8, FD(y));
				return 2;
			}
			return 0;
		}
		if (sz || !mem_ok)
			return 0;
		/* shifts and rotates of a word in memory by 1 */
		if (!(op & 0x800) && mode >= 2) {
			kind = (op >> 9) & 3;
			n = decode_ea(c, mode, y, 2, pc, 2, &d);
			if (n < 0 || !writable(&d))
				return 0;
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 2);
			be_host_load(2, V0);
			if (kind == 2)
				be_rox(2, left, 1, -1);
			else
				be_shift(2, kind, left, V0, 1, -1);
			be_host_store(2, V0);
			emit_update(&d);
			return 2 + n;
		}
		/* bit fields */
		if ((op & 0x800) && mode != 1 && mode != 3 && mode != 4) {
			uae_u16 ext = code_word(c, pc, 2);
			int bkind = (op >> 8) & 7, size;
			bool writes = bkind == 2 || bkind == 4 || bkind == 6 || bkind == 7;
			if (mode == 0) {
				be_movi(V0, ((uae_u32)op << 16) | ext);
				be_ccall(jh_bitfield, true);
				return 4;
			}
			n = decode_ea(c, mode, y, 1, pc, 4, &d);
			if (n < 0 || d.kind != OPK_MEM || (writes && !writable(&d)))
				return 0;
			emit_addr(c, &d);
			if (ext & 0x800) {
				/* the byte offset of Dn / 8 */
				be_ld(4, V0, FD((ext >> 6) & 7));
				be_sari(V0, 3);
				be_add(VA, V0);
				size = 5;
			} else {
				int off = (ext >> 6) & 31;
				be_addi(VA, off >> 3);
				size = (ext & 0x20) ? 5 : ((off & 7) + ((ext - 1) & 31) + 1 + 7) >> 3;
			}
			be_tlb_check(c, writes ? PT_TLB_WRITE : PT_TLB_READ, size);
			be_movi(V0, ((uae_u32)op << 16) | ext);
			be_ccall(jh_bitfield, true);
			return 4 + n;
		}
		return 0;
	}

	case 0xf:
		if ((op & 0xffc0) == 0xf200)
			return emit_fpu(c, in, pc);
		/* MOVE16 (Ax)+,(Ay)+ and with an absolute address */
		if (mem_ok && ((op & 0xfff8) == 0xf620 || (op & 0xffe0) == 0xf600)) {
			int sr = y, dr = y, k = (op >> 3) & 3, len = 6;
			bool sreg, dreg, supd, dupd;
			uae_u32 absa = 0;
			if ((op & 0xfff8) == 0xf620) {
				uae_u16 ext = code_word(c, pc, 2);
				if ((ext & 0x8fff) != 0x8000)
					return 0;
				dr = (ext >> 12) & 7;
				sreg = dreg = supd = dupd = true;
				len = 4;
			} else {
				absa = code_long(c, pc, 2);
				sreg = !(k & 1);
				dreg = !sreg;
				supd = sreg && !(k & 2);
				dupd = dreg && !(k & 2);
			}
			if (sreg)
				be_ld(4, VA, FA(sr));
			else
				be_movi(VA, absa);
			be_alui(4, OP_AND, VA, 0xfffffff0, FL_NONE);
			be_tlb_check(c, PT_TLB_READ, 16);
			be_host_load_off(4, V0, 0);
			be_host_load_off(4, V1, 4);
			be_host_load_off(4, VJ, 8);
			be_host_load_off(4, VI, 12);
			if (dreg)
				be_ld(4, VA, FA(dr));
			else
				be_movi(VA, absa);
			be_alui(4, OP_AND, VA, 0xfffffff0, FL_NONE);
			be_tlb_check(c, PT_TLB_WRITE, 16);
			be_host_store_off(4, V0, 0);
			be_host_store_off(4, V1, 4);
			be_host_store_off(4, VJ, 8);
			be_host_store_off(4, VI, 12);
			if (supd && !(dupd && sr == dr))
				be_field_addi(FA(sr), 16);
			if (dupd)
				be_field_addi(FA(dr), 16);
			return len;
		}
		/* FBcc */
		if ((op & 0xff80) == 0xf280) {
			bool l = (op & 0x40) != 0;
			uae_s32 disp = l ? (uae_s32)code_long(c, pc, 2) : (uae_s16)code_word(c, pc, 2);
			uae_u8 *skip;
			if (disp & 1)
				return 0;
			be_movi(V0, op & 0x3f);
			be_ccall(jh_fcc, false);
			be_slow_cmp(c, 4, SC_LT, V0, -1);
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			if (in->len > (l ? 6 : 4) && disp + 2 == in->len) {
				/* taken when recorded, to the next instruction of the
				 * block: left when not taken */
				skip = be_jnz(V0);
				exit_to(c, pc + (l ? 6 : 4));
				be_bind(skip);
				return in->len;
			}
			skip = be_jz(V0);
			exit_to(c, pc + 2 + disp);
			be_bind(skip);
			return l ? 6 : 4;
		}
		/* FDBcc */
		if ((op & 0xfff8) == 0xf248) {
			uae_u16 ext = code_word(c, pc, 2);
			uae_s32 disp = (uae_s16)code_word(c, pc, 4);
			uae_u8 *skip;
			if (disp & 1)
				return 0;
			be_movi(V0, ext & 0x3f);
			be_movi(V1, y);
			be_ccall(jh_fdbcc, true);
			be_slow_cmp(c, 4, SC_LT, V0, -1);
			be_add_cycles(in->cycles);
			c->cycles_added = true;
			skip = be_jz(V0);
			exit_to(c, pc + 4 + disp);
			be_bind(skip);
			return 6;
		}
		/* FScc <ea> */
		if ((op & 0xffc0) == 0xf240 && mode != 1 && !(mode == 7 && y >= 2)) {
			uae_u16 ext = code_word(c, pc, 2);
			n = 0;
			if (mode != 0) {
				if (!mem_ok)
					return 0;
				n = decode_ea(c, mode, y, 1, pc, 4, &d);
				if (n < 0 || !writable(&d))
					return 0;
			}
			be_movi(V0, ext & 0x3f);
			be_ccall(jh_fcc, false);
			be_slow_cmp(c, 4, SC_LT, V0, -1);
			be_movi(V1, 0);
			be_alu(4, OP_SUB, V1, V0, FL_NONE);     /* 0 or -1 */
			if (mode == 0) {
				be_st(1, FD(y), V1);
				return 4;
			}
			emit_addr(c, &d);
			be_tlb_check(c, PT_TLB_WRITE, 1);
			be_host_store(1, V1);
			emit_update(&d);
			return 4 + n;
		}
		/* FTRAPcc: the handler for the exception */
		if (op >= 0xf27a && op <= 0xf27c) {
			be_movi(V0, code_word(c, pc, 2) & 0x3f);
			be_ccall(jh_fcc, false);
			be_slow_cmp(c, 4, SC_NE, V0, -1);
			return op == 0xf27a ? 6 : op == 0xf27b ? 8 : 4;
		}
		return 0;
	}
	return 0;
}

/* The flags an instruction sets (as the 68k defines it), and whether it may
 * read the flags in any way: by its operation, or because it may call its
 * handler, fault or leave the block, after which the flags must be exact.
 * Only the instructions that use registers and immediates alone can let
 * the flags of the one before go unset. */
static int flag_info(uae_u16 op, bool *reads)
{
	int mode = (op >> 3) & 7, y = op & 7, opmode = (op >> 6) & 7;
	bool reg_src = mode == 0 || mode == 1 || (mode == 7 && y == 4);   /* Dn, An or #imm */
	int ss;

	*reads = true;
	switch (op >> 12) {
	case 0x0:
		/* ORI, ANDI, SUBI, ADDI, EORI, CMPI to Dn or memory */
		if ((op & 0x100) || !size_of((op >> 6) & 3) || mode == 1)
			return 0;
		switch ((op >> 9) & 7) {
		case 0: case 1: case 5: case 6:
			*reads = mode != 0;
			return F_NZVC;
		case 2: case 3:
			*reads = mode != 0;
			return F_ALL;
		}
		return 0;
	case 0x1: case 0x2: case 0x3:
		/* MOVE, MOVEA */
		ss = op >> 12;
		if (ss == 1 && (mode == 1 || ((op >> 6) & 7) == 1))
			return 0;                 /* byte moves with An: illegal */
		if (((op >> 6) & 7) == 1)
			return (*reads = !reg_src, 0);
		*reads = !(reg_src && ((op >> 6) & 7) == 0);
		return F_NZVC;
	case 0x4:
		if ((op & 0xf1c0) == 0x41c0) {
			/* LEA: no access (the other modes are illegal) */
			*reads = !(mode == 2 || mode == 5 || mode == 6 || (mode == 7 && y <= 3));
			return 0;
		}
		if (((op & 0xff00) == 0x4200 || (op & 0xff00) == 0x4600 || (op & 0xff00) == 0x4a00) && size_of((op >> 6) & 3)) {
			/* CLR, NOT, TST (TST.W and TST.L of An too) */
			*reads = !(mode == 0 || ((op & 0xff00) == 0x4a00 && mode == 1 && ((op >> 6) & 3) != 0));
			return F_NZVC;
		}
		if ((op & 0xff00) == 0x4400 && size_of((op >> 6) & 3)) {
			*reads = mode != 0;       /* NEG */
			return F_ALL;
		}
		if ((op & 0xfff8) == 0x4840 || (op & 0xfff8) == 0x4880 || (op & 0xfff8) == 0x48c0 || (op & 0xfff8) == 0x49c0) {
			*reads = false;           /* SWAP, EXT, EXTB */
			return F_NZVC;
		}
		return 0;
	case 0x5:
		if (((op >> 6) & 3) == 3)
			return 0;                 /* Scc, DBcc, TRAPcc */
		if (mode == 1) {
			*reads = ((op >> 6) & 3) == 0;   /* ADDQ, SUBQ to An (not bytes) */
			return 0;
		}
		*reads = mode != 0;
		return F_ALL;
	case 0x7:
		if (op & 0x100)
			return 0;
		*reads = false;               /* MOVEQ */
		return F_NZVC;
	case 0x8: case 0x9: case 0xb: case 0xc: case 0xd:
		if ((op >> 12) == 0xc && ((op & 0x1f8) == 0x140 || (op & 0x1f8) == 0x148 || (op & 0x1f8) == 0x188)) {
			*reads = false;           /* EXG */
			return 0;
		}
		if ((op >> 12) == 0xc && (opmode == 3 || opmode == 7)) {
			*reads = !(mode == 0 || (mode == 7 && y == 4));   /* MULU, MULS */
			return F_NZVC;
		}
		if ((op >> 12) == 0x8 && (opmode == 3 || opmode == 7))
			return 0;                 /* DIVU, DIVS */
		if (opmode == 3 || opmode == 7) {
			*reads = !reg_src;        /* ADDA, SUBA, CMPA */
			return (op >> 12) == 0xb ? F_NZVC : 0;
		}
		if (opmode < 3) {
			*reads = !reg_src || (mode == 1 && (opmode == 0 ||
				((op >> 12) != 0x9 && (op >> 12) != 0xd && (op >> 12) != 0xb)));
			return ((op >> 12) == 0x9 || (op >> 12) == 0xd) ? F_ALL : F_NZVC;
		}
		/* op Dn,<ea>, EOR, CMPM; ADDX, SUBX, ABCD, SBCD and the like read X */
		if (mode <= 1 && (op >> 12) != 0xb)
			return 0;
		*reads = !((op >> 12) == 0xb && mode == 0);   /* EOR Dn,Dn */
		return ((op >> 12) == 0x9 || (op >> 12) == 0xd) ? F_ALL : F_NZVC;
	case 0xe:
		if (((op >> 6) & 3) != 3 && (op & 0x20) && ((op >> 3) & 3) <= 1) {
			*reads = false;           /* LSL, LSR, ASL, ASR by a register (X kept for 0) */
			return F_NZVC;
		}
		/* shifts and rotates of a register by an immediate count */
		if (((op >> 6) & 3) == 3 || (op & 0x20) || ((op >> 3) & 3) == 2)
			return 0;
		*reads = false;
		return ((op >> 3) & 3) == 3 ? F_NZVC : F_ALL;
	}
	return 0;
}

static uae_u8 *compile_pass(struct jit040_block *b, int only_native, bool dry)
{
	int needs[JIT040_MAX_INS];
	struct ctx c;
	uae_u8 *start;
	uaecptr pc = b->pc;
	int i, native = 0;

	if (!code_buf)
		return NULL;
	if (code_used + CODE_MAX_BLOCK > CODE_SIZE)
		return NULL;
	c.b = b;
	c.nexits = 0;
	c.ended = false;
	c.nstubs = 0;
	cp = start = code_buf + code_used;
	be_target();
	be_write_begin();

	be_prologue(b);
	be_cache_load();
	be_target();
	b->chain_self = cp;

	/* the flags each instruction must set: all at the end of the block */
	{
		int live = F_ALL;
		for (i = b->n - 1; i >= 0; i--) {
			bool reads;
			int writes = flag_info(b->ins[i].opcode, &reads);
			needs[i] = writes ? writes & live : F_ALL;
			live = reads ? F_ALL : live & ~writes;
		}
	}

	for (i = 0; i < b->n && !c.ended; i++) {
		struct jit040_ins *in = &b->ins[i];
		uae_u8 *mark = cp;
		int nexits = c.nexits, nstubs = c.nstubs;
		bool last = i == b->n - 1;
		int len;
		c.cycles_added = false;
		c.cur = NULL;
		need = needs[i];
		len = emit_native(&c, in, pc);
		if (!len) {
			cp = mark;
			be_target();
			len = emit_mem(&c, in, pc);
		}
		if (!len) {
			cp = mark;
			be_target();
			c.nexits = nexits;
			c.nstubs = nstubs;
			c.ended = false;
			c.cur = NULL;
			c.cycles_added = false;
			len = emit_ext(&c, in, pc);
		}
		if (len && in->len && len != in->len) {
			/* not what was recorded: use the handler */
			len = 0;
		}
		if (!len) {
			cp = mark;
			be_target();
			c.nexits = nexits;
			c.nstubs = nstubs;
			c.ended = false;
			c.cur = NULL;
		}
		if (len) {
			native++;
			if (!c.cycles_added)
				be_add_cycles(in->cycles);
			if (last && !c.ended) {
				exit_to(&c, pc + len);
				c.ended = true;
			}
			if (c.cur) {
				c.cur->in = in;
				c.cur->pc = pc;
				c.cur->last = last || c.ended || in->len == 0;
				be_join(c.cur, in->cycles);
			}
		} else {
			if (only_native == 1 || (only_native == 2 && !last)) {
				need = F_ALL;
				be_write_end(start, start);
				return NULL;
			}
			be_call(&c, in, pc, last);
		}
		pc += in->len;
	}

	need = F_ALL;

	/* the handlers of the accesses that are not done directly, out of the way
	 * (a call to the last instruction's handler falls through to here) */
	if (c.nstubs)
		be_exit(&c);
	for (i = 0; i < c.nstubs; i++) {
		struct stub *st = &c.stubs[i];
		int j;
		for (j = 0; j < st->njumps; j++)
			be_bind(st->jumps[j]);
		be_call(&c, st->in, st->pc, st->last);
		if (st->last)
			be_exit(&c);
		else
			be_rejoin(st);
	}

	/* the exits */
	for (i = 0; i < c.nexits; i++)
		be_bind(c.exits[i]);
	be_epilogue();

	be_write_end(start, cp);
	if (!only_native && native == 0) {
		/* nothing gained over the block cache alone */
		return NULL;
	}
	if (dry)
		return start;
	code_used = (cp - code_buf + 15) & ~(size_t)15;
	return start;
}

/* The block is compiled once to see which guest registers it uses most,
 * then again with those in host registers: in the guest registers' order,
 * so that blocks keeping the same ones keep them in the same places, and
 * a block linked to from one of them need not load them */
static uae_u8 *compile(struct jit040_block *b, int only_native)
{
	int k, f, least = jit040_selftesting ? 1 : 2;
	bool pick[16] = { false };

	memset(cache_reg, -1, sizeof(cache_reg));
	memset(cache_uses, 0, sizeof(cache_uses));
	memset(b->cached, -1, sizeof(b->cached));
	if (BE_CACHED && compile_pass(b, only_native, true)) {
		for (k = 0; k < BE_CACHED; k++) {
			int best = -1;
			for (f = 0; f < 16; f++) {
				if (!pick[f] && cache_uses[f] >= least &&
				    (best < 0 || cache_uses[f] > cache_uses[best]))
					best = f;
			}
			if (best < 0)
				break;
			pick[best] = true;
		}
		for (f = 0, k = 0; f < 16; f++) {
			if (pick[f]) {
				cache_reg[f] = cache_host[k];
				b->cached[k++] = f;
			}
		}
	}
	return compile_pass(b, only_native, false);
}

bool jit040_compile(struct jit040_block *b)
{
	b->code = (jit040_code)compile(b, 0);
	if (!b->code && code_buf && code_used + CODE_MAX_BLOCK > CODE_SIZE) {
		/* full: start again, blocks and all (called between blocks) */
		jit040_flush();
	}
	return b->code != NULL;
}

/* Self-test: every instruction that has a translation, run as a one
 * instruction block and by its handler from the same random states, must
 * give the same registers, flags and PC. Started with the environment
 * variable PREVIOUSTURBO_JIT_SELFTEST when Turbo comes on; the program
 * then stops, as the machine's state is lost. */

static uae_u32 st_seed = 0x2545f491;
static uae_u32 st_rand(void)
{
	st_seed ^= st_seed << 13;
	st_seed ^= st_seed >> 17;
	st_seed ^= st_seed << 5;
	return st_seed;
}
static uae_u32 st_value(void)
{
	static const uae_u32 edges[] = {
		0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
		0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0xffffff80, 0xffff8000
	};
	switch (st_rand() & 3) {
	case 0:
		return edges[st_rand() % (sizeof(edges) / sizeof(edges[0]))];
	case 1:
		return st_rand() & 0xff;
	default:
		return st_rand();
	}
}

struct st_state {
	uae_u32 r[16], pc;
	uae_u32 cznv, x;
	uae_u32 s;
	uae_u32 fpu[27];    /* FP0-FP7 (3 words each), FPCR, FPSR, FPIAR */
};
static void st_get(struct st_state *t)
{
	memcpy(t->r, regs.regs, sizeof(t->r));
	t->pc = regs.pc;
	t->cznv = regflags.cznv & HOST_CZNV;
	t->x = regflags.x & 1;
	t->s = regs.s;
	fpp_jit_get_state(t->fpu);
}
static struct regstruct st_regs;  /* everything else, the same for each run */
static void st_set(const struct st_state *t)
{
	regs = st_regs;
	memcpy(regs.regs, t->r, sizeof(t->r));
	regs.pc = t->pc;
	regflags.cznv = t->cznv;
	regflags.x = t->x;
	regs.s = t->s;
	fpp_jit_set_state(t->fpu);
}

/* an extended precision value, often an ordinary one, sometimes 0, an
 * infinity, a NaN, a denormal or an unnormal */
static void st_fp(uae_u32 *w)
{
	uae_u32 e;
	switch (st_rand() & 7) {
	case 0: e = 0; break;                                   /* 0, denormal */
	case 1: e = 0x7fff; break;                              /* infinity, NaN */
	case 2: e = st_rand() & 0x7fff; break;
	default: e = 0x3fff + (st_rand() % 64) - 32; break;     /* near 1 */
	}
	w[0] = ((st_rand() & 1) ? 0x80000000 : 0) | (e << 16);
	w[1] = st_rand();
	w[2] = (st_rand() & 1) ? st_rand() : 0;
	if (e && e != 0x7fff && (st_rand() & 7))
		w[1] |= 0x80000000;                                 /* normalized */
	if ((st_rand() & 7) == 0)
		w[1] = w[2] = 0;
}

/* the page cache with the code page for fetches and the data page for reads
 * and writes (but not for the kinds in drop) */
static void st_tlb(uae_u8 *page, uae_u8 *data, uaecptr vpc, uaecptr vdata, int drop)
{
	int i;

	memset(pt_tlb, 0, sizeof(pt_tlb));
	for (i = 0; i < 3; i++) {
		uaecptr va = i == 0 ? vpc : vdata;
		struct pt_tlb_entry *e = PT_TLB_SLOT(i, va);
		if (drop & (1 << i))
			continue;
		e->tag = PT_TLB_TAG(va);
		e->phys = va & 0xfffff000;
		e->host = i == 0 ? page : data;
		e->cache = 0;
		PT_TLB_JIT(e);
	}
}

static uae_u32 st_next_iword(void)
{
	return next_iword_jit();
}
static uae_u32 st_next_ilong(void)
{
	return next_ilong_jit();
}

/* an extension word of the FPU's general instructions that the JIT takes */
static uae_u16 st_fpu_ext(void)
{
	static const uae_u8 ops[] = {
		0x00, 0x04, 0x18, 0x1a, 0x20, 0x22, 0x23, 0x24, 0x27, 0x28, 0x38, 0x3a, 0x40, 0x41,
		0x44, 0x45, 0x58, 0x5a, 0x5c, 0x5e, 0x60, 0x62, 0x63, 0x64, 0x66, 0x67, 0x68, 0x6c
	};
	uae_u16 op = ops[st_rand() % sizeof(ops)], fp = (st_rand() & 7) << 7;
	uae_u16 size = (st_rand() % 7) << 10;

	switch (st_rand() % 6) {
	case 0: return ((st_rand() & 7) << 10) | fp | op;           /* FPm to FPn */
	case 1: case 2: return 0x4000 | size | fp | op;           /* <ea> to FPn */
	case 3: return 0x6000 | size | fp;                         /* FPn to <ea> */
	case 4: return 0x8000 | (st_rand() & 0x2000) | (0x400 << (st_rand() % 3));   /* FPcr */
	default: return 0xc000 | (st_rand() & 0x3000) | (st_rand() & 0xff);         /* FMOVEM */
	}
}

void jit040_selftest(void)
{
	static uae_u8 page[4096], data[4096 + 4], data_in[4096 + 4], data_nat[4096 + 4];
	static uae_u8 other_in[sizeof(jit040_selftest_mem)], other_nat[sizeof(jit040_selftest_mem)];
	static union {
		struct jit040_block b;
		uae_u8 space[sizeof(struct jit040_block) + 2 * sizeof(struct jit040_ins)];
	} u;
	struct jit040_block *b = &u.b;
	const uaecptr vpc = 0x04000100, vdata = 0x05000000;
	/* after the instruction, in half the runs: EXG D3,D4 (translated) or
	 * NOP (a call to its handler) */
	uae_u16 second;
	int op, i, trial, ops = 0, runs = 0, failed = 0, direct = 0, pairs = 0;
	int first = 0, last = 65535;
	const char *range = getenv("PREVIOUSTURBO_JIT_SELFTEST");

	if (!jit040_native_init()) {
		fprintf(stderr, "JIT SELFTEST: no native code on this host\n");
		fflush(NULL);
		_Exit(1);
	}
	/* the FPU's handlers through the test's pages too */
	x_cp_next_iword = st_next_iword;
	x_cp_next_ilong = st_next_ilong;
	x_cp_get_byte = jit040_get_byte;
	x_cp_get_word = jit040_get_word;
	x_cp_get_long = jit040_get_long;
	x_cp_put_byte = jit040_put_byte;
	x_cp_put_word = jit040_put_word;
	x_cp_put_long = jit040_put_long;
	x_cp_get_disp_ea_020 = jit040_get_disp_ea_020;
	regs.t0 = regs.t1 = 0;
	regs.spcflags = 0;
	st_regs = regs;
	jit040_selftest_data = data;
	jit040_selftest_vdata = vdata;
	jit040_selftesting = true;
	for (i = 0; i < (int)sizeof(data_in); i++)
		data_in[i] = st_rand();
	for (i = 0; i < (int)sizeof(other_in); i++)
		other_in[i] = st_rand();
	/* PREVIOUSTURBO_JIT_SELFTEST=first-last (hexadecimal) tests those opcodes only */
	if (range && strchr(range, '-'))
		sscanf(range, "%x-%x", (unsigned *)&first, (unsigned *)&last);
	for (op = first; op <= last && op < 65536; op++) {
		int ran = 0;
		bool fpu = (op & 0xffc0) == 0xf200;
		for (trial = 0; trial < (fpu ? 50000 : 200); trial++) {
			struct st_state in, nat, ref, ref1;
			const struct st_state *want;
			jit040_code code;
			volatile bool faulted = false, faulted_nat = false;
			volatile uae_u32 cycles = 0, c0 = 0, c1 = 0;
			uae_u32 want_cycles;
			uae_u32 len;
			bool two;
			int drop;

			for (i = 0; i < 32; i += 2) {
				/* small displacements, addresses in the data page */
				uae_u32 w = st_rand();
				switch (w & 3) {
				case 0: w = (w & 4) ? (w >> 16) & 0x7c07 : (w >> 8) & 0xff; break;   /* also as MULx.L's */
				case 1: w = 0xff00 | ((w >> 8) & 0xff); break;
				case 2: w = vdata >> 16; break;
				default: w >>= 16; break;
				}
				do_put_mem_word(page + 0x100 + i, w);
			}
			do_put_mem_word(page + 0x100, op);
			if (fpu)
				do_put_mem_word(page + 0x102, st_fpu_ext());

			/* only instructions that are translated */
			memset(b, 0, sizeof(u));
			b->pc = vpc;
			b->page = -1;
			b->host_page = page;
			b->hdelta = (uintptr_t)page - (vpc & 0xfffff000);
			b->n = 1;
			b->ins[0].fn = jit040_functbl[op];
			b->ins[0].opcode = op;
			b->ins[0].cycles = 1;
			b->s = (trial >> 1) & 1;
			code_used = CODE_START;
			if (!compile(b, 1))
				continue;

			for (i = 0; i < 8; i++)
				in.r[i] = (st_rand() & 1) ? (st_rand() & 0x7f) : st_value();
			for (i = 8; i < 16; i++) {
				switch (st_rand() & 7) {
				case 0: in.r[i] = vdata + 0xff8 + st_rand() % 8; break;   /* accesses across the page's ends */
				case 1: in.r[i] = vdata + st_rand() % 8; break;
				default: in.r[i] = vdata + 0x100 + st_rand() % 0xe00; break;
				}
			}
			for (i = 0; i < 8; i++)
				st_fp(&in.fpu[i * 3]);
			/* FPCR: mostly the defaults, sometimes another rounding and
			 * precision, sometimes exceptions enabled */
			in.fpu[24] = 0;
			if ((st_rand() & 3) == 0)
				in.fpu[24] |= st_rand() & 0xf0;
			if ((st_rand() & 7) == 0)
				in.fpu[24] |= st_rand() & 0xff00;
			in.fpu[25] = st_rand() & 0x0fffffff;
			in.fpu[26] = st_rand();
			in.pc = vpc;
			in.cznv = st_rand() & HOST_CZNV;
			in.x = st_rand() & 1;
			in.s = (trial >> 1) & 1;
			/* sometimes the handler does the data accesses */
			drop = 0;
			force_slow = (st_rand() & 3) == 0;
			st_set(&in);
			st_tlb(page, data, vpc, vdata, drop);

			/* the handlers first: the instruction, and the second one
			 * if the first went on to it */
			memcpy(data, data_in, sizeof(data_in));
			memcpy(jit040_selftest_mem, other_in, sizeof(other_in));
			jit040_hdelta = (uintptr_t)page - (vpc & 0xfffff000);
			regs.instruction_pc = vpc;
			TRY(prb) {
				c0 = (*jit040_functbl[op])(op);
			} CATCH(prb) {
				faulted = true;
			} ENDTRY
			len = regs.pc - vpc;
			two = !faulted && (trial & 1) && len >= 2 && len <= 22 && regs.s == in.s;
			/* NOP, EXG, MOVEQ (sets NZVC), CMP (reads none), BEQ */
			second = (uint16_t[]){ 0x4e71, 0xc744, 0x7a05, 0xbc87, 0x6702 }[(trial >> 2) % 5];
			st_get(&ref1);
			if (two) {
				do_put_mem_word(page + 0x100 + len, second);
				c1 = (*jit040_functbl[second])(second);
			}
			st_get(&ref);

			memset(b, 0, sizeof(u));
			b->pc = vpc;
			b->s = in.s;
			b->page = -1;
			b->host_page = page;
			b->hdelta = (uintptr_t)page - (vpc & 0xfffff000);
			b->n = two ? 2 : 1;
			b->ins[0].fn = jit040_functbl[op];
			b->ins[0].opcode = op;
			b->ins[0].len = two ? len : 0;
			b->ins[0].cycles = c0;
			b->ins[1].fn = jit040_functbl[second];
			b->ins[1].opcode = second;
			b->ins[1].cycles = c1;
			code_used = CODE_START;
			code = (jit040_code)compile(b, two ? 2 : 1);
			force_slow = false;
			if (!code)
				continue;

			st_set(&in);
			memcpy(data, data_in, sizeof(data_in));
			memcpy(jit040_selftest_mem, other_in, sizeof(other_in));
			st_tlb(page, data, vpc, vdata, drop);
			mmu_opcode = 0xffff;   /* the handler's call sets it */
			TRY(prb) {
				cycles = code();
			} CATCH(prb) {
				faulted_nat = true;
			} ENDTRY
			st_get(&nat);
			memcpy(data_nat, data, sizeof(data_nat));
			memcpy(other_nat, jit040_selftest_mem, sizeof(other_nat));
			if (mmu_opcode == 0xffff)
				direct++;
			memcpy(data, data_in, sizeof(data_in));
			ran++;
			pairs += two;

			/* the handler's run again, for its memory */
			memcpy(jit040_selftest_mem, other_in, sizeof(other_in));
			st_set(&in);
			st_tlb(page, data, vpc, vdata, drop);
			regs.instruction_pc = vpc;
			faulted = false;
			TRY(prb) {
				(*jit040_functbl[op])(op);
				if (two)
					(*jit040_functbl[second])(second);
			} CATCH(prb) {
				faulted = true;
			} ENDTRY

			/* a block may also be left early, at the right place */
			want = (two && nat.pc == vpc + len) ? &ref1 : &ref;
			want_cycles = two && want == &ref ? c0 + c1 : c0;
			if (faulted != faulted_nat || (!faulted_nat && cycles != want_cycles) ||
			    memcmp(&nat, want, sizeof(nat)) || memcmp(data, data_nat, sizeof(data_nat)) ||
			    memcmp(jit040_selftest_mem, other_nat, sizeof(other_nat))) {
				if (failed++ < 40) {
					fprintf(stderr, "JIT SELFTEST: %04x %04x %04x %04x%s%s%s%s%s\n", op,
						do_get_mem_word(page + 0x102), do_get_mem_word(page + 0x104),
						do_get_mem_word(page + 0x106), in.s ? " super" : " user", two ? (second == 0x4e71 ? " +nop" : second == 0xc744 ? " +exg" : second == 0x7a05 ? " +moveq" : second == 0xbc87 ? " +cmp" : " +beq") : "",
						faulted ? " (handler faulted)" : "", faulted_nat ? " (jit faulted)" : "",
						memcmp(data, data_nat, sizeof(data_nat)) || memcmp(jit040_selftest_mem, other_nat, sizeof(other_nat)) ?
						" (memory differs)" : "");
					for (i = 0; i < 16; i++) {
						if (nat.r[i] != want->r[i] || in.r[i] != want->r[i])
							fprintf(stderr, "  %c%d in %08x jit %08x want %08x\n",
								i < 8 ? 'D' : 'A', i & 7, in.r[i], nat.r[i], want->r[i]);
					}
					fprintf(stderr, "  ccr in %04x/%d jit %04x/%d want %04x/%d  pc jit %08x want %08x\n",
						(int)in.cznv, (int)in.x, (int)nat.cznv, (int)nat.x, (int)want->cznv, (int)want->x,
						nat.pc, want->pc);
					if (!faulted_nat && cycles != want_cycles)
						fprintf(stderr, "  cycles jit %u want %u\n", (unsigned)cycles, (unsigned)want_cycles);
				}
				break;
			}
		}
		if (ran) {
			ops++;
			runs += ran;
		}
	}
	code_used = CODE_START;
	jit040_selftesting = false;
	fprintf(stderr, "JIT SELFTEST: %d opcodes translated, %d runs (%d without the handler, %d with a second instruction), %d failed\n",
		ops, runs, direct, pairs, failed);
	/* the other threads are still running: no clean-up */
	fflush(NULL);
	_Exit(failed ? 1 : 0);
}

#else /* !JIT040_NATIVE */

bool jit040_native_init(void)
{
	return false;
}
void jit040_native_flush(void)
{
}
bool jit040_compile(struct jit040_block *b)
{
	return false;
}
void jit040_link(void)
{
}
void jit040_selftest(void)
{
	fprintf(stderr, "JIT SELFTEST: no native code on this host\n");
	fflush(NULL);
	_Exit(1);
}

#endif /* JIT040_NATIVE */
