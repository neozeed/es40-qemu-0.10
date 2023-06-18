/*
 *  Alpha emulation cpu helpers for qemu.
 *
 *  Copyright (c) 2007 Jocelyn Mayer
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA  02110-1301 USA
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "cpu.h"
#include "sysemu.h"
#include "exec-all.h"


//#define DEBUG_MMU

#if defined(CONFIG_USER_ONLY)

int cpu_alpha_handle_mmu_fault (CPUState *env, target_ulong address, int rw,
                                int mmu_idx, int is_softmmu)
{
    if (rw == 2)
        env->exception_index = EXCP_USER_ITB_MISS;
    else
        env->exception_index = EXCP_USER_DFAULT;
    env->exc_addr = address;

    return 1;
}

target_phys_addr_t cpu_get_phys_page_debug (CPUState *env, target_ulong addr)
{
    return addr;
}

void do_interrupt (CPUState *env)
{
    env->exception_index = -1;
}

#else

target_phys_addr_t cpu_get_phys_page_debug (CPUState *env, target_ulong addr)
{
    return -1;
}

#define GH_MASK(gh) (~((1ULL << (13 + gh)) - 1))
#define TB_PTE_GET_GH(v) (((v) >> 5) & 3)
#define TB_PTE_GET_RE(v) (((v) >> 8) & 0x0f)
#define TB_PTE_GET_WE(v) (((v) >> 12) & 0x0f)
#define TB_PTE_GET_ASM(v) (((v) >> 4) & 1)
#define TB_PTE_GET_FO(v) (((v) >> 1) & 2)
#define IPR_CM_GET_CM(v) (((v) >> 3) & 3)

static uint64_t va_form (int64_t va, uint64_t vptb, int form)
{
    va = (va >> 13) << 3;
    switch (form) {
    case 0: /* VA_48 = 0, VA_FORM_32 = 0 */
        return              (vptb & 0xfffffffe00000000ULL)
            |                 (va & 0x00000001fffffff8ULL);
    case 1: /* VA_48 = 1, VA_FORM_32 = 0 */
        return              (vptb & 0xfffff80000000000ULL)
            | (((va << 26) >> 26) & 0x000007fffffffff8ULL);
    case 2: /* VA_48 = 0, VA_FORM_32 = 1 */
        return              (vptb & 0xffffffffc0000000ULL)
            |                 (va & 0x00000000003ffff8ULL);
    default:
        abort();
    }
}

struct alpha_pte cpu_alpha_mmu_v2p_21264(CPUState *env, int64_t address,
                                         int rwx)
{
    struct alpha_21264_tlb *tlb;
    struct alpha_pte pte;
    int i;
    int va_sh;

    if (rwx == 2) {
        /* Instruction translation buffer miss */
        tlb = &env->a21264.itlb;
        va_sh = env->a21264.iva_48 ? 64 - 48 : 64 - 43;
    } else {
        /* Data translation buffer miss */
        tlb = &env->a21264.dtlb;
        va_sh = env->a21264.dva_48 ? 64 - 48 : 64 - 43;
    }

    /* Check sign extension. */
    if (((address << va_sh) >> va_sh) != address)
        return ((struct alpha_pte){0, 0, PTE_ASN_BAD_VA});

#if 0
    fprintf(stderr, "mmu_fault_21264: addr=%016llx, rwx=%d\n", address, rwx);
#endif

    /* Super page.  */
    if ((tlb->spe & 4) && ((address >> 46) & 3) == 2) {
        pte.pa = (address & 0x000008ffffffe000ULL) >> 13;
        pte.fl = ALPHA_PTE_KRE | ALPHA_PTE_KWE | ALPHA_PTE_V;
        pte.asn = 0;
        return pte;
    }
    if ((tlb->spe & 2) && ((address >> 41) & 0x7f) == 0x7e) {
        pte.pa = (((address << 23) >> 23) & 0x000008ffffffe000ULL) >> 13;
        pte.fl = ALPHA_PTE_KRE | ALPHA_PTE_KWE | ALPHA_PTE_V;
        pte.asn = 0;
        return pte;
    }
    if ((tlb->spe & 1) && ((address >> 30) & 0x3ffff) == 0x3fffe) {
        pte.pa = (address & 0x000000003fffe000ULL) >> 13;
        pte.fl = ALPHA_PTE_KRE | ALPHA_PTE_KWE | ALPHA_PTE_V;
        pte.asn = 0;
        return pte;
    }

    /* Search in TLB.  */
    for (i = 0; i < MAX_NBR_TLB_21264; i++) {
        struct alpha_21264_tlbe *tlbe = &tlb->entries[i];
        int pg_sh;

        if (!(tlbe->pte.fl & ALPHA_PTE_V))
            continue;

        pg_sh = 13 + 3 * TB_PTE_GET_GH(tlbe->pte.fl);
        if ((tlbe->va >> pg_sh) == (address >> pg_sh)
            && ((tlbe->pte.asn == env->asn) || (tlbe->pte.fl & ALPHA_PTE_ASM)))
            return tlbe->pte;
    }

    return ((struct alpha_pte){0, 0, PTE_ASN_MISS});
}

