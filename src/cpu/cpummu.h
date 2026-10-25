/*
 * cpummu.h - MMU emulation
 *
 * Copyright (c) 2001-2004 Milan Jurik of ARAnyM dev team (see AUTHORS)
 * 
 * Inspired by UAE MMU patch
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * ARAnyM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ARAnyM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ARAnyM; if not, write to the Free Software Foundation,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */


#ifndef UAE_CPUMMU_H
#define UAE_CPUMMU_H

#include "uae/types.h"

#define MMU_ICACHE 0
#define MMU_IPAGECACHE 0
#define MMU_DPAGECACHE 0

#define CACHE_HIT_COUNT 0

#include "mmu_common.h"

#ifndef FULLMMU
#define FULLMMU
#endif

#define DUNUSED(x)
#define D
#ifndef bug
#if DEBUG
#define bug write_log
#else
#define bug
#endif
#endif

static __inline void flush_internals (void) { }

extern int mmu060_state;

extern int mmu040_movem;
extern uaecptr mmu040_movem_ea;
extern uae_u32 mmu040_move16[4];

extern bool mmu_pagesize_8k;
extern int mmu_pageshift, mmu_pageshift1m;
extern uae_u16 mmu_opcode;
extern bool mmu_restart;
extern bool mmu_ttr_enabled, mmu_ttr_enabled_ins, mmu_ttr_enabled_data;
extern bool rmw_cycle;
extern uae_u8 mmu_cache_state;
extern uae_u8 cache_default_ins, cache_default_data;

extern void mmu_dump_tables(void);

#define MMU_TTR_LOGICAL_BASE				0xff000000
#define MMU_TTR_LOGICAL_MASK				0x00ff0000
#define MMU_TTR_BIT_ENABLED					(1 << 15)
#define MMU_TTR_BIT_SFIELD_ENABLED			(1 << 14)
#define MMU_TTR_BIT_SFIELD_SUPER			(1 << 13)
#define MMU_TTR_SFIELD_SHIFT				13
#define MMU_TTR_UX_MASK						((1 << 9) | (1 << 8))
#define MMU_TTR_UX_SHIFT					8
#define MMU_TTR_CACHE_MASK					((1 << 6) | (1 << 5))
#define MMU_TTR_CACHE_SHIFT					5
#define MMU_TTR_CACHE_DISABLE				(1 << 6)
#define MMU_TTR_CACHE_MODE					(1 << 5)
#define MMU_TTR_BIT_WRITE_PROTECT			(1 << 2)

#define MMU_UDT_MASK	3
#define MMU_PDT_MASK	3

#define MMU_DES_WP			4
#define MMU_DES_USED		8

/* page descriptors only */
#define MMU_DES_MODIFIED	16
#define MMU_DES_SUPER		(1 << 7)
#define MMU_DES_GLOBAL		(1 << 10)

#define MMU_ROOT_PTR_ADDR_MASK			0xfffffe00
#define MMU_PTR_PAGE_ADDR_MASK_8		0xffffff80
#define MMU_PTR_PAGE_ADDR_MASK_4		0xffffff00

#define MMU_PAGE_INDIRECT_MASK			0xfffffffc
#define MMU_PAGE_ADDR_MASK_8			0xffffe000
#define MMU_PAGE_ADDR_MASK_4			0xfffff000
#define MMU_PAGE_UR_MASK_8				((1 << 12) | (1 << 11))
#define MMU_PAGE_UR_MASK_4				(1 << 11)
#define MMU_PAGE_UR_SHIFT				11

#define MMU_MMUSR_ADDR_MASK				0xfffff000
#define MMU_MMUSR_B						(1 << 11)
#define MMU_MMUSR_G						(1 << 10)
#define MMU_MMUSR_U1					(1 << 9)
#define MMU_MMUSR_U0					(1 << 8)
#define MMU_MMUSR_Ux					(MMU_MMUSR_U1 | MMU_MMUSR_U0)
#define MMU_MMUSR_S						(1 << 7)
#define MMU_MMUSR_CM					((1 << 6) | ( 1 << 5))
#define MMU_MMUSR_CM_DISABLE			(1 << 6)
#define MMU_MMUSR_CM_MODE				(1 << 5)
#define MMU_MMUSR_M						(1 << 4)
#define MMU_MMUSR_W						(1 << 2)
#define MMU_MMUSR_T						(1 << 1)
#define MMU_MMUSR_R						(1 << 0)

// 68040 and 68060
#define MMU_TCR_E						0x8000
#define MMU_TCR_P						0x4000
// 68060 only
#define MMU_TCR_NAD						0x2000
#define MMU_TCR_NAI						0x1000
#define MMU_TCR_FOTC					0x0800
#define MMU_TCR_FITC					0x0400
#define MMU_TCR_DCO1					0x0200
#define MMU_TCR_DCO0					0x0100
#define MMU_TCR_DUO1					0x0080
#define MMU_TCR_DUO0					0x0040
#define MMU_TCR_DWO						0x0020
#define MMU_TCR_DCI1					0x0010
#define MMU_TCR_DCI0					0x0008
#define MMU_TCR_DUI1					0x0004
#define MMU_TCR_DUI0					0x0002

