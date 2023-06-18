/*
 *  Alpha emulation cpu definitions for qemu.
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

#if !defined (__CPU_ALPHA_H__)
#define __CPU_ALPHA_H__

#include "config.h"

#define TARGET_LONG_BITS 64

#define CPUState struct CPUAlphaState

#include "cpu-defs.h"

#include <setjmp.h>

#include "softfloat.h"

#define TARGET_HAS_ICE 1

#define ELF_MACHINE     EM_ALPHA

#define ICACHE_LINE_SIZE 32
#define DCACHE_LINE_SIZE 32

#define TARGET_PAGE_BITS 13

#define VA_BITS 43

/* Alpha major type */
enum {
    ALPHA_EV3  = 1,
    ALPHA_EV4  = 2,
    ALPHA_SIM  = 3,
    ALPHA_LCA  = 4,
    ALPHA_EV5  = 5, /* 21164 */
    ALPHA_EV45 = 6, /* 21064A */
    ALPHA_EV56 = 7, /* 21164A */
};

/* EV4 minor type */
enum {
    ALPHA_EV4_2 = 0,
    ALPHA_EV4_3 = 1,
};

/* LCA minor type */
enum {
    ALPHA_LCA_1 = 1, /* 21066 */
    ALPHA_LCA_2 = 2, /* 20166 */
    ALPHA_LCA_3 = 3, /* 21068 */
    ALPHA_LCA_4 = 4, /* 21068 */
    ALPHA_LCA_5 = 5, /* 21066A */
    ALPHA_LCA_6 = 6, /* 21068A */
};

/* EV5 minor type */
enum {
    ALPHA_EV5_1 = 1, /* Rev BA, CA */
    ALPHA_EV5_2 = 2, /* Rev DA, EA */
    ALPHA_EV5_3 = 3, /* Pass 3 */
    ALPHA_EV5_4 = 4, /* Pass 3.2 */
    ALPHA_EV5_5 = 5, /* Pass 4 */
};

/* EV45 minor type */
enum {
    ALPHA_EV45_1 = 1, /* Pass 1 */
    ALPHA_EV45_2 = 2, /* Pass 1.1 */
    ALPHA_EV45_3 = 3, /* Pass 2 */
};

/* EV56 minor type */
enum {
    ALPHA_EV56_1 = 1, /* Pass 1 */
    ALPHA_EV56_2 = 2, /* Pass 2 */
};

enum {
    IMPLVER_2106x = 0, /* EV4, EV45 & LCA45 */
    IMPLVER_21164 = 1, /* EV5, EV56 & PCA45 */
    IMPLVER_21264 = 2, /* EV6, EV67 & EV68x */
    IMPLVER_21364 = 3, /* EV7 & EV79 */
};

enum {
    AMASK_BWX      = 0x00000001,
    AMASK_FIX      = 0x00000002,
    AMASK_CIX      = 0x00000004,
    AMASK_MVI      = 0x00000100,
    AMASK_TRAP     = 0x00000200,
    AMASK_PREFETCH = 0x00001000,
};

enum {
    VAX_ROUND_NORMAL = 0,
    VAX_ROUND_CHOPPED,
};

enum {
    IEEE_ROUND_NORMAL = 0,
    IEEE_ROUND_DYNAMIC,
    IEEE_ROUND_PLUS,
    IEEE_ROUND_MINUS,
    IEEE_ROUND_CHOPPED,
};

/* IEEE floating-point operations encoding */
/* Trap mode */
enum {
    FP_TRAP_I   = 0x0,
    FP_TRAP_U   = 0x1,
    FP_TRAP_S  = 0x4,
    FP_TRAP_SU  = 0x5,
    FP_TRAP_SUI = 0x7,
};

/* Rounding mode */
enum {
    FP_ROUND_CHOPPED = 0x0,
    FP_ROUND_MINUS   = 0x1,
    FP_ROUND_NORMAL  = 0x2,
    FP_ROUND_DYNAMIC = 0x3,
};

/* How palcode is interpreted.  */
enum pal_emul {
    /* No pal emulation (user linux).  */
    PAL_NONE,

    /* As a real-cpu palcode.  */
    PAL_21264
#if 0
    PAL_21064,
    PAL_21164,
    /* Palcode virtualization (TODO).  */
    PAL_CONSOLE,
    PAL_OPENVMS,
    PAL_UNIX
#endif
};