void cpu_alpha_mmu_fault_pal(CPUState *env, int64_t address)
{
    target_ulong phys_addr = address & TARGET_PAGE_MASK;

    if ((address & env->a21264.pal_reloc_mask)
        == env->a21264.pal_reloc_val)
        phys_addr += env->a21264.pal_reloc_offset;

    tlb_set_page_exec(env, address & TARGET_PAGE_MASK, phys_addr,
                      PAGE_EXEC, MMU_PAL_IDX, 1);
}

void cpu_alpha_mmu_dfault_21264(CPUState *env, int64_t address)
{
    env->a21264.va_form =
        va_form(address, env->a21264.d_vptb, env->a21264.dva_48);
}

int cpu_alpha_mmu_fault_21264(CPUState *env, int64_t address, int rwx,
                              int mmu_idx, void *retaddr)
{
    struct alpha_pte pte;
    int rights;

    pte = cpu_alpha_mmu_v2p_21264(env, address, rwx);

#ifdef DEBUG_MMU
    if (mmu_idx != 0 && mmu_idx != 4)
        qemu_log("mmu_fault: addr=%016llx rwx=%d idx=%d pte.fl=%04x asn=%02x\n",
                 address, rwx, mmu_idx, pte.fl, env->asn);
#endif

    rights = (pte.fl >> env->a21264.cm);
    if ((pte.fl & ALPHA_PTE_V)
        && (rights & (rwx == 1 ? ALPHA_PTE_KWE : ALPHA_PTE_KRE))
        && (rwx == 2 || !((pte.fl >> rwx) & ALPHA_PTE_FOR))) {
        uint64_t mask;
        uint64_t pa;
        int mode;

        mask = ((1ULL << (3 * TB_PTE_GET_GH(pte.fl))) - 1) << 13;
        pa = ((((uint64_t)pte.pa) << 13) & ~mask) | (address & mask);
        if (rwx == 2)
            mode = PAGE_READ | PAGE_EXEC;
        else {
            if ((rights & ALPHA_PTE_KWE) && !(pte.fl & ALPHA_PTE_FOW))
                mode = PAGE_WRITE;
            else
                mode = 0;
            if ((rights & ALPHA_PTE_KRE) && !(pte.fl & ALPHA_PTE_FOR))
                mode |= PAGE_READ;
        }
        tlb_set_page_exec(env, address & TARGET_PAGE_MASK, pa,
                          mode, mmu_idx, 1);
        return 0;
    }

    /* Not found.  */
    if (rwx == 2) {
        if (pte.fl == 0) {
            if (pte.asn == PTE_ASN_MISS) {
                env->exception_index = EXCP_21264_ITB_MISS;
                env->a21264.exc_sum = 0;
            } else if (pte.asn == PTE_ASN_BAD_VA) {
                env->exception_index = EXCP_21264_IACV;
                env->a21264.exc_sum = (1ULL << 41);
                env->a21264.va = address;
            } else
                abort();
        } else {
            env->exception_index = EXCP_21264_IACV;
            env->a21264.exc_sum = 0;
        }
        env->a21264.iva_form =
            va_form(address, env->a21264.i_vptb, env->a21264.iva_48);
        if (retaddr)
            abort();
    } else {
        TranslationBlock *tb;
        unsigned long pc;
        uint64_t phys_pc;
        uint32_t insn;

        if (pte.fl == 0 && pte.asn == PTE_ASN_MISS)
            env->exception_index = EXCP_21264_DTBM_SINGLE;
        else
            env->exception_index = EXCP_21264_DFAULT;

        /* In order to correctly set mm_stat and find the right exception,
           we must find which instruction created the fault.  */

        /* This code can only be called from a tb (but the debugger!).
           FIXME: remove the abort() when the code is correct.  */
        if (!likely(retaddr))
            abort();

        /* now we have a real cpu fault */
        pc = (unsigned long)retaddr;
        tb = tb_find_pc(pc);
        if (!likely(tb)) {
            /* Not from translated code!!  Not possible.  */
            abort();
        }

        /* the PC is inside the translated code. It means that we
           have a virtual CPU fault */
        cpu_restore_state(tb, env, pc, NULL);

        /* Extract physical pc address.  */
        /* FIXME: justify why page_addr[1] is not needed.  */
        phys_pc = tb->page_addr[0] + (env->pc & ~TARGET_PAGE_MASK);

        /* Extract instruction.  */
        insn = ldl_phys(phys_pc);

        env->a21264.exc_sum = ((insn >> 21) & 0x1f) << 8;
        env->a21264.mm_stat = ((insn >> 26) << 4)
            | (rwx == 1 ? 1 : 0)
            | (rwx == 0 && (pte.fl & ALPHA_PTE_FOR) ? 6 : 0)
            | (rwx == 1 && (pte.fl & ALPHA_PTE_FOW) ? 0xa : 0)
            | (pte.fl == 0 && pte.asn == PTE_ASN_BAD_VA ? 2 : 0);
        env->a21264.va = address;
        cpu_alpha_mmu_dfault_21264(env, address);
    }

#ifdef DEBUG_MMU
    if (mmu_idx != 0 && mmu_idx != 4)
        qemu_log
            ("mmu_excp:  addr=%016llx excp=%04x exc_sum=%08x mm_stat=%04x\n",
             address,
             env->exception_index,
             (unsigned)env->a21264.exc_sum,
             (unsigned)env->a21264.mm_stat);
#endif

    return 1;
}