#define TTR_I0	4
#define TTR_I1	5
#define TTR_D0	6
#define TTR_D1	7

#define TTR_NO_MATCH	0
#define TTR_NO_WRITE	1
#define TTR_OK_MATCH	2

struct mmu_atc_line {
	uaecptr tag; // tag is 16 or 17 bits S+logical
	uae_u32 valid;
	uae_u32 status;
	uaecptr phys; // phys base address
};

/*
 * 68040 ATC is a 4 way 16 slot associative address translation cache
 * the 68040 has a DATA and an INSTRUCTION ATC.
 * an ATC lookup may result in : a hit, a miss and a modified state.
 * the 68060 can disable ATC allocation
 * we must take care of 8k and 4k page size, index position is relative to page size
 */

#define ATC_WAYS 4
#define ATC_SLOTS 16
#define ATC_TYPE 2

extern uae_u32 mmu_is_super;
extern uae_u32 mmu_tagmask, mmu_pagemask, mmu_pagemaski;
extern struct mmu_atc_line mmu_atc_array[ATC_TYPE][ATC_SLOTS][ATC_WAYS];

extern void mmu_tt_modified(void);
extern int mmu_match_ttr_ins(uaecptr addr, bool super);
extern int mmu_match_ttr(uaecptr addr, bool super, bool data);
extern void mmu_bus_error_ttr_write_fault(uaecptr addr, bool super, bool data, uae_u32 val, int size);
extern int mmu_match_ttr_write(uaecptr addr, bool super, bool data, uae_u32 val, int size);
extern int mmu_match_ttr_maybe_write(uaecptr addr, bool super, bool data, int size, bool write);
extern uaecptr mmu_translate(uaecptr addr, uae_u32 val, bool super, bool data, bool write, int size);
extern void mmu_hardware_bus_error(uaecptr addr, uae_u32 v, bool read, bool ins, int size);
extern bool mmu_is_super_access(bool read);

extern uae_u32 REGPARAM3 mmu060_get_rmw_bitfield (uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width) REGPARAM;
extern void REGPARAM3 mmu060_put_rmw_bitfield (uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width) REGPARAM;

extern uae_u16 REGPARAM3 mmu_get_word_unaligned(uaecptr addr, bool data) REGPARAM;
extern uae_u32 REGPARAM3 mmu_get_long_unaligned(uaecptr addr, bool data) REGPARAM;

extern uae_u32 REGPARAM3 mmu_get_ilong_unaligned(uaecptr addr) REGPARAM;

extern void REGPARAM3 mmu_put_word_unaligned(uaecptr addr, uae_u16 val, bool data) REGPARAM;
extern void REGPARAM3 mmu_put_long_unaligned(uaecptr addr, uae_u32 val, bool data) REGPARAM;

extern void mmu_make_transparent_region(uaecptr baseaddr, uae_u32 size, int datamode);

#define FC_DATA		(regs.s ? 5 : 1)
#define FC_INST		(regs.s ? 6 : 2)

extern void mmu_bus_error(uaecptr addr, uae_u32 val, int fc, bool write, int size, uae_u32 status, bool nonmmu);

extern uae_u32 REGPARAM3 sfc_get_long(uaecptr addr) REGPARAM;
extern uae_u16 REGPARAM3 sfc_get_word(uaecptr addr) REGPARAM;
extern uae_u8 REGPARAM3 sfc_get_byte(uaecptr addr) REGPARAM;
extern void REGPARAM3 dfc_put_long(uaecptr addr, uae_u32 val) REGPARAM;
extern void REGPARAM3 dfc_put_word(uaecptr addr, uae_u16 val) REGPARAM;
extern void REGPARAM3 dfc_put_byte(uaecptr addr, uae_u8 val) REGPARAM;

#define sfc040_get_long sfc_get_long
#define sfc040_get_word sfc_get_word
#define sfc040_get_byte sfc_get_byte
#define dfc040_put_long dfc_put_long
#define dfc040_put_word dfc_put_word
#define dfc040_put_byte dfc_put_byte

#define sfc060_get_long sfc_get_long
#define sfc060_get_word sfc_get_word
#define sfc060_get_byte sfc_get_byte
#define dfc060_put_long dfc_put_long
#define dfc060_put_word dfc_put_word
#define dfc060_put_byte dfc_put_byte

extern void uae_mmu_put_lrmw (uaecptr addr, uae_u32 v, int size, int type);
extern uae_u32 uae_mmu_get_lrmw (uaecptr addr, int size, int type);

extern void REGPARAM3 mmu_flush_atc(uaecptr addr, bool super, bool global) REGPARAM;
extern void REGPARAM3 mmu_flush_atc_all(bool global) REGPARAM;
extern void REGPARAM3 mmu_op_real(uae_u32 opcode, uae_u16 extra) REGPARAM;

