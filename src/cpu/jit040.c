/*
  PreviousTurbo - jit040.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Block cache for the 68040 with MMU, used in Turbo.

  A block is a run of instructions in one code page, recorded while the
  interpreter runs them for the first time. It is found again by its address
  and supervisor mode, and used only while the MMU still maps that address to
  the same page. Its instructions are run by the handlers of cpuemu_36, which
  read the instruction words straight from the host's copy of the page. The
  block is left as soon as an instruction does not go on to the next one
  (taken branch, exception, change of mode) or something needs attention.

  The RAM pages that hold blocks are kept out of the page cache's direct
  writes, so every write to them reaches the RAM bank functions, which drop
  the page's blocks. That covers the CPU, DMA and self-modifying code.
*/
#include "sysconfig.h"
#include "sysdeps.h"

#include "main.h"
#include "hatari-glue.h"

#include "options_cpu.h"
#include "memory.h"
#include "newcpu.h"
#include "cpummu.h"
#include "jit040.h"

#define JIT040_HASH_SIZE 65536
#define JIT040_POOL_SIZE (16 << 20)
#define JIT040_RAM_PAGES 65536      /* 256 MB, more than any NeXT has */
#define JIT040_MAX_INS_LEN 22       /* the longest 68040 instruction */

cpuop_func *jit040_functbl[65536];
bool jit040_active;
int jit040_stop;
bool jit040_recording;
uintptr_t jit040_hdelta;

uae_u8 *jit040_ram_base;
uae_u32 jit040_ram_size;
uae_u8 jit040_code_ram[JIT040_RAM_PAGES];

static struct jit040_block *hash[JIT040_HASH_SIZE];
struct jit040_block *jit040_fast[JIT040_FAST_SIZE];
static struct jit040_block *page_blocks[JIT040_RAM_PAGES];
static uae_u8 *pool;
static size_t pool_used;

static struct {
	uaecptr pc;
	uae_u8 s;
	int page;
	uae_u8 *host_page;
	int n;
	struct jit040_ins ins[JIT040_MAX_INS];
} rec;

static inline unsigned int hash_of(uaecptr pc)
{
	return ((pc >> 1) ^ (pc >> 17)) & (JIT040_HASH_SIZE - 1);
}

/* The handlers of cpuemu_36 in place of those of cpuemu_31 */

struct handler_pair {
	cpuop_func *mmu, *jit;
};

static int pair_cmp(const void *a, const void *b)
{
	uintptr_t x = (uintptr_t)((const struct handler_pair *)a)->mmu;
	uintptr_t y = (uintptr_t)((const struct handler_pair *)b)->mmu;
	return x < y ? -1 : x > y;
}

void jit040_build_functbl(const struct cputbl *tbl, const struct cputbl *jittbl)
{
	struct handler_pair *pairs;
	int i, n;

	for (n = 0; tbl[n].handler_ff != NULL || tbl[n].handler_ff_noret != NULL; n++)
		;
	pairs = xmalloc(struct handler_pair, n);
	for (i = 0; i < n; i++) {
		if (jittbl[i].opcode != tbl[i].opcode)
			abort();
		pairs[i].mmu = tbl[i].handler_ff;
		pairs[i].jit = jittbl[i].handler_ff;
	}
	qsort(pairs, n, sizeof(*pairs), pair_cmp);
	for (i = 0; i < 65536; i++) {
		struct handler_pair key, *found;
		key.mmu = cpufunctbl[i];
		found = bsearch(&key, pairs, n, sizeof(*pairs), pair_cmp);
		jit040_functbl[i] = found ? found->jit : cpufunctbl[i];
	}
	xfree(pairs);
}

/* The full data accesses of the cpuemu_36 handlers. In the self-test they
 * use a scratch page instead of the machine's memory. */

bool jit040_selftesting;
uae_u8 jit040_selftest_mem[4096 + 4];
uae_u8 *jit040_selftest_data;     /* the test's data page, at jit040_selftest_vdata */
uaecptr jit040_selftest_vdata;

#define SELFTEST_MEM(addr) ((((addr) & 0xfffff000) == jit040_selftest_vdata ? \
	jit040_selftest_data : jit040_selftest_mem) + ((addr) & 0xfff))