static void insert_itlb_21264(CPUState *env, int64_t va, uint64_t pte)
{
    struct alpha_21264_tlb *tlb = &env->a21264.itlb;
    struct alpha_21264_tlbe *e = &tlb->entries[tlb->next];

    /* Should an already matching entry be discarded ?  Not sure.  */
    e->va = ((va & TARGET_PAGE_MASK) << 16) >> 16;
    e->pte.pa = pte >> 13;
    e->pte.fl = (pte & 0x1fff) | ALPHA_PTE_V;
    e->pte.asn = env->asn;

    /* FIXME: Should tlb_set_page be called ?  Worth a try. */

    tlb->next = (tlb->next + 1) % MAX_NBR_TLB_21264;

#ifdef DEBUG_MMU
    if (pte & ALPHA_PTE_ERE)
        qemu_log("insert itlb: va=%016llx fl=%04x pa=%08x asn=%02x\n",
                 va, e->pte.fl, e->pte.pa, e->pte.asn);
#endif
}

static void insert_dtlb_21264(CPUState *env, int64_t va, uint64_t pte)
{
    struct alpha_21264_tlb *tlb = &env->a21264.dtlb;
    struct alpha_21264_tlbe *e = &tlb->entries[tlb->next];

    /* Should an already matching entry be discarded ?  Not sure.  */
    e->va = ((va & TARGET_PAGE_MASK) << 16) >> 16;
    e->pte.pa = pte >> 32;
    e->pte.asn = env->asn;
    e->pte.fl = pte | ALPHA_PTE_V;
    tlb->next = (tlb->next + 1) % MAX_NBR_TLB_21264;

#ifdef DEBUG_MMU
    if (pte & (ALPHA_PTE_ERE | ALPHA_PTE_EWE))
        qemu_log("insert dtlb: va=%016llx fl=%04x pa=%08x asn=%02x\n",
                 va, e->pte.fl, e->pte.pa, e->pte.asn);
#endif
}