extern void REGPARAM3 mmu_reset(void) REGPARAM;
extern void REGPARAM3 mmu_set_funcs(void) REGPARAM;
extern uae_u16 REGPARAM3 mmu_set_tc(uae_u16 tc) REGPARAM;
extern void REGPARAM3 mmu_set_super(bool super) REGPARAM;
extern void REGPARAM3 mmu_flush_cache(void) REGPARAM;

static ALWAYS_INLINE uaecptr mmu_get_real_address(uaecptr addr, struct mmu_atc_line *cl)
{
	return cl->phys | (addr & mmu_pagemask);
}

extern void mmu_get_move16(uaecptr addr, uae_u32 *v, bool data, int size);
extern void mmu_put_move16(uaecptr addr, uae_u32 *val, bool data, int size);

extern uaecptr debug_mmu_translate(uaecptr addr, uae_u32 val, bool super, bool data, bool write, int size, struct mmu_debug_data **mdd);
extern void debug_mmu_translate_end(void);

#if MMU_IPAGECACHE
extern uae_u32 atc_last_ins_laddr, atc_last_ins_paddr;
extern uae_u8 atc_last_ins_cache;
#endif

#if MMU_DPAGECACHE
#define MMUFASTCACHE_ENTRIES 256
struct mmufastcache
{
	uae_u32 log;
	uae_u32 phys;
	uae_u8 cache_state;
};
extern struct mmufastcache atc_data_cache_read[MMUFASTCACHE_ENTRIES];
extern struct mmufastcache atc_data_cache_write[MMUFASTCACHE_ENTRIES];
#endif

#if CACHE_HIT_COUNT
extern int mmu_ins_hit, mmu_ins_miss;
extern int mmu_data_read_hit, mmu_data_read_miss;
extern int mmu_data_write_hit, mmu_data_write_miss;
#endif


/* PreviousTurbo: in Turbo, a page cache in front of the transparent
 * translation check and the ATC. An entry is made only after an access
 * went the full way without a fault, so used and modified bits, write
 * protection and faults behave as before; it is emptied with the ATC and
 * whenever the MMU registers change. */
#define PT_TLB_SIZE 1024
#define PT_TLB_INS   0
#define PT_TLB_READ  1
#define PT_TLB_WRITE 2
struct pt_tlb_entry {
	uae_u32 tag;
	uae_u32 phys;
	uae_u8 *host;   /* the page in the host's memory, if plain RAM/ROM */
	uae_u8 cache;
	uae_u32 jit_tag;     /* for the native code: the tag if host is set, else 0 */
	uintptr_t jit_host;  /* and host less the page's virtual address */
};
extern struct pt_tlb_entry pt_tlb[3][PT_TLB_SIZE];
extern bool pt_tlb_on;
extern bool pt_tlb_direct;  /* no cache or cycle-exact emulation in between */
extern uae_u8 *mem_host_read[65536];
extern uae_u8 *mem_host_write[65536];
extern void pt_tlb_flush(void);
#define PT_TLB_TAG(addr) (((addr) & 0xfffff000) | (regs.s ? 3 : 2))
#define PT_TLB_SLOT(type, addr) (&pt_tlb[type][((addr) >> 12) & (PT_TLB_SIZE - 1)])

#define PT_TLB_LOOKUP(type, getter, hostget) \
	struct pt_tlb_entry *pte = NULL; \
	uae_u32 ptag = 0; \
	if (pt_tlb_on && (type) >= 0) { \
		pte = PT_TLB_SLOT(type, addr); \
		ptag = PT_TLB_TAG(addr); \
		if (pte->tag == ptag) { \
			mmu_cache_state = pte->cache; \
			if (pte->host) \
				return hostget(pte->host + (addr & 0xfff)); \
			return getter(pte->phys | (addr & 0xfff)); \
		} \
	}
#define PT_TLB_STORE_LOOKUP(type, putter, hostput) \
	struct pt_tlb_entry *pte = NULL; \
	uae_u32 ptag = 0; \
	if (pt_tlb_on && (type) >= 0) { \
		pte = PT_TLB_SLOT(type, addr); \
		ptag = PT_TLB_TAG(addr); \
		if (pte->tag == ptag) { \
			mmu_cache_state = pte->cache; \
			if (pte->host) \
				hostput(pte->host + (addr & 0xfff), val); \
			else \
				putter(pte->phys | (addr & 0xfff), val); \
			return; \
		} \
	}
#define PT_HOST_GET_BYTE(p) (*(p))
#define PT_HOST_PUT_BYTE(p, v) (*(p) = (uae_u8)(v))
#define PT_TLB_FILL(table) \
	if (pte) { \
		uae_u8 *bank = pt_tlb_direct ? table[addr >> 16] : NULL; \
		pte->tag = ptag; \
		pte->phys = addr & 0xfffff000; \
		pte->host = bank ? bank + (addr & 0xf000) : NULL; \
		pte->cache = mmu_cache_state; \
		PT_TLB_JIT(pte); \
	}
#define PT_TLB_FILL_WRITE() \
	PT_TLB_FILL(mem_host_write) \
	if (pte && pte->host && jit040_is_code(pte->host)) { \
		pte->host = NULL; \
		pte->jit_tag = 0; \
	}