/* Internal processor registers */
/* XXX: TOFIX: most of those registers are implementation dependant */
enum {
    /* Ebox IPRs */
    IPR_CC           = 0xC0,            /* 21264 */
    IPR_CC_CTL       = 0xC1,            /* 21264 */
#define IPR_CC_CTL_ENA_SHIFT 32
#define IPR_CC_CTL_COUNTER_MASK 0xfffffff0UL
    IPR_VA           = 0xC2,            /* 21264 */
    IPR_VA_CTL       = 0xC4,            /* 21264 */
#define IPR_VA_CTL_VA_48_SHIFT 1
#define IPR_VA_CTL_VPTB_SHIFT 30
    IPR_VA_FORM      = 0xC3,            /* 21264 */
    /* Ibox IPRs */
    IPR_ITB_TAG      = 0x00,            /* 21264 */
    IPR_ITB_PTE      = 0x01,            /* 21264 */
    IPR_ITB_IAP      = 0x02,            /* 21264 */
    IPR_ITB_IA       = 0x03,            /* 21264 */
    IPR_ITB_IS       = 0x04,            /* 21264 */
    IPR_PMPC         = 0x05,
    IPR_EXC_ADDR     = 0x06,            /* 21264 */
    IPR_IVA_FORM     = 0x07,            /* 21264 */
    IPR_CM           = 0x09,            /* 21264 */
#define IPR_CM_SHIFT 3
#define IPR_CM_MASK (3ULL << IPR_CM_SHIFT)      /* 21264 */
    IPR_IER          = 0x0A,            /* 21264 */
#define IPR_IER_MASK 0x0000007fffffe000ULL
    IPR_IER_CM       = 0x0B,            /* 21264: = CM | IER */
    IPR_SIRR         = 0x0C,            /* 21264 */
#define IPR_SIRR_SHIFT 14
#define IPR_SIRR_MASK (0x7fffULL << IPR_SIRR_SHIFT)
    IPR_ISUM         = 0x0D,            /* 21264 */
    IPR_HW_INT_CLR   = 0x0E,            /* 21264 */
    IPR_EXC_SUM      = 0x0F,            /* 21264 */
    IPR_PAL_BASE     = 0x10,            /* 21264 */
    IPR_I_CTL        = 0x11,
#define IPR_I_CTL_CHIP_ID_SHIFT 24      /* 21264 */
#define IPR_I_CTL_BIST_FAIL (1 << 23)   /* 21264 */
#define IPR_I_CTL_IC_EN_SHIFT 1         /* 21264 */
#define IPR_I_CTL_SDE1_SHIFT 7          /* 21264 */
#define IPR_I_CTL_HWE_SHIFT 12          /* 21264 */
#define IPR_I_CTL_VA_48_SHIFT 15        /* 21264 */
#define IPR_I_CTL_SPE_SHIFT 3           /* 21264 */
#define IPR_I_CTL_CALL_PAL_R23_SHIFT 20 /* 21264 */
    IPR_I_STAT       = 0x16,            /* 21264 */
    IPR_IC_FLUSH     = 0x13,            /* 21264 */
    IPR_IC_FLUSH_ASM = 0x12,            /* 21264 */
    IPR_CLR_MAP      = 0x15,
    IPR_SLEEP        = 0x17,
    IPR_PCTX         = 0x40,            /* 21264 */
    IPR_PCTX_ASN       = 0x01,  /* field */
#define IPR_PCTX_ASN_SHIFT 39
    IPR_PCTX_ASTER     = 0x02,  /* field */
#define IPR_PCTX_ASTER_SHIFT 5
    IPR_PCTX_ASTRR     = 0x04,  /* field */
#define IPR_PCTX_ASTRR_SHIFT 9
    IPR_PCTX_PPCE      = 0x08,  /* field */
#define IPR_PCTX_PPCE_SHIFT 1
    IPR_PCTX_FPE       = 0x10,  /* field */
#define IPR_PCTX_FPE_SHIFT 2
    IPR_PCTX_ALL       = 0x5f,  /* all fields */
    IPR_PCTR_CTL     = 0x14,            /* 21264 */
    /* Mbox IPRs */
    IPR_DTB_TAG0     = 0x20,            /* 21264 */
    IPR_DTB_TAG1     = 0xA0,            /* 21264 */
    IPR_DTB_PTE0     = 0x21,            /* 21264 */
    IPR_DTB_PTE1     = 0xA1,            /* 21264 */
    IPR_DTB_ALTMODE  = 0xA6,
    IPR_DTB_ALTMODE0 = 0x26,            /* 21264 */
#define IPR_DTB_ALTMODE_MASK 3
    IPR_DTB_IAP      = 0xA2,            /* 21264 */
    IPR_DTB_IA       = 0xA3,            /* 21264 */
    IPR_DTB_IS0      = 0x24,            /* 21264 */
    IPR_DTB_IS1      = 0xA4,            /* 21264 */
    IPR_DTB_ASN0     = 0x25,            /* 21264 */
    IPR_DTB_ASN1     = 0xA5,            /* 21264 */
#define IPR_DTB_ASN_SHIFT 56
    IPR_MM_STAT      = 0x27,            /* 21264 */
    IPR_M_CTL        = 0x28,            /* 21264 */
#define IPR_M_CTL_SPE_SHIFT 1
#define IPR_M_CTL_SPE_MASK 7
    IPR_DC_CTL       = 0x29,            /* 21264 */
    IPR_DC_STAT      = 0x2A,            /* 21264 */
    /* Cbox IPRs */
    IPR_C_DATA       = 0x2B,
    IPR_C_SHIFT      = 0x2C
};