static void flush_tlb_asm_21264(CPUState *env, struct alpha_21264_tlb *tlb)
{
    int i;

    for (i = 0; i < MAX_NBR_TLB_21264; i++) {
        struct alpha_21264_tlbe *e = &tlb->entries[i];
        if (!(e->pte.fl & ALPHA_PTE_ASM))
            e->pte.fl = 0;
    }
}

static void flush_tlb_all_21264(CPUState *env, struct alpha_21264_tlb *tlb)
{
    int i;

    for (i = 0; i < MAX_NBR_TLB_21264; i++)
        tlb->entries[i].pte.fl = 0;
}

static void flush_tlb_21264_page(CPUState *env, struct alpha_21264_tlb *tlb,
                                 uint64_t addr)
{
    int i;

    for (i = 0; i < MAX_NBR_TLB_21264; i++) {
        struct alpha_21264_tlbe *e = &tlb->entries[i];
        int pg_sh = 13 + 3 * TB_PTE_GET_GH(e->pte.fl);

        if ((e->va >> pg_sh) == (addr >> pg_sh)
            && ((e->pte.fl & ALPHA_PTE_ASM) || e->pte.asn == env->asn)) {
            uint64_t k;
            uint64_t baddr = (addr >> pg_sh) << pg_sh;

            for (k = 0; k < 1 << pg_sh; k += TARGET_PAGE_SIZE)
                tlb_flush_page(env, baddr + k);
            e->pte.fl = 0;
        }
    }
}

uint64_t cpu_alpha_mfpr_21264 (CPUState *env, int iprn)
{
    switch (iprn) {
    case IPR_PAL_BASE:
        return env->pal_base;
    case IPR_I_CTL:
        return env->a21264.i_vptb
            | (env->a21264.chip_id << IPR_I_CTL_CHIP_ID_SHIFT)
            | (env->a21264.iva_48 << IPR_I_CTL_VA_48_SHIFT)
            | (env->a21264.hwe << IPR_I_CTL_HWE_SHIFT)
            | (env->a21264.sde1 << IPR_I_CTL_SDE1_SHIFT)
            | (env->a21264.ic_en << IPR_I_CTL_IC_EN_SHIFT)
            | (env->a21264.call_pal_r23 << IPR_I_CTL_CALL_PAL_R23_SHIFT)
            | (env->a21264.itlb.spe << IPR_I_CTL_SPE_SHIFT);
    case IPR_IVA_FORM:
        return env->a21264.iva_form;
    case IPR_VA:
        return env->a21264.va;
    case IPR_EXC_ADDR:
        return env->exc_addr;
    case IPR_I_STAT:   /* Not emulated  */
    case IPR_DC_STAT:  /* Not emulated  */
        return 0;
    case IPR_C_DATA:
    case IPR_C_SHIFT:
        return 0;
    case IPR_PCTX ... IPR_PCTX_ALL:
        return (((uint64_t)env->asn) << IPR_PCTX_ASN_SHIFT)
            | (((uint64_t)env->a21264.astrr) << IPR_PCTX_ASTRR_SHIFT)
            | (((uint64_t)env->a21264.aster) << IPR_PCTX_ASTER_SHIFT)
            | (((uint64_t)env->fen) << IPR_PCTX_FPE_SHIFT)
            | (((uint64_t)env->a21264.ppce) << IPR_PCTX_PPCE_SHIFT);
    case IPR_IER_CM:
    case IPR_CM:
    case IPR_IER:
        return (((uint64_t)env->a21264.cm) << IPR_CM_SHIFT)
            | env->a21264.ier;
    case IPR_ISUM:
        return env->a21264.isum;
    case IPR_SIRR:
        return env->a21264.sirr;
    case IPR_MM_STAT:
        return env->a21264.mm_stat;
    case IPR_VA_FORM:
        return env->a21264.va_form;
    case IPR_EXC_SUM:
        return env->a21264.exc_sum;
    default:
        cpu_abort(env, "cpu_alpha_mfpr_21264: ipr 0x%x not handled\n", iprn);
    }
}