/* jit_tag and jit_host from tag and host */
#define PT_TLB_JIT(pte) \
	(pte)->jit_tag = (pte)->host ? (pte)->tag : 0; \
	(pte)->jit_host = (uintptr_t)(pte)->host - ((pte)->tag & 0xfffff000)

/* PreviousTurbo block cache (jit040.c): RAM pages that hold code blocks.
 * Writes to them go the full way, past the RAM bank functions, which drop
 * the blocks. */
extern uae_u8 *jit040_ram_base;
extern uae_u32 jit040_ram_size;
extern uae_u8 jit040_code_ram[];
static ALWAYS_INLINE int jit040_is_code(const uae_u8 *host)
{
	uintptr_t off = (uintptr_t)(host - jit040_ram_base);
	return off < jit040_ram_size && jit040_code_ram[off >> 12];
}

static ALWAYS_INLINE uae_u32 mmu_get_ilong(uaecptr addr, int size)
{
	PT_TLB_LOOKUP(PT_TLB_INS, x_phys_get_ilong, do_get_mem_long)
	mmu_cache_state = cache_default_ins;
	if ((!mmu_ttr_enabled_ins || mmu_match_ttr_ins(addr,regs.s!=0) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_IPAGECACHE
		if (((addr & mmu_pagemaski) | regs.s) == atc_last_ins_laddr) {
#if CACHE_HIT_COUNT
			mmu_ins_hit++;
#endif
			addr = atc_last_ins_paddr | (addr & mmu_pagemask);
			mmu_cache_state = atc_last_ins_cache;
		} else {
#if CACHE_HIT_COUNT
			mmu_ins_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, false, false, size);
#if MMU_IPAGECACHE
		}
#endif
	}
	PT_TLB_FILL(mem_host_read)
	return x_phys_get_ilong(addr);
}

static ALWAYS_INLINE uae_u16 mmu_get_iword(uaecptr addr, int size)
{
	PT_TLB_LOOKUP(PT_TLB_INS, x_phys_get_iword, do_get_mem_word)
	mmu_cache_state = cache_default_ins;
	if ((!mmu_ttr_enabled_ins || mmu_match_ttr_ins(addr,regs.s!=0) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_IPAGECACHE
		if (((addr & mmu_pagemaski) | regs.s) == atc_last_ins_laddr) {
#if CACHE_HIT_COUNT
			mmu_ins_hit++;
#endif
			addr = atc_last_ins_paddr | (addr & mmu_pagemask);
			mmu_cache_state = atc_last_ins_cache;
		} else {
#if CACHE_HIT_COUNT
			mmu_ins_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, false, false, size);
#if MMU_IPAGECACHE
		}
#endif
	}
	PT_TLB_FILL(mem_host_read)
	return x_phys_get_iword(addr);
}


static ALWAYS_INLINE uae_u32 mmu_get_long(uaecptr addr, bool data, int size)
{
	PT_TLB_LOOKUP((data ? PT_TLB_READ : PT_TLB_INS), x_phys_get_long, do_get_mem_long)
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr(addr,regs.s!=0,data) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, data, false, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	PT_TLB_FILL(mem_host_read)
	return x_phys_get_long(addr);
}

static ALWAYS_INLINE uae_u16 mmu_get_word(uaecptr addr, bool data, int size)
{
	PT_TLB_LOOKUP((data ? PT_TLB_READ : PT_TLB_INS), x_phys_get_word, do_get_mem_word)
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr(addr,regs.s!=0,data) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, data, false, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	PT_TLB_FILL(mem_host_read)
	return x_phys_get_word(addr);
}

static ALWAYS_INLINE uae_u8 mmu_get_byte(uaecptr addr, bool data, int size)
{
	PT_TLB_LOOKUP((data ? PT_TLB_READ : PT_TLB_INS), x_phys_get_byte, PT_HOST_GET_BYTE)
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr(addr,regs.s!=0,data) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, regs.s!=0, data, false, size);
#if MMU_DPAGECACHE
		}
#endif
}
	PT_TLB_FILL(mem_host_read)
	return x_phys_get_byte(addr);
}

static ALWAYS_INLINE void mmu_put_long(uaecptr addr, uae_u32 val, bool data, int size)
{
	PT_TLB_STORE_LOOKUP((data ? PT_TLB_WRITE : -1), x_phys_put_long, do_put_mem_long)
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,regs.s!=0,data,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, regs.s!=0, data, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	PT_TLB_FILL_WRITE()
	x_phys_put_long(addr, val);
}

static ALWAYS_INLINE void mmu_put_word(uaecptr addr, uae_u16 val, bool data, int size)
{
	PT_TLB_STORE_LOOKUP((data ? PT_TLB_WRITE : -1), x_phys_put_word, do_put_mem_word)
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,regs.s!=0,data,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, regs.s!=0, data, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	PT_TLB_FILL_WRITE()
	x_phys_put_word(addr, val);
}