typedef struct CPUAlphaState CPUAlphaState;

struct alpha_pte {
    uint32_t pa;
    uint16_t fl;
    uint8_t asn;
};

/* PTE flags.  */
#define ALPHA_PTE_V   (1 << 0)
#define ALPHA_PTE_FOR (1 << 1)
#define ALPHA_PTE_FOW (1 << 2)
#define ALPHA_PTE_ASM (1 << 4)
#define ALPHA_PTE_GH_SHIFT 5
#define ALPHA_PTE_KRE (1 << 8)
#define ALPHA_PTE_ERE (1 << 9)
#define ALPHA_PTE_SRE (1 << 10)
#define ALPHA_PTE_URE (1 << 11)
#define ALPHA_PTE_KWE (1 << 12)
#define ALPHA_PTE_EWE (1 << 13)
#define ALPHA_PTE_SWE (1 << 14)
#define ALPHA_PTE_UWE (1 << 15)

/* If PTE_V is not set, the pte is not valid and ASN indicates
   the error cause.  */
#define PTE_ASN_BAD_VA 1	/* Invalid VA - not correctly sign extended. */
#define PTE_ASN_MISS   0	/* No PTE found in TLB. */

#define MAX_NBR_TLB_21264 128
struct alpha_21264_tlb {
    short int in_use;
    short int next;
    unsigned char spe;
    struct alpha_21264_tlbe {
        int64_t va;
        struct alpha_pte pte;
    } entries[MAX_NBR_TLB_21264];
};


#if !defined(CONFIG_USER_ONLY)
#define NB_MMU_MODES 5
#else
#define NB_MMU_MODES 2
#endif

struct CPUAlphaState {
    uint64_t ir[31];
    float64  fir[31];
    float_status fp_status;
    uint64_t fpcr;
    uint64_t pc;
    uint64_t lock;

    /* Those resources are used only in Qemu core */
    CPU_COMMON

    unsigned char intr_flag; /* For RC and RS */
    unsigned char fen; /* FPU enable */
    unsigned char pal_mode;
    enum pal_emul pal_emul;
    unsigned char mmu_data_index; /* 0-3 */
    unsigned char mmu_code_index; /* 0-4 (pal).  */
    unsigned char asn;

    /* Common.  */
    uint64_t pal_base;
    uint64_t exc_addr;

#if defined(CONFIG_USER_ONLY)
    struct {
        uint64_t usp;
        uint64_t unique;
    } user;
#else
    union {
        struct {
            /* Trick to emulate an Icache during early pal decompression.  */
            uint64_t pal_reloc_mask;
            uint64_t pal_reloc_val;
            uint64_t pal_reloc_offset;

            /* Shadow registers for pal mode.  */
            uint64_t shadow_r4;
            uint64_t shadow_r5;
            uint64_t shadow_r6;
            uint64_t shadow_r7;
            uint64_t shadow_r20;
            uint64_t shadow_r21;
            uint64_t shadow_r22;
            uint64_t shadow_r23;

            /* CC */
            uint32_t cc_counter;
            uint64_t cc_load_ticks;

            /* CC_CTL */
            uint64_t cc_offset; /* Only the 32 MSB are set.  */
            unsigned char cc_ena;