void cpu_alpha_mtpr_21264 (CPUState *env, int iprn, uint64_t val)
{
#if 0
    qemu_log("cpu_alpha_mtpr_21264: ipr=0x%02x, val="TARGET_FMT_lx"\n",
             iprn, val);
#endif
    switch (iprn) {
    case IPR_CC:
        env->a21264.cc_offset = (val >> 32) << 32;
        break;
    case IPR_CC_CTL:
        env->a21264.cc_ena = (val >> IPR_CC_CTL_ENA_SHIFT) & 1;
        env->a21264.cc_counter = val & IPR_CC_CTL_COUNTER_MASK;
        env->a21264.cc_load_ticks = cpu_get_ticks();
        break;
    case IPR_ITB_TAG:
        env->a21264.itb_tag = val & 0x0000ffffffffe000ULL;
        break;
    case IPR_DTB_TAG0:
        env->a21264.dtb_tag = val & 0x0000ffffffffe000ULL;
        break;
    case IPR_DTB_TAG1:
    case IPR_DTB_ASN1:
    case IPR_DTB_PTE1:
        break; /* DTAG */
    case IPR_ITB_PTE:
        env->a21264.itb_pte = val & 0x00000fffffffef70ULL;
        insert_itlb_21264(env, env->a21264.itb_tag, env->a21264.itb_pte);
        break;
    case IPR_DTB_PTE0:
        env->a21264.dtb_pte = val &= 0x7fffffff0000ffe6ULL;
        insert_dtlb_21264(env, env->a21264.dtb_tag, env->a21264.dtb_pte);
        break;
    case IPR_DTB_ASN0:
        env->a21264.dtb_asn = (val >> IPR_DTB_ASN_SHIFT) & 0xff;
        break;
    case IPR_PAL_BASE:
        env->pal_base = val & 0x00000fffffff8000ULL;
        break;
    case IPR_I_CTL:
    {
        unsigned char old_sde1 = env->a21264.sde1;
        env->a21264.i_vptb =
          ((((int64_t)val) << 16) >> 16) & 0xffffffffc0000000ULL;
        env->a21264.hwe = (val >> IPR_I_CTL_HWE_SHIFT) & 1;
        env->a21264.sde1 = (val >> IPR_I_CTL_SDE1_SHIFT) & 1;
        env->a21264.iva_48 = (val >> IPR_I_CTL_VA_48_SHIFT) & 3;
        env->a21264.itlb.spe = (val >> IPR_I_CTL_SPE_SHIFT) & 7;
        env->a21264.call_pal_r23 = (val >> IPR_I_CTL_CALL_PAL_R23_SHIFT) & 1;
        if (env->pal_mode && old_sde1 != env->a21264.sde1)
            swap_shadow_21264(env);
        break;
    }
    case IPR_VA_CTL:
        env->a21264.d_vptb = val & 0xffffffffc0000000ULL;
        env->a21264.dva_48 = (val >> IPR_VA_CTL_VA_48_SHIFT) & 3;
        /* env->a21264.b_endian = val & 1; */
        if (val & 1)
            cpu_abort (env, "mtpr va_ctl: b_endian not yet handled\n");
        break;
    case IPR_IER_CM:
    case IPR_CM:
    case IPR_IER:
        if (iprn & 2) {
            env->a21264.ier = val & IPR_IER_MASK;
            env->a21264.isum = env->a21264.ipend & env->a21264.ier;
        }
        if (iprn & 1) {
            env->a21264.cm = (val & IPR_CM_MASK) >> IPR_CM_SHIFT;
            env->mmu_data_index = env->a21264.cm;
        }
        break;
    case IPR_IC_FLUSH:
    case IPR_IC_FLUSH_ASM:
        tb_flush(env);
        break;
    case IPR_ITB_IA:
        tlb_flush(env, 1);
        flush_tlb_all_21264(env, &env->a21264.itlb);
        break;
    case IPR_ITB_IAP:
        tlb_flush(env, 1);
        flush_tlb_asm_21264(env, &env->a21264.itlb);
        break;
    case IPR_ITB_IS:
        flush_tlb_21264_page(env, &env->a21264.itlb, val);
        break;
    case IPR_DTB_IA:
        tlb_flush(env, 1);
        flush_tlb_all_21264(env, &env->a21264.dtlb);
        break;
    case IPR_DTB_IAP:
        tlb_flush(env, 1);
        flush_tlb_asm_21264(env, &env->a21264.dtlb);
        break;
    case IPR_DTB_IS0:
        flush_tlb_21264_page(env, &env->a21264.dtlb, val);
        break;
    case IPR_DTB_IS1:
        break;
    case IPR_I_STAT:   /* Not emulated  */
    case IPR_DC_STAT:  /* Not emulated  */
        break;
    case IPR_MM_STAT:  /* RO */
        break;
    case IPR_PCTX ... IPR_PCTX_ALL:
        if (iprn & IPR_PCTX_ASN) {
            uint8_t nasn = (val >> IPR_PCTX_ASN_SHIFT) & 0xff;
            if (nasn != env->asn) {
                env->asn = nasn;
                tlb_flush(env, 1);
            }
        }
        if (iprn & IPR_PCTX_ASTRR) {
            env->a21264.astrr = (val >> IPR_PCTX_ASTRR_SHIFT) & 0xf;
            if (env->a21264.astrr)
                cpu_abort(env, "set pctx.astrr unhandled");
        }
        if (iprn & IPR_PCTX_ASTER) {
            env->a21264.aster = (val >> IPR_PCTX_ASTER_SHIFT) & 0xf;
            if (env->a21264.aster)
                cpu_abort(env, "set pctx.aster unhandled");
        }
        if (iprn & IPR_PCTX_FPE)
            env->fen = (val >> IPR_PCTX_FPE_SHIFT) & 1;
        if (iprn & IPR_PCTX_PPCE)
            env->a21264.ppce = (val >> IPR_PCTX_PPCE_SHIFT) & 1;
        break;
    case IPR_M_CTL:
        env->a21264.dtlb.spe =
            (val >> IPR_M_CTL_SPE_SHIFT) & IPR_M_CTL_SPE_MASK;
        break;
    case IPR_SIRR:
        env->a21264.sirr = val & IPR_SIRR_MASK;
        env->a21264.ipend = ((env->a21264.ipend & ~IPR_SIRR_MASK)
                             | env->a21264.sirr);
        env->a21264.isum = env->a21264.ipend & env->a21264.ier;
        break;
    case IPR_HW_INT_CLR:
        break;
    case IPR_DTB_ALTMODE0:
        env->a21264.altmode = val & IPR_DTB_ALTMODE_MASK;
        break;
    case IPR_PCTR_CTL:
        /* Not emulated.  */
        break;
    case IPR_C_DATA:
    case IPR_C_SHIFT:
        break;
    case IPR_DC_CTL:
        /* Unhandled: f_bad_decc, f_bad_tpar, f_hit.  */
        if (val & 0x34)
            cpu_abort(env, "cpu_alpha_mtpr_21264 dc_ctl: bad value %08x\n",
                      (unsigned)val);
        break;
    case 0x2d:  /* Not documented (M_FIX)  */
        /* Hack: save srm.  */
        if (env->a21264.pal_reloc_val)
            alpha_21264_srm_write(env);
        break;
    default:
        cpu_abort(env, "cpu_alpha_mtpr_21264: ipr 0x%x not handled\n", iprn);
    }
}