static ALWAYS_INLINE void mmu_put_byte(uaecptr addr, uae_u8 val, bool data, int size)
{
	PT_TLB_STORE_LOOKUP((data ? PT_TLB_WRITE : -1), x_phys_put_byte, PT_HOST_PUT_BYTE)
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,regs.s!=0,data,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | regs.s;
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, regs.s!=0, data, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	PT_TLB_FILL_WRITE()
	x_phys_put_byte(addr, val);
}

static ALWAYS_INLINE uae_u32 mmu_get_user_long(uaecptr addr, bool super, bool write, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_maybe_write(addr,super,true,size,write) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, super, true, write, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	return x_phys_get_long(addr);
}

static ALWAYS_INLINE uae_u16 mmu_get_user_word(uaecptr addr, bool super, bool write, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_maybe_write(addr,super,true,size,write) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, super, true, write, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	return x_phys_get_word(addr);
}

static ALWAYS_INLINE uae_u8 mmu_get_user_byte(uaecptr addr, bool super, bool write, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_maybe_write(addr,super,true,size,write) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_read[idx2].log == idx1) {
			addr = atc_data_cache_read[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_read_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_read_miss++;
#endif
#endif
			addr = mmu_translate(addr, 0, super, true, write, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	return x_phys_get_byte(addr);
}

static ALWAYS_INLINE void mmu_put_user_long(uaecptr addr, uae_u32 val, bool super, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,super,true,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, super, true, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	x_phys_put_long(addr, val);
}

static ALWAYS_INLINE void mmu_put_user_word(uaecptr addr, uae_u16 val, bool super, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,super,true,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, super, true, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	x_phys_put_word(addr, val);
}

static ALWAYS_INLINE void mmu_put_user_byte(uaecptr addr, uae_u8 val, bool super, int size, bool ci)
{
	mmu_cache_state = cache_default_data;
	if ((!mmu_ttr_enabled || mmu_match_ttr_write(addr,super,true,val,size) == TTR_NO_MATCH) && regs.mmu_enabled) {
#if MMU_DPAGECACHE
		uae_u32 idx1 = ((addr & mmu_pagemaski) >> mmu_pageshift1m) | (super ? 1 : 0);
		uae_u32 idx2 = idx1 & (MMUFASTCACHE_ENTRIES - 1);
		if (atc_data_cache_write[idx2].log == idx1) {
			addr = atc_data_cache_write[idx2].phys | (addr & mmu_pagemask);
			mmu_cache_state = atc_data_cache_read[idx2].cache_state;
#if CACHE_HIT_COUNT
			mmu_data_write_hit++;
#endif
		} else {
#if CACHE_HIT_COUNT
			mmu_data_write_miss++;
#endif
#endif
			addr = mmu_translate(addr, val, super, true, true, size);
#if MMU_DPAGECACHE
		}
#endif
	}
	if (ci)
		mmu_cache_state = CACHE_DISABLE_MMU;
	x_phys_put_byte(addr, val);
}


static ALWAYS_INLINE void HWput_l(uaecptr addr, uae_u32 l)
{
	put_long (addr, l);
}
static ALWAYS_INLINE void HWput_w(uaecptr addr, uae_u32 w)
{
	put_word (addr, w);
}
static ALWAYS_INLINE void HWput_b(uaecptr addr, uae_u32 b)
{
	put_byte (addr, b);
}
static ALWAYS_INLINE uae_u32 HWget_l(uaecptr addr)
{
	return get_long (addr);
}
static ALWAYS_INLINE uae_u32 HWget_w(uaecptr addr)
{
	return get_word (addr);
}
static ALWAYS_INLINE uae_u32 HWget_b(uaecptr addr)
{
	return get_byte (addr);
}

#if MMU_ICACHE
#define MMU_ICACHE_SZ 4096
struct mmu_icache
{
	uae_u16 data;
	uae_u32 addr;
};

extern struct mmu_icache mmu_icache_data[MMU_ICACHE_SZ];

static ALWAYS_INLINE uae_u16 uae_mmu040_getc_iword(uaecptr addr)
{
	int icidx = (addr & (MMU_ICACHE_SZ - 1)) | regs.s;
	if (addr != mmu_icache_data[icidx].addr || !(regs.cacr & 0x8000)) {
		mmu_icache_data[icidx].data = mmu_get_iword(addr, sz_word);
		mmu_icache_data[icidx].addr = addr;
		return mmu_icache_data[icidx].data;
	} else {
		return mmu_icache_data[icidx].data;
	}
}
#endif

static ALWAYS_INLINE uae_u16 uae_mmu040_get_iword(uaecptr addr)
{
#if MMU_ICACHE
	return uae_mmu040_getc_iword(addr);
#else
	return mmu_get_iword(addr, sz_word);
#endif
}
static ALWAYS_INLINE uae_u32 uae_mmu040_get_ilong(uaecptr addr)
{
#if MMU_ICACHE
	uae_u32 result = uae_mmu040_getc_iword(addr);
	result <<= 16;
	result |= uae_mmu040_getc_iword(addr + 2);
	return result;
#else
	if (unlikely(is_unaligned_page(addr, 4)))
		return mmu_get_ilong_unaligned(addr);
	return mmu_get_ilong(addr, sz_long);
#endif
}
static ALWAYS_INLINE uae_u16 uae_mmu040_get_ibyte(uaecptr addr)
{
#if MMU_ICACHE
	uae_u16 result = uae_mmu040_getc_iword(addr & ~1);
	return (addr & 1) ? result & 0xFF : result >> 8;
#else
	return mmu_get_byte(addr, false, sz_byte);
#endif
}
static ALWAYS_INLINE uae_u32 uae_mmu040_get_long(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		return mmu_get_long_unaligned(addr, true);
	return mmu_get_long(addr, true, sz_long);
}
static ALWAYS_INLINE uae_u16 uae_mmu040_get_word(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 2)))
		return mmu_get_word_unaligned(addr, true);
	return mmu_get_word(addr, true, sz_word);
}
static ALWAYS_INLINE uae_u8 uae_mmu040_get_byte(uaecptr addr)
{
	return mmu_get_byte(addr, true, sz_byte);
}

