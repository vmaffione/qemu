/*
 *  CRIS mmu emulation.
 *
 *  Copyright (c) 2007 AXIS Communications AB
 *  Written by Edgar E. Iglesias.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef CONFIG_USER_ONLY

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "cpu.h"
#include "mmu.h"
#include "exec-all.h"

#define D(x)

static inline int cris_mmu_enabled(uint32_t rw_gc_cfg)
{
	return (rw_gc_cfg & 12) != 0;
}

static inline int cris_mmu_segmented_addr(int seg, uint32_t rw_mm_cfg)
{
	return (1 << seg) & rw_mm_cfg;
}

static uint32_t cris_mmu_translate_seg(CPUState *env, int seg)
{
	uint32_t base;
	int i;

	if (seg < 8)
		base = env->sregs[SFR_RW_MM_KBASE_LO];
	else
		base = env->sregs[SFR_RW_MM_KBASE_HI];

	i = seg & 7;
	base >>= i * 4;
	base &= 15;

	base <<= 28;
	return base;
}
/* Used by the tlb decoder.  */
#define EXTRACT_FIELD(src, start, end) \
	    (((src) >> start) & ((1 << (end - start + 1)) - 1))

static inline void set_field(uint32_t *dst, unsigned int val, 
			     unsigned int offset, unsigned int width)
{
	uint32_t mask;

	mask = (1 << width) - 1;
	mask <<= offset;
	val <<= offset;

	val &= mask;
	*dst &= ~(mask);
	*dst |= val;
}

static void dump_tlb(CPUState *env, int mmu)
{
	int set;
	int idx;
	uint32_t hi, lo, tlb_vpn, tlb_pfn;

	for (set = 0; set < 4; set++) {
		for (idx = 0; idx < 16; idx++) {
			lo = env->tlbsets[mmu][set][idx].lo;
			hi = env->tlbsets[mmu][set][idx].hi;
			tlb_vpn = EXTRACT_FIELD(hi, 13, 31);
			tlb_pfn = EXTRACT_FIELD(lo, 13, 31);

			printf ("TLB: [%d][%d] hi=%x lo=%x v=%x p=%x\n", 
					set, idx, hi, lo, tlb_vpn, tlb_pfn);
		}
	}
}