void init_cpu_21264(CPUState *env)
{
    env->pal_base = 0;
    env->a21264.chip_id = 0x21;
    env->a21264.ic_en = 3;
    env->pal_emul = PAL_21264;
    memset (&env->a21264.itlb, 0, sizeof (env->a21264.itlb));
    memset (&env->a21264.dtlb, 0, sizeof (env->a21264.dtlb));
}

void swap_shadow_21264(CPUState *env)
{
#define swap(a, b) do { uint64_t t = a; a = b; b = t; } while (0)
    swap(env->a21264.shadow_r4, env->ir[4]);
    swap(env->a21264.shadow_r5, env->ir[5]);
    swap(env->a21264.shadow_r6, env->ir[6]);
    swap(env->a21264.shadow_r7, env->ir[7]);
    swap(env->a21264.shadow_r20, env->ir[20]);
    swap(env->a21264.shadow_r21, env->ir[21]);
    swap(env->a21264.shadow_r22, env->ir[22]);
    swap(env->a21264.shadow_r23, env->ir[23]);
#undef swap
}

void cpu_alpha_update_irq (CPUState *env, int irqs)
{
    switch (env->pal_emul) {
    case PAL_21264:
        env->a21264.ipend &= ~(0x3fULL << 33);
        env->a21264.ipend |= ((uint64_t)irqs) << 33;
        env->a21264.isum = env->a21264.ipend & env->a21264.ier;
        if (env->a21264.isum && !env->pal_mode)
            cpu_interrupt(env, CPU_INTERRUPT_HARD);
        break;
    default:
        abort();
        break;
    }
}