static ALWAYS_INLINE void uae_mmu040_put_word(uaecptr addr, uae_u16 val)
{
	if (unlikely(is_unaligned_page(addr, 2)))
		mmu_put_word_unaligned(addr, val, true);
	else
		mmu_put_word(addr, val, true, sz_word);
}
static ALWAYS_INLINE void uae_mmu040_put_byte(uaecptr addr, uae_u8 val)
{
	mmu_put_byte(addr, val, true, sz_byte);
}
static ALWAYS_INLINE void uae_mmu040_put_long(uaecptr addr, uae_u32 val)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		mmu_put_long_unaligned(addr, val, true);
	else
		mmu_put_long(addr, val, true, sz_long);
}


static ALWAYS_INLINE uae_u32 uae_mmu060_get_ilong(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		return mmu_get_ilong_unaligned(addr);
	return mmu_get_ilong(addr, sz_long);
}
static ALWAYS_INLINE uae_u16 uae_mmu060_get_iword(uaecptr addr)
{
	return mmu_get_iword(addr, sz_word);
}
static ALWAYS_INLINE uae_u16 uae_mmu060_get_ibyte(uaecptr addr)
{
	return mmu_get_byte(addr, false, sz_byte);
}
static ALWAYS_INLINE uae_u32 uae_mmu060_get_long(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		return mmu_get_long_unaligned(addr, true);
	return mmu_get_long(addr, true, sz_long);
}
static ALWAYS_INLINE uae_u16 uae_mmu060_get_word(uaecptr addr)
{
	if (unlikely(is_unaligned_page(addr, 2)))
		return mmu_get_word_unaligned(addr, true);
	return mmu_get_word(addr, true, sz_word);
}
static ALWAYS_INLINE uae_u8 uae_mmu060_get_byte(uaecptr addr)
{
	return mmu_get_byte(addr, true, sz_byte);
}
static ALWAYS_INLINE void uae_mmu_get_move16(uaecptr addr, uae_u32 *val)
{
	// move16 is always aligned
	mmu_get_move16(addr, val, true, 16);
}

static ALWAYS_INLINE void uae_mmu060_put_long(uaecptr addr, uae_u32 val)
{
	if (unlikely(is_unaligned_page(addr, 4)))
		mmu_put_long_unaligned(addr, val, true);
	else
		mmu_put_long(addr, val, true, sz_long);
}
static ALWAYS_INLINE void uae_mmu060_put_word(uaecptr addr, uae_u16 val)
{
	if (unlikely(is_unaligned_page(addr, 2)))
		mmu_put_word_unaligned(addr, val, true);
	else
		mmu_put_word(addr, val, true, sz_word);
}
static ALWAYS_INLINE void uae_mmu060_put_byte(uaecptr addr, uae_u8 val)
{
	mmu_put_byte(addr, val, true, sz_byte);
}
static ALWAYS_INLINE void uae_mmu_put_move16(uaecptr addr, uae_u32 *val)
{
	// move16 is always aligned
	mmu_put_move16(addr, val, true, 16);
}

// normal 040
STATIC_INLINE void put_byte_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu040_put_byte (addr, v);
}
STATIC_INLINE void put_word_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu040_put_word (addr, v);
}
STATIC_INLINE void put_long_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu040_put_long (addr, v);
}
STATIC_INLINE uae_u32 get_byte_mmu040 (uaecptr addr)
{
	return uae_mmu040_get_byte (addr);
}
STATIC_INLINE uae_u32 get_word_mmu040 (uaecptr addr)
{
	return uae_mmu040_get_word (addr);
}
STATIC_INLINE uae_u32 get_long_mmu040 (uaecptr addr)
{
	return uae_mmu040_get_long (addr);
}
/* PreviousTurbo block cache: the handlers of cpuemu_36 read the instruction
 * words from the host's copy of the code page, which was checked when the
 * block was entered (a block's instructions never leave its page), and try
 * the page cache before the full data access. */