            /* I_CTL  */
            uint64_t i_vptb;
            unsigned char iva_48;
            unsigned char hwe;
            unsigned char sde1;
            unsigned char chip_id;
            unsigned char ic_en;
            unsigned char call_pal_r23;

            /* IER + CM  */
            unsigned char cm;
            uint64_t ier;

            uint64_t isum;
            uint64_t ipend; /* fake. */

            /* VA_CTL */
            uint64_t d_vptb;
            unsigned char dva_48;

            /* PCTX.  */
            unsigned char astrr;
            unsigned char aster;
            unsigned char fpe;
            unsigned char ppce;

            unsigned char altmode;

            /* SIRR */
            uint32_t sirr;

            uint32_t mm_stat;
            uint64_t iva_form;

            uint64_t va_form;
            uint64_t va;

            uint64_t exc_sum;
            uint64_t itb_tag;
            uint64_t itb_pte;
            uint64_t dtb_tag;
            uint64_t dtb_pte;
            unsigned char dtb_asn;

            struct alpha_21264_tlb itlb, dtlb;
        } a21264;
    };
#endif

    int error_code;

    uint32_t features;
    uint32_t amask;
    int implver;
};

#define cpu_init cpu_alpha_init
#define cpu_exec cpu_alpha_exec
#define cpu_gen_code cpu_alpha_gen_code
#define cpu_signal_handler cpu_alpha_signal_handler
#define cpu_list alpha_cpu_list

/* MMU modes definitions */
#define MMU_MODE0_SUFFIX _kernel
#define MMU_MODE1_SUFFIX _executive
#define MMU_MODE2_SUFFIX _supervisor
#define MMU_MODE3_SUFFIX _user
#define MMU_MODE4_SUFFIX _pal
#define MMU_KERNEL_IDX 0
#define MMU_USER_IDX 3
#define MMU_PAL_IDX 4

#if !defined(CONFIG_USER_ONLY)
static inline int cpu_mmu_index_data (CPUState *env)
{
    return env->mmu_data_index;
}

static inline int cpu_mmu_index_code (CPUState *env)
{
    return env->mmu_code_index;
}
#else
#define cpu_mmu_index_data cpu_mmu_index_code
static inline int cpu_mmu_index_code (CPUState *env)
{
    return 0;
}

static inline void cpu_clone_regs(CPUState *env, target_ulong newsp)
{
    if (newsp)
        env->ir[30] = newsp;
    /* FIXME: Zero syscall return value.  */
}
#endif

#include "cpu-all.h"
#include "exec-all.h"

enum {
    FEATURE_ASN    = 0x00000001,
    FEATURE_SPS    = 0x00000002,
    FEATURE_VIRBND = 0x00000004,
    FEATURE_TBCHK  = 0x00000008,
};

enum {
    EXCP_21064_RESET            = 0x0000,
    EXCP_21064_MCHK             = 0x0020,
    EXCP_21064_ARITH            = 0x0060,
    EXCP_21064_HW_INTERRUPT     = 0x00E0,
    EXCP_21064_DFAULT           = 0x01E0,
    EXCP_21064_DTB_MISS_PAL     = 0x09E0,
    EXCP_21064_ITB_MISS         = 0x03E0,
    EXCP_21064_ITB_ACV          = 0x07E0,
    EXCP_21064_DTB_MISS_NATIVE  = 0x08E0,
    EXCP_21064_UNALIGN          = 0x11E0,
    EXCP_21064_OPCDEC           = 0x13E0,
    EXCP_21064_FEN              = 0x17E0,

    EXCP_21264_DTBM_DOUBLE_3    = 0x0100,
    EXCP_21264_DTBM_DOUBLE_4    = 0x0180,
    EXCP_21264_FEN              = 0x0200,
    EXCP_21264_UNALIGN          = 0x0280,
    EXCP_21264_DTBM_SINGLE      = 0x0300,
    EXCP_21264_DFAULT           = 0x0380,
    EXCP_21264_OPCDEC           = 0x0400,
    EXCP_21264_IACV             = 0x0480,
    EXCP_21264_MCHK             = 0x0500,
    EXCP_21264_ITB_MISS         = 0x0580,
    EXCP_21264_ARITH            = 0x0600,
    EXCP_21264_INTERRUPT        = 0x0680,
    EXCP_21264_MT_FPCR          = 0x0700,
    EXCP_21264_RESET            = 0x0780,