uae_u32 jit040_get_byte(uaecptr addr)
{
	if (unlikely(jit040_selftesting))
		return *SELFTEST_MEM(addr);
	return uae_mmu040_get_byte(addr);
}
uae_u32 jit040_get_word(uaecptr addr)
{
	if (unlikely(jit040_selftesting))
		return do_get_mem_word(SELFTEST_MEM(addr));
	return uae_mmu040_get_word(addr);
}
uae_u32 jit040_get_long(uaecptr addr)
{
	if (unlikely(jit040_selftesting))
		return do_get_mem_long(SELFTEST_MEM(addr));
	return uae_mmu040_get_long(addr);
}
void jit040_put_byte(uaecptr addr, uae_u32 v)
{
	if (unlikely(jit040_selftesting))
		*SELFTEST_MEM(addr) = v;
	else
		uae_mmu040_put_byte(addr, v);
}
void jit040_put_word(uaecptr addr, uae_u32 v)
{
	if (unlikely(jit040_selftesting))
		do_put_mem_word(SELFTEST_MEM(addr), v);
	else
		uae_mmu040_put_word(addr, v);
}
void jit040_put_long(uaecptr addr, uae_u32 v)
{
	if (unlikely(jit040_selftesting))
		do_put_mem_long(SELFTEST_MEM(addr), v);
	else
		uae_mmu040_put_long(addr, v);
}

/* The other accesses of the cpuemu_36 handlers: the 68040 MMU's, but in the
 * self-test, where they use the scratch page too */

uae_u32 REGPARAM2 jit040_get_disp_ea_020(uae_u32 base, int idx)
{
	uae_u16 dp = next_iword_jit();
	int reg = (dp >> 12) & 15;
	uae_s32 regd = regs.regs[reg];

	if ((dp & 0x800) == 0)
		regd = (uae_s32)(uae_s16)regd;
	regd <<= (dp >> 9) & 3;
	if (dp & 0x100) {
		uae_s32 outer = 0;
		if (dp & 0x80)
			base = 0;
		if (dp & 0x40)
			regd = 0;
		if ((dp & 0x30) == 0x20)
			base += (uae_s32)(uae_s16)next_iword_jit();
		if ((dp & 0x30) == 0x30)
			base += next_ilong_jit();
		if ((dp & 0x3) == 0x2)
			outer = (uae_s32)(uae_s16)next_iword_jit();
		if ((dp & 0x3) == 0x3)
			outer = next_ilong_jit();
		if ((dp & 0x4) == 0)
			base += regd;
		if (dp & 0x3)
			base = get_long_jit(base);
		if (dp & 0x4)
			base += regd;
		return base + outer;
	}
	return base + (uae_s32)((uae_s8)dp) + regd;
}

uae_u32 jit040_get_lrmw_byte(uaecptr addr)
{
	if (unlikely(jit040_selftesting))
		return *SELFTEST_MEM(addr);
	return get_lrmw_byte_mmu040(addr);
}
uae_u32 jit040_get_lrmw_word(uaecptr addr)
{
	if (unlikely(jit040_selftesting))
		return do_get_mem_word(SELFTEST_MEM(addr));
	return get_lrmw_word_mmu040(addr);
}
uae_u32 jit040_get_lrmw_long(uaecptr addr)
{
	if (unlikely(jit040_selftesting))
		return do_get_mem_long(SELFTEST_MEM(addr));
	return get_lrmw_long_mmu040(addr);
}
void jit040_put_lrmw_byte(uaecptr addr, uae_u32 v)
{
	if (unlikely(jit040_selftesting))
		*SELFTEST_MEM(addr) = v;
	else
		put_lrmw_byte_mmu040(addr, v);
}
void jit040_put_lrmw_word(uaecptr addr, uae_u32 v)
{
	if (unlikely(jit040_selftesting))
		do_put_mem_word(SELFTEST_MEM(addr), v);
	else
		put_lrmw_word_mmu040(addr, v);
}
void jit040_put_lrmw_long(uaecptr addr, uae_u32 v)
{
	if (unlikely(jit040_selftesting))
		do_put_mem_long(SELFTEST_MEM(addr), v);
	else
		put_lrmw_long_mmu040(addr, v);
}

void jit040_get_move16(uaecptr addr, uae_u32 *v)
{
	int i;

	if (likely(!jit040_selftesting)) {
		get_move16_mmu(addr, v);
		return;
	}
	for (i = 0; i < 4; i++)
		v[i] = do_get_mem_long(SELFTEST_MEM((addr & ~15) + i * 4));
}
void jit040_put_move16(uaecptr addr, uae_u32 *v)
{
	int i;

	if (likely(!jit040_selftesting)) {
		put_move16_mmu(addr, v);
		return;
	}
	for (i = 0; i < 4; i++)
		do_put_mem_long(SELFTEST_MEM((addr & ~15) + i * 4), v[i]);
}

