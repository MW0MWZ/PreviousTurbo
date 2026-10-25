/*
  PreviousTurbo - jit040.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Block cache for the 68040 with MMU, used in Turbo.
*/
#ifndef JIT040_H
#define JIT040_H

#define JIT040_MAX_INS 64

struct jit040_ins {
	cpuop_func *fn;     /* handler from cpuemu_36 */
	uae_u16 opcode;
	uae_u16 len;        /* bytes to the next instruction, 0 for the last one */
	uae_u16 cycles;     /* as the handler returned them when recorded */
};

typedef uae_u32 (*jit040_code)(void);

struct jit040_block {
	struct jit040_block *next;       /* in its hash chain */
	struct jit040_block *page_next;  /* in its RAM page's list */
	uaecptr pc;
	uae_u8 s;                        /* supervisor mode */
	uae_u8 dead;                     /* dropped, no longer in the hash */
	int page;                        /* RAM page, or -1 for ROM */
	uae_u8 *host_page;               /* the code page in the host's memory */
	uintptr_t hdelta;                /* host address - guest address */
	jit040_code code;                /* native code, once the block is used often */
	uae_u8 *chain;                   /* in it, where other blocks' code goes on */
	uae_u8 *chain_self;              /* and its own, its registers loaded */
	signed char cached[4];           /* the guest register in each host register kept, or -1 */
	uae_u16 runs;                    /* times run before it has native code */
	int n;
	struct jit040_ins ins[];
};

extern cpuop_func *jit040_functbl[65536];
extern bool jit040_active;
extern int jit040_stop;      /* leave the block after this instruction */
extern bool jit040_recording;
extern bool jit040_selftesting;
extern uae_u8 jit040_selftest_mem[4096 + 4];
extern uae_u8 *jit040_selftest_data;
extern uaecptr jit040_selftest_vdata;
extern struct flag_struct jit040_f;  /* the flags before the current instruction */

/* a bit field in memory at host, as get_bitfield and put_bitfield do it */
extern uae_u32 jit040_bf_get(uae_u8 *host, uae_u32 bdata[2], uae_s32 offset, int width);
extern void jit040_bf_put(uae_u8 *host, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width);

/* runs of a block before it is compiled to native code */
#define JIT040_HOT 16
/* cycles the native code may run on from block to block */
#define JIT040_CHAIN_CYCLES 1024

extern void jit040_build_functbl(const struct cputbl *tbl, const struct cputbl *jittbl);
extern void jit040_set_active(bool on);
extern void jit040_flush(void);
extern void jit040_invalidate_page(uae_u32 page);
extern void jit040_memory_changed(uae_u8 *ram, uae_u32 size);

extern struct jit040_block *jit040_lookup(uaecptr pc);

/* The blocks found last, by address: the usual case of jit040_lookup */
#define JIT040_FAST_SIZE 16384
extern struct jit040_block *jit040_fast[JIT040_FAST_SIZE];

static inline struct jit040_block *jit040_find(uaecptr pc)
{
	struct jit040_block *b = jit040_fast[(pc >> 1) & (JIT040_FAST_SIZE - 1)];

	if (b && b->pc == pc && b->s == regs.s && !b->dead) {
		struct pt_tlb_entry *e = PT_TLB_SLOT(PT_TLB_INS, pc);
		if (e->tag == PT_TLB_TAG(pc) && e->host == b->host_page)
			return b;
	}
	return jit040_lookup(pc);
}
extern void jit040_record_start(uaecptr pc);
extern void jit040_record_step(uaecptr pc, uae_u16 opcode, int cycles);

/* jit040_native.c */
extern bool jit040_native_init(void);
extern void jit040_native_flush(void);
extern bool jit040_compile(struct jit040_block *b);
extern uae_u8 *jit040_link_site;
extern struct jit040_block *jit040_link_from;
extern void jit040_link(void);
extern void jit040_selftest(void);
extern void jit040_record_end(void);

#endif /* JIT040_H */