    /* Generic exception - to be mapped to processor.  */
    EXCP_GEN_OPCDEC             = 1,
    EXCP_GEN_ARITH              = 2,
    EXCP_GEN_FEN                = 3,
    EXCP_GEN_INTERRUPT          = 4,
    EXCP_GEN_LAST               = 4,

    /* User linux exception.  */
    EXCP_USER_DFAULT            = 0x0100,
    EXCP_USER_ITB_MISS          = 0x0101,

    EXCP_CALL_PALP        = 0x2000,
    EXCP_CALL_PAL         = 0x3000,
    EXCP_CALL_PALE        = 0x4000, /* End of Pal */
    /* Pseudo exception for console */
    EXCP_CONSOLE_DISPATCH = 0x4001,
    EXCP_CONSOLE_FIXUP    = 0x4002,
};

/* Arithmetic exception */
enum {
    EXCP_ARITH_OVERFLOW,
};

enum {
    IR_V0   = 0,
    IR_T0   = 1,
    IR_T1   = 2,
    IR_T2   = 3,
    IR_T3   = 4,
    IR_T4   = 5,
    IR_T5   = 6,
    IR_T6   = 7,
    IR_T7   = 8,
    IR_S0   = 9,
    IR_S1   = 10,
    IR_S2   = 11,
    IR_S3   = 12,
    IR_S4   = 13,
    IR_S5   = 14,
    IR_S6   = 15,
#define IR_FP IR_S6
    IR_A0   = 16,
    IR_A1   = 17,
    IR_A2   = 18,
    IR_A3   = 19,
    IR_A4   = 20,
    IR_A5   = 21,
    IR_T8   = 22,
    IR_T9   = 23,
    IR_T10  = 24,
    IR_T11  = 25,
    IR_RA   = 26,
    IR_T12  = 27,
#define IR_PV IR_T12
    IR_AT   = 28,
    IR_GP   = 29,
    IR_SP   = 30,
    IR_ZERO = 31,
};

CPUAlphaState * cpu_alpha_init (const char *cpu_model);
int cpu_alpha_exec(CPUAlphaState *s);
void alpha_cpu_list(FILE *f, int (*cpu_fprintf)(FILE *f, const char *fmt, ...));
/* you can call this signal handler from your SIGBUS and SIGSEGV
   signal handlers to inform the virtual CPU of exceptions. non zero
   is returned if the signal was handled by the virtual CPU.  */
int cpu_alpha_signal_handler(int host_signum, void *pinfo,
                             void *puc);
int cpu_alpha_handle_mmu_fault (CPUState *env, uint64_t address, int rw,
                                int mmu_idx, int is_softmmu);
void do_interrupt (CPUState *env);

uint64_t cpu_alpha_mfpr_21264 (CPUState *env, int iprn);
void cpu_alpha_mtpr_21264 (CPUState *env, int iprn, uint64_t val);
void init_cpu_21264(CPUState *env);
void swap_shadow_21264(CPUState *env);
struct alpha_pte cpu_alpha_mmu_v2p_21264(CPUState *env, int64_t address,
                                         int rwx);
void cpu_alpha_mmu_dfault_21264(CPUState *env, int64_t address);
int cpu_alpha_mmu_fault_21264 (CPUState *env, int64_t address, int rwx,
                               int mmu_idx, void *retaddr);

void cpu_alpha_mmu_fault_pal(CPUState *env, int64_t address);

void alpha_21264_srm_write(CPUState *env);

void cpu_alpha_update_irq (CPUState *env, int irqs);

static inline void cpu_pc_from_tb(CPUState *env, TranslationBlock *tb)
{
    env->pc = tb->pc;
}

static inline void cpu_get_tb_cpu_state(CPUState *env, target_ulong *pc,
                                        target_ulong *cs_base, int *flags)
{
    *pc = env->pc;
    *cs_base = 0;
    *flags = (env->mmu_code_index << 2) | env->mmu_data_index
        | (env->fen << 5)
        | (env->mmu_code_index == MMU_PAL_IDX ? 0x100 << 6: env->asn << 6);
}

/* Flags for virt_to_phys helper. */
#define ALPHA_HW_MMUIDX_MASK 3
#define ALPHA_HW_V (1 << 2)
#define ALPHA_HW_W (1 << 3)
#define ALPHA_HW_E (1 << 4)
#define ALPHA_HW_A (1 << 8)
#define ALPHA_HW_L (1 << 9)
#define ALPHA_HW_Q (1 << 10)

#endif /* !defined (__CPU_ALPHA_H__) */