/* a bit field in memory at host, as get_bitfield and put_bitfield do it */
uae_u32 jit040_bf_get(uae_u8 *host, uae_u32 bdata[2], uae_s32 offset, int width)
{
	uae_u32 tmp, res, mask;

	offset &= 7;
	mask = 0xffffffffu << (32 - width);
	switch ((offset + width + 7) >> 3) {
	case 1:
		tmp = host[0];
		res = tmp << (24 + offset);
		bdata[0] = tmp & ~(mask >> (24 + offset));
		break;
	case 2:
		tmp = do_get_mem_word(host);
		res = tmp << (16 + offset);
		bdata[0] = tmp & ~(mask >> (16 + offset));
		break;
	case 3:
		tmp = do_get_mem_word(host);
		res = tmp << (16 + offset);
		bdata[0] = tmp & ~(mask >> (16 + offset));
		tmp = host[2];
		res |= tmp << (8 + offset);
		bdata[1] = tmp & ~(mask >> (8 + offset));
		break;
	case 4:
		tmp = do_get_mem_long(host);
		res = tmp << offset;
		bdata[0] = tmp & ~(mask >> offset);
		break;
	default:
		tmp = do_get_mem_long(host);
		res = tmp << offset;
		bdata[0] = tmp & ~(mask >> offset);
		tmp = host[4];
		res |= tmp >> (8 - offset);
		bdata[1] = tmp & ~(mask << (8 - offset));
		break;
	}
	return res;
}
void jit040_bf_put(uae_u8 *host, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width)
{
	offset = (offset & 7) + width;
	switch ((offset + 7) >> 3) {
	case 1:
		host[0] = bdata[0] | (val << (8 - offset));
		break;
	case 2:
		do_put_mem_word(host, bdata[0] | (val << (16 - offset)));
		break;
	case 3:
		do_put_mem_word(host, bdata[0] | (val >> (offset - 16)));
		host[2] = bdata[1] | (val << (24 - offset));
		break;
	case 4:
		do_put_mem_long(host, bdata[0] | (val << (32 - offset)));
		break;
	default:
		do_put_mem_long(host, bdata[0] | (val >> (offset - 32)));
		host[4] = bdata[1] | (val << (40 - offset));
		break;
	}
}

uae_u32 jit040_get_bitfield(uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width)
{
	if (unlikely(jit040_selftesting))
		return jit040_bf_get(SELFTEST_MEM(src), bdata, offset, width);
	return x_get_bitfield(src, bdata, offset, width);
}
void jit040_put_bitfield(uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width)
{
	if (unlikely(jit040_selftesting))
		jit040_bf_put(SELFTEST_MEM(dst), bdata, val, offset, width);
	else
		x_put_bitfield(dst, bdata, val, offset, width);
}

/* Dropping blocks */

void jit040_flush(void)
{
	memset(hash, 0, sizeof(hash));
	memset(jit040_fast, 0, sizeof(jit040_fast));
	memset(page_blocks, 0, sizeof(page_blocks));
	memset(jit040_code_ram, 0, sizeof(jit040_code_ram));
	pool_used = 0;
	jit040_native_flush();
	jit040_recording = false;
	jit040_stop = 1;
}

static void unhash(struct jit040_block *b)
{
	struct jit040_block **p = &hash[hash_of(b->pc)];

	while (*p) {
		if (*p == b) {
			*p = b->next;
			break;
		}
		p = &(*p)->next;
	}
	b->dead = 1;
}

void jit040_invalidate_page(uae_u32 page)
{
	struct jit040_block *b;

	for (b = page_blocks[page]; b; b = b->page_next) {
		if (!b->dead)
			unhash(b);
	}
	page_blocks[page] = NULL;
	jit040_code_ram[page] = 0;
	if (jit040_recording && rec.page == (int)page)
		jit040_recording = false;  /* what was recorded may have changed */
	jit040_stop = 1;
}

/* RAM was allocated again: no code in it any more */
void jit040_memory_changed(uae_u8 *ram, uae_u32 size)
{
	jit040_ram_base = ram;
	jit040_ram_size = size < (JIT040_RAM_PAGES << 12) ? size : (JIT040_RAM_PAGES << 12);
	jit040_flush();
}

void jit040_set_active(bool on)
{
	jit040_flush();
	if (on && !pool) {
		pool = malloc(JIT040_POOL_SIZE);
		if (!pool)
			on = false;
		else if (!jit040_native_init())
			write_log("JIT: no native code, the block cache only\n");
		if (pool && getenv("PREVIOUSTURBO_JIT_SELFTEST"))
			jit040_selftest();
	}
	jit040_active = on;
}

/* A RAM page now holds code: its direct writes in the page cache go */
static void mark_code_page(int page, uae_u8 *host_page)
{
	int i;

	if (page < 0 || jit040_code_ram[page])
		return;
	jit040_code_ram[page] = 1;
	for (i = 0; i < PT_TLB_SIZE; i++) {
		if (pt_tlb[PT_TLB_WRITE][i].host == host_page)
			pt_tlb[PT_TLB_WRITE][i].tag = pt_tlb[PT_TLB_WRITE][i].jit_tag = 0;
	}
}