/* rw 0 = read, 1 = write, 2 = exec.  */
static int cris_mmu_translate_page(struct cris_mmu_result_t *res,
				   CPUState *env, uint32_t vaddr,
				   int rw, int usermode)
{
	unsigned int vpage;
	unsigned int idx;
	uint32_t lo, hi;
	uint32_t tlb_vpn, tlb_pfn = 0;
	int tlb_pid, tlb_g, tlb_v, tlb_k, tlb_w, tlb_x;
	int cfg_v, cfg_k, cfg_w, cfg_x;	
	int set, match = 0;
	uint32_t r_cause;
	uint32_t r_cfg;
	int rwcause;
	int mmu = 1; /* Data mmu is default.  */
	int vect_base;

	r_cause = env->sregs[SFR_R_MM_CAUSE];
	r_cfg = env->sregs[SFR_RW_MM_CFG];

	switch (rw) {
		case 2: rwcause = CRIS_MMU_ERR_EXEC; mmu = 0; break;
		case 1: rwcause = CRIS_MMU_ERR_WRITE; break;
		default:
		case 0: rwcause = CRIS_MMU_ERR_READ; break;
	}

	/* I exception vectors 4 - 7, D 8 - 11.  */
	vect_base = (mmu + 1) * 4;

	vpage = vaddr >> 13;

	/* We know the index which to check on each set.
	   Scan both I and D.  */
#if 0
	for (set = 0; set < 4; set++) {
		for (idx = 0; idx < 16; idx++) {
			lo = env->tlbsets[mmu][set][idx].lo;
			hi = env->tlbsets[mmu][set][idx].hi;
			tlb_vpn = EXTRACT_FIELD(hi, 13, 31);
			tlb_pfn = EXTRACT_FIELD(lo, 13, 31);

			printf ("TLB: [%d][%d] hi=%x lo=%x v=%x p=%x\n", 
					set, idx, hi, lo, tlb_vpn, tlb_pfn);
		}
	}
#endif

	idx = vpage & 15;
	for (set = 0; set < 4; set++)
	{
		lo = env->tlbsets[mmu][set][idx].lo;
		hi = env->tlbsets[mmu][set][idx].hi;

		tlb_vpn = EXTRACT_FIELD(hi, 13, 31);
		tlb_pfn = EXTRACT_FIELD(lo, 13, 31);

		D(printf("TLB[%d][%d] v=%x vpage=%x -> pfn=%x lo=%x hi=%x\n", 
				i, idx, tlb_vpn, vpage, tlb_pfn, lo, hi));
		if (tlb_vpn == vpage) {
			match = 1;
			break;
		}
	}

	res->bf_vec = vect_base;
	if (match) {
		cfg_w  = EXTRACT_FIELD(r_cfg, 19, 19);
		cfg_k  = EXTRACT_FIELD(r_cfg, 18, 18);
		cfg_x  = EXTRACT_FIELD(r_cfg, 17, 17);
		cfg_v  = EXTRACT_FIELD(r_cfg, 16, 16);

		tlb_pid = EXTRACT_FIELD(hi, 0, 7);
		tlb_pfn = EXTRACT_FIELD(lo, 13, 31);
		tlb_g  = EXTRACT_FIELD(lo, 4, 4);
		tlb_v = EXTRACT_FIELD(lo, 3, 3);
		tlb_k = EXTRACT_FIELD(lo, 2, 2);
		tlb_w = EXTRACT_FIELD(lo, 1, 1);
		tlb_x = EXTRACT_FIELD(lo, 0, 0);

		/*
		set_exception_vector(0x04, i_mmu_refill);
		set_exception_vector(0x05, i_mmu_invalid);
		set_exception_vector(0x06, i_mmu_access);
		set_exception_vector(0x07, i_mmu_execute);
		set_exception_vector(0x08, d_mmu_refill);
		set_exception_vector(0x09, d_mmu_invalid);
		set_exception_vector(0x0a, d_mmu_access);
		set_exception_vector(0x0b, d_mmu_write);
		*/
		if (!tlb_g
		    && tlb_pid != (env->pregs[PR_PID] & 0xff)) {
			D(printf ("tlb: wrong pid %x %x pc=%x\n", 
				 tlb_pid, env->pregs[PR_PID], env->pc));
			match = 0;
			res->bf_vec = vect_base;
		} else if (cfg_k && tlb_k && usermode) {
			D(printf ("tlb: kernel protected %x lo=%x pc=%x\n", 
				  vaddr, lo, env->pc));
			match = 0;
			res->bf_vec = vect_base + 2;
		} else if (rw == 1 && cfg_w && !tlb_w) {
			D(printf ("tlb: write protected %x lo=%x pc=%x\n", 
				  vaddr, lo, env->pc));
			match = 0;
			/* write accesses never go through the I mmu.  */
			res->bf_vec = vect_base + 3;
		} else if (rw == 2 && cfg_x && !tlb_x) {
			D(printf ("tlb: exec protected %x lo=%x pc=%x\n", 
				 vaddr, lo, env->pc));
			match = 0;
			res->bf_vec = vect_base + 3;
		} else if (cfg_v && !tlb_v) {
			D(printf ("tlb: invalid %x\n", vaddr));
			set_field(&r_cause, rwcause, 8, 9);
			match = 0;
			res->bf_vec = vect_base + 1;
		}

		res->prot = 0;
		if (match) {
			res->prot |= PAGE_READ;
			if (tlb_w)
				res->prot |= PAGE_WRITE;
			if (tlb_x)
				res->prot |= PAGE_EXEC;
		}
		else
			D(dump_tlb(env, mmu));

		env->sregs[SFR_RW_MM_TLB_HI] = hi;
		env->sregs[SFR_RW_MM_TLB_LO] = lo;
	}

	if (!match) {
		/* miss.  */
		idx = vpage & 15;
		set = 0;

		/* Update RW_MM_TLB_SEL.  */
		env->sregs[SFR_RW_MM_TLB_SEL] = 0;
		set_field(&env->sregs[SFR_RW_MM_TLB_SEL], idx, 0, 4);
		set_field(&env->sregs[SFR_RW_MM_TLB_SEL], set, 4, 5);

		/* Update RW_MM_CAUSE.  */
		set_field(&r_cause, rwcause, 8, 2);
		set_field(&r_cause, vpage, 13, 19);
		set_field(&r_cause, env->pregs[PR_PID], 0, 8);
		env->sregs[SFR_R_MM_CAUSE] = r_cause;
		D(printf("refill vaddr=%x pc=%x\n", vaddr, env->pc));
	}


	D(printf ("%s rw=%d mtch=%d pc=%x va=%x vpn=%x tlbvpn=%x pfn=%x pid=%x"
		  " %x cause=%x sel=%x sp=%x %x %x\n",
		  __func__, rw, match, env->pc,
		  vaddr, vpage,
		  tlb_vpn, tlb_pfn, tlb_pid, 
		  env->pregs[PR_PID],
		  r_cause,
		  env->sregs[SFR_RW_MM_TLB_SEL],
		  env->regs[R_SP], env->pregs[PR_USP], env->ksp));

	res->pfn = tlb_pfn;
	return !match;
}

/* Give us the vaddr corresponding to the latest TLB update.  */
target_ulong cris_mmu_tlb_latest_update(CPUState *env, uint32_t new_lo)
{
	uint32_t sel = env->sregs[SFR_RW_MM_TLB_SEL];
	uint32_t vaddr;
	uint32_t hi;
	int set;
	int idx;

	idx = EXTRACT_FIELD(sel, 0, 4);
	set = EXTRACT_FIELD(sel, 4, 5);

	hi = env->tlbsets[1][set][idx].hi;
	vaddr = EXTRACT_FIELD(hi, 13, 31);
	return vaddr << TARGET_PAGE_BITS;
}

int cris_mmu_translate(struct cris_mmu_result_t *res,
		       CPUState *env, uint32_t vaddr,
		       int rw, int mmu_idx)
{
	uint32_t phy = vaddr;
	int seg;
	int miss = 0;
	int is_user = mmu_idx == MMU_USER_IDX;
	uint32_t old_srs;

	old_srs= env->pregs[PR_SRS];

	/* rw == 2 means exec, map the access to the insn mmu.  */
	env->pregs[PR_SRS] = rw == 2 ? 1 : 2;

	if (!cris_mmu_enabled(env->sregs[SFR_RW_GC_CFG])) {
		res->phy = vaddr;
		res->prot = PAGE_BITS;		
		goto done;
	}

	seg = vaddr >> 28;
	if (cris_mmu_segmented_addr(seg, env->sregs[SFR_RW_MM_CFG]))
	{
		uint32_t base;

		miss = 0;
		base = cris_mmu_translate_seg(env, seg);
		phy = base | (0x0fffffff & vaddr);
		res->phy = phy;
		res->prot = PAGE_BITS;		
	}
	else
	{
		miss = cris_mmu_translate_page(res, env, vaddr, rw, is_user);
		phy = (res->pfn << 13);
		res->phy = phy;
	}
  done:
	env->pregs[PR_SRS] = old_srs;
	return miss;
}
#endif