extern uintptr_t jit040_hdelta;
#define JIT_IPTR(addr) ((uae_u8 *)(jit040_hdelta + (uae_u32)(addr)))
STATIC_INLINE uae_u32 get_ibyte_jit (int o)
{
	return do_get_mem_word (JIT_IPTR (m68k_getpci () + o));
}
STATIC_INLINE uae_u32 get_iword_jit (int o)
{
	return do_get_mem_word (JIT_IPTR (m68k_getpci () + o));
}
STATIC_INLINE uae_u32 get_ilong_jit (int o)
{
	return do_get_mem_long (JIT_IPTR (m68k_getpci () + o));
}
STATIC_INLINE uae_u32 next_iword_jit (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (2);
	return do_get_mem_word (JIT_IPTR (pc));
}
STATIC_INLINE uae_u32 next_ilong_jit (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (4);
	return do_get_mem_long (JIT_IPTR (pc));
}
/* The full accesses, kept out of line */
extern uae_u32 jit040_get_byte (uaecptr addr);
extern uae_u32 jit040_get_word (uaecptr addr);
extern uae_u32 jit040_get_long (uaecptr addr);
extern void jit040_put_byte (uaecptr addr, uae_u32 v);
extern void jit040_put_word (uaecptr addr, uae_u32 v);
extern void jit040_put_long (uaecptr addr, uae_u32 v);
/* and the others they use, as the 68040 MMU's (but in the self-test) */
extern uae_u32 REGPARAM3 jit040_get_disp_ea_020 (uae_u32 base, int idx) REGPARAM;
extern uae_u32 jit040_get_lrmw_byte (uaecptr addr);
extern uae_u32 jit040_get_lrmw_word (uaecptr addr);
extern uae_u32 jit040_get_lrmw_long (uaecptr addr);
extern void jit040_put_lrmw_byte (uaecptr addr, uae_u32 v);
extern void jit040_put_lrmw_word (uaecptr addr, uae_u32 v);
extern void jit040_put_lrmw_long (uaecptr addr, uae_u32 v);
extern void jit040_get_move16 (uaecptr addr, uae_u32 *v);
extern void jit040_put_move16 (uaecptr addr, uae_u32 *v);
extern uae_u32 jit040_get_bitfield (uae_u32 src, uae_u32 bdata[2], uae_s32 offset, int width);
extern void jit040_put_bitfield (uae_u32 dst, uae_u32 bdata[2], uae_u32 val, uae_s32 offset, int width);
/* The host address of a data access the page cache can do directly, or NULL */
static ALWAYS_INLINE uae_u8 *pt_tlb_host (int type, uaecptr addr, int size)
{
	struct pt_tlb_entry *pte = PT_TLB_SLOT (type, addr);
	if (likely (pte->tag == PT_TLB_TAG (addr) && pte->host) && (int)(addr & 0xfff) <= 0x1000 - size)
		return pte->host + (addr & 0xfff);
	return NULL;
}
STATIC_INLINE uae_u32 get_byte_jit (uaecptr addr)
{
	uae_u8 *p = pt_tlb_host (PT_TLB_READ, addr, 1);
	return p ? *p : jit040_get_byte (addr);
}
STATIC_INLINE uae_u32 get_word_jit (uaecptr addr)
{
	uae_u8 *p = pt_tlb_host (PT_TLB_READ, addr, 2);
	return p ? do_get_mem_word (p) : jit040_get_word (addr);
}
STATIC_INLINE uae_u32 get_long_jit (uaecptr addr)
{
	uae_u8 *p = pt_tlb_host (PT_TLB_READ, addr, 4);
	return p ? do_get_mem_long (p) : jit040_get_long (addr);
}
STATIC_INLINE void put_byte_jit (uaecptr addr, uae_u32 v)
{
	uae_u8 *p = pt_tlb_host (PT_TLB_WRITE, addr, 1);
	if (p)
		*p = (uae_u8)v;
	else
		jit040_put_byte (addr, v);
}
STATIC_INLINE void put_word_jit (uaecptr addr, uae_u32 v)
{
	uae_u8 *p = pt_tlb_host (PT_TLB_WRITE, addr, 2);
	if (p)
		do_put_mem_word (p, v);
	else
		jit040_put_word (addr, v);
}
STATIC_INLINE void put_long_jit (uaecptr addr, uae_u32 v)
{
	uae_u8 *p = pt_tlb_host (PT_TLB_WRITE, addr, 4);
	if (p)
		do_put_mem_long (p, v);
	else
		jit040_put_long (addr, v);
}

// normal 060
STATIC_INLINE void put_byte_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu060_put_byte (addr, v);
}
STATIC_INLINE void put_word_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu060_put_word (addr, v);
}
STATIC_INLINE void put_long_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu060_put_long (addr, v);
}
STATIC_INLINE uae_u32 get_byte_mmu060 (uaecptr addr)
{
	return uae_mmu060_get_byte (addr);
}
STATIC_INLINE uae_u32 get_word_mmu060 (uaecptr addr)
{
	return uae_mmu060_get_word (addr);
}
STATIC_INLINE uae_u32 get_long_mmu060 (uaecptr addr)
{
	return uae_mmu060_get_long (addr);
}