/* The code page for an instruction fetch at pc, through the page cache,
 * which is filled first if needed; NULL if it is not plain RAM or ROM or
 * the fetch would fault (the interpreter will then take the fault). */
static uae_u8 *code_page(uaecptr pc)
{
	struct pt_tlb_entry *e = PT_TLB_SLOT(PT_TLB_INS, pc);

	if (e->tag != PT_TLB_TAG(pc)) {
		volatile bool ok = true;
		TRY(prb) {
			uae_mmu040_get_iword(pc);
		} CATCH(prb) {
			ok = false;
		} ENDTRY
		if (!ok || e->tag != PT_TLB_TAG(pc))
			return NULL;
	}
	return e->host;
}

static int ram_page_of(uae_u8 *host_page)
{
	uintptr_t off = (uintptr_t)(host_page - jit040_ram_base);
	return off < jit040_ram_size ? (int)(off >> 12) : -1;
}

struct jit040_block *jit040_lookup(uaecptr pc)
{
	struct jit040_block *b;
	uae_u8 *host_page;

	for (b = hash[hash_of(pc)]; b; b = b->next) {
		if (b->pc == pc && b->s == regs.s)
			break;
	}
	if (!b)
		return NULL;
	host_page = code_page(pc);
	if (host_page != b->host_page)
		return NULL;
	jit040_fast[(pc >> 1) & (JIT040_FAST_SIZE - 1)] = b;
	return b;
}

/* Recording */

void jit040_record_start(uaecptr pc)
{
	uae_u8 *host_page = code_page(pc);

	if (!host_page)
		return;
	rec.pc = pc;
	rec.s = regs.s;
	rec.host_page = host_page;
	rec.page = ram_page_of(host_page);
	rec.n = 0;
	/* writes to the page from now on drop the recording */
	mark_code_page(rec.page, host_page);
	jit040_stop = 0;
	jit040_recording = true;
}

void jit040_record_end(void)
{
	struct jit040_block *b, **p;
	size_t size;

	if (!jit040_recording)
		return;
	jit040_recording = false;
	if (rec.n == 0)
		return;
	size = sizeof(*b) + rec.n * sizeof(struct jit040_ins);
	size = (size + 15) & ~(size_t)15;
	if (pool_used + size > JIT040_POOL_SIZE) {
		/* full: start again */
		int page = rec.page;
		jit040_flush();
		mark_code_page(page, rec.host_page);
	}
	b = (struct jit040_block *)(pool + pool_used);
	pool_used += size;

	b->pc = rec.pc;
	b->s = rec.s;
	b->dead = 0;
	b->page = rec.page;
	b->host_page = rec.host_page;
	b->hdelta = (uintptr_t)rec.host_page - (rec.pc & 0xfffff000);
	b->code = NULL;
	b->runs = 0;
	b->n = rec.n;
	memcpy(b->ins, rec.ins, rec.n * sizeof(struct jit040_ins));

	/* a block for the same place made before goes */
	for (p = &hash[hash_of(b->pc)]; *p; p = &(*p)->next) {
		if ((*p)->pc == b->pc && (*p)->s == b->s) {
			(*p)->dead = 1;
			*p = (*p)->next;
			break;
		}
	}
	b->next = hash[hash_of(b->pc)];
	hash[hash_of(b->pc)] = b;
	if (b->page >= 0) {
		b->page_next = page_blocks[b->page];
		page_blocks[b->page] = b;
	} else {
		b->page_next = NULL;
	}
}

/* After the interpreter ran the instruction at pc */
void jit040_record_step(uaecptr pc, uae_u16 opcode, int cycles)
{
	uaecptr next = m68k_getpci();
	uae_u32 len = next - pc;
	uae_u32 off = pc & 0xfff;
	struct jit040_ins *in;

	if (jit040_stop) {
		/* the MMU changed or the page was written */
		jit040_recording = false;
		return;
	}
	in = &rec.ins[rec.n];
	in->fn = jit040_functbl[opcode];
	in->opcode = opcode;
	in->cycles = cycles;
	if (regs.s == rec.s && len >= 2 && len <= JIT040_MAX_INS_LEN && off + len <= 0x1000) {
		/* went on to the next instruction */
		in->len = len;
		rec.n++;
		if (rec.n < JIT040_MAX_INS && off + len < 0x1000)
			return;
	} else if (off <= 0x1000 - JIT040_MAX_INS_LEN) {
		/* went elsewhere: the last instruction, which fits in the page */
		in->len = 0;
		rec.n++;
	}
	jit040_record_end();
}