void do_interrupt (CPUState *env)
{
    int excp;

    if (env->pal_mode && env->exception_index == EXCP_GEN_INTERRUPT) {
        /* Can this happen ?  Maybe if the basic block finishes with a
           palcall.  */
        cpu_abort(env, "do_interrupt: pal_mode=1\n");
    }

    env->exc_addr = env->pc | env->pal_mode;
    excp = env->exception_index;
    env->exception_index = 0;
    env->error_code = 0;
    env->pal_mode = 1;
    env->mmu_code_index = MMU_PAL_IDX;

    /* Generic exception translation.  */
    if (excp <= EXCP_GEN_LAST) {
        switch (env->pal_emul) {
        case PAL_21264:
            switch (excp) {
            case EXCP_GEN_OPCDEC:    excp = EXCP_21264_OPCDEC; break;
            case EXCP_GEN_ARITH:     excp = EXCP_21264_ARITH; break;
            case EXCP_GEN_FEN:       excp = EXCP_21264_FEN; break;
            case EXCP_GEN_INTERRUPT: excp = EXCP_21264_INTERRUPT; break;
            default:
                abort();
            }
            break;
        case PAL_NONE:
            cpu_abort(env, "do_interrupt: pal emul not supported\n");
        }
    }

    switch (env->pal_emul) {
    case PAL_21264:
        if (env->a21264.sde1 && !(env->exc_addr & 1))
            swap_shadow_21264(env);
        if ((excp & EXCP_CALL_PALP) && env->a21264.call_pal_r23)
            env->ir[23] = env->pc;
        if (excp == EXCP_21264_INTERRUPT)
          cpu_reset_interrupt(env, CPU_INTERRUPT_HARD);
        break;
    default:
        break;
    }


    /* We use native PALcode */
    env->pc = env->pal_base + excp;

#if 0
    if (env->pal_base != -1ULL) {
        /* We use native PALcode */
        env->pc = env->pal_base + excp;
    } else {
        /* We use emulated PALcode */
        call_pal(env);
        /* Emulate REI */
        env->pc = env->exc_addr & ~3ULL;
        /* env->ipr[IPR_EXC_ADDR] = env->ipr[IPR_EXC_ADDR] & 1; */
        /* XXX: re-enable interrupts and memory mapping */
    }
#endif
}
#endif

void cpu_dump_state (CPUState *env, FILE *f,
                     int (*cpu_fprintf)(FILE *f, const char *fmt, ...),
                     int flags)
{
    static const char *linux_reg_names[] = {
        "v0 ", "t0 ", "t1 ", "t2 ", "t3 ", "t4 ", "t5 ", "t6 ",
        "t7 ", "s0 ", "s1 ", "s2 ", "s3 ", "s4 ", "s5 ", "fp ",
        "a0 ", "a1 ", "a2 ", "a3 ", "a4 ", "a5 ", "t8 ", "t9 ",
        "t10", "t11", "ra ", "t12", "at ", "gp ", "sp ", "zero",
    };
    int i;

    cpu_fprintf(f, "     PC  " TARGET_FMT_lx "      pal=%d\n",
                env->pc, env->pal_mode);
    for (i = 0; i < 31; i++) {
        cpu_fprintf(f, "IR%02d %s " TARGET_FMT_lx " ", i,
                    linux_reg_names[i], env->ir[i]);
        if ((i % 3) == 2)
            cpu_fprintf(f, "\n");
    }
    cpu_fprintf(f, "\n");
    for (i = 0; i < 31; i++) {
        cpu_fprintf(f, "FIR%02d    " TARGET_FMT_lx " ", i,
                    *((uint64_t *)(&env->fir[i])));
        if ((i % 3) == 2)
            cpu_fprintf(f, "\n");
    }
    cpu_fprintf(f, "\n");
    /* cpu_fprintf(f, "\nlock     " TARGET_FMT_lx "\n", env->lock); */
}