STATIC_INLINE void get_move16_mmu (uaecptr addr, uae_u32 *v)
{
	uae_mmu_get_move16 (addr, v);
}
STATIC_INLINE void put_move16_mmu (uaecptr addr, uae_u32 *v)
{
	uae_mmu_put_move16 (addr, v);
}

// locked rmw 060
STATIC_INLINE void put_lrmw_byte_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_byte, 1);
}
STATIC_INLINE void put_lrmw_word_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_word, 1);
}
STATIC_INLINE void put_lrmw_long_mmu060 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_long, 1);
}
STATIC_INLINE uae_u32 get_lrmw_byte_mmu060 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_byte, 1);
}
STATIC_INLINE uae_u32 get_lrmw_word_mmu060 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_word, 1);
}
STATIC_INLINE uae_u32 get_lrmw_long_mmu060 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_long, 1);
}
// normal rmw 060
STATIC_INLINE void put_rmw_byte_mmu060 (uaecptr addr, uae_u32 v)
{
	rmw_cycle = true;
	uae_mmu060_put_byte (addr, v);
	rmw_cycle = false;
}
STATIC_INLINE void put_rmw_word_mmu060 (uaecptr addr, uae_u32 v)
{
	rmw_cycle = true;
	uae_mmu060_put_word (addr, v);
	rmw_cycle = false;
}
STATIC_INLINE void put_rmw_long_mmu060 (uaecptr addr, uae_u32 v)
{
	rmw_cycle = true;
	uae_mmu060_put_long (addr, v);
	rmw_cycle = false;
}
STATIC_INLINE uae_u32 get_rmw_byte_mmu060 (uaecptr addr)
{
	rmw_cycle = true;
	uae_u32 v = uae_mmu060_get_byte (addr);
	rmw_cycle = false;
	return v;
}
STATIC_INLINE uae_u32 get_rmw_word_mmu060 (uaecptr addr)
{
	rmw_cycle = true;
	uae_u32 v = uae_mmu060_get_word (addr);
	rmw_cycle = false;
	return v;
}
STATIC_INLINE uae_u32 get_rmw_long_mmu060 (uaecptr addr)
{
	rmw_cycle = true;
	uae_u32 v = uae_mmu060_get_long (addr);
	rmw_cycle = false;
	return v;
}
// locked rmw 040
STATIC_INLINE void put_lrmw_byte_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_byte, 0);
}
STATIC_INLINE void put_lrmw_word_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_word, 0);
}
STATIC_INLINE void put_lrmw_long_mmu040 (uaecptr addr, uae_u32 v)
{
	uae_mmu_put_lrmw (addr, v, sz_long, 0);
}
STATIC_INLINE uae_u32 get_lrmw_byte_mmu040 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_byte, 0);
}
STATIC_INLINE uae_u32 get_lrmw_word_mmu040 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_word, 0);
}
STATIC_INLINE uae_u32 get_lrmw_long_mmu040 (uaecptr addr)
{
	return uae_mmu_get_lrmw (addr, sz_long, 0);
}

STATIC_INLINE uae_u32 get_ibyte_mmu040 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu040_get_iword (pc);
}
STATIC_INLINE uae_u32 get_iword_mmu040 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu040_get_iword (pc);
}
STATIC_INLINE uae_u32 get_ilong_mmu040 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu040_get_ilong (pc);
}
STATIC_INLINE uae_u32 next_iword_mmu040 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (2);
	return uae_mmu040_get_iword (pc);
}
STATIC_INLINE uae_u32 next_ilong_mmu040 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (4);
	return uae_mmu040_get_ilong (pc);
}

STATIC_INLINE uae_u32 get_ibyte_mmu060 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu060_get_iword (pc);
}
STATIC_INLINE uae_u32 get_iword_mmu060 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu060_get_iword (pc);
}
STATIC_INLINE uae_u32 get_ilong_mmu060 (int o)
{
	uae_u32 pc = m68k_getpci () + o;
	return uae_mmu060_get_ilong (pc);
}
STATIC_INLINE uae_u32 next_iword_mmu060 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (2);
	return uae_mmu060_get_iword (pc);
}
STATIC_INLINE uae_u32 next_ilong_mmu060 (void)
{
	uae_u32 pc = m68k_getpci ();
	m68k_incpci (4);
	return uae_mmu060_get_ilong (pc);
}

extern void flush_mmu040 (uaecptr, int);
extern void m68k_do_rts_mmu040 (void);
extern void m68k_do_rte_mmu040 (uaecptr a7);
extern void m68k_do_bsr_mmu040 (uaecptr oldpc, uae_s32 offset);

extern void flush_mmu060 (uaecptr, int);
extern void m68k_do_rts_mmu060 (void);
extern void m68k_do_rte_mmu060 (uaecptr a7);
extern void m68k_do_bsr_mmu060 (uaecptr oldpc, uae_s32 offset);

#endif /* UAE_CPUMMU_H */
