/*
 *  Alpha emulation cpu micro-operations helpers for qemu.
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

#include "exec.h"
#include "host-utils.h"
#include "softfloat.h"
#include "helper.h"

#if !defined(CONFIG_USER_ONLY)
#include "softmmu_exec.h"
#endif /* !defined(CONFIG_USER_ONLY) */

extern uint64_t cpu_get_ticks(void);

void helper_tb_flush (void)
{
    tb_flush(env);
}

/*****************************************************************************/
/* Exceptions processing helpers */
void helper_excp (int excp, int error)
{
    env->exception_index = excp;
    env->error_code = error;
    cpu_loop_exit();
}

uint64_t helper_load_pcc (void)
{
#ifdef CONFIG_USER_ONLY
    /* FIXME */
    return 0;
#else
    uint32_t res;

    switch (env->pal_emul) {
    case PAL_21264:
        res = env->a21264.cc_counter;
        if (env->a21264.cc_ena)
            res += (cpu_get_ticks() - env->a21264.cc_load_ticks) >> 3;
        return res | env->a21264.cc_offset;
    default:
        cpu_abort(env,"load_ppc: bad pal emul\n");
    }
#endif
}

uint64_t helper_load_fpcr (void)
{
    uint64_t ret = 0;
#ifdef CONFIG_SOFTFLOAT
    ret |= env->fp_status.float_exception_flags << 52;
    if (env->fp_status.float_exception_flags)
        ret |= 1ULL << 63;
    env->ipr[IPR_EXC_SUM] &= ~0x3E:
    env->ipr[IPR_EXC_SUM] |= env->fp_status.float_exception_flags << 1;
#endif
    switch (env->fp_status.float_rounding_mode) {
    case float_round_nearest_even:
        ret |= 2ULL << 58;
        break;
    case float_round_down:
        ret |= 1ULL << 58;
        break;
    case float_round_up:
        ret |= 3ULL << 58;
        break;
    case float_round_to_zero:
        break;
    }
    return ret;
}

void helper_store_fpcr (uint64_t val)
{
#ifdef CONFIG_SOFTFLOAT
    set_float_exception_flags((val >> 52) & 0x3F, &FP_STATUS);
#endif
    switch ((val >> 58) & 3) {
    case 0:
        set_float_rounding_mode(float_round_to_zero, &FP_STATUS);
        break;
    case 1:
        set_float_rounding_mode(float_round_down, &FP_STATUS);
        break;
    case 2:
        set_float_rounding_mode(float_round_nearest_even, &FP_STATUS);
        break;
    case 3:
        set_float_rounding_mode(float_round_up, &FP_STATUS);
        break;
    }
}

spinlock_t intr_cpu_lock = SPIN_LOCK_UNLOCKED;

uint64_t helper_rs(void)
{
    uint64_t tmp;

    spin_lock(&intr_cpu_lock);
    tmp = env->intr_flag;
    env->intr_flag = 1;
    spin_unlock(&intr_cpu_lock);

    return tmp;
}

uint64_t helper_rc(void)
{
    uint64_t tmp;

    spin_lock(&intr_cpu_lock);
    tmp = env->intr_flag;
    env->intr_flag = 0;
    spin_unlock(&intr_cpu_lock);

    return tmp;
}

uint64_t helper_addqv (uint64_t op1, uint64_t op2)
{
    uint64_t tmp = op1;
    op1 += op2;
    if (unlikely((tmp ^ op2 ^ (-1ULL)) & (tmp ^ op1) & (1ULL << 63))) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    return op1;
}

uint64_t helper_addlv (uint64_t op1, uint64_t op2)
{
    uint64_t tmp = op1;
    op1 = (uint32_t)(op1 + op2);
    if (unlikely((tmp ^ op2 ^ (-1UL)) & (tmp ^ op1) & (1UL << 31))) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    return op1;
}

uint64_t helper_subqv (uint64_t op1, uint64_t op2)
{
    uint64_t res;
    res = op1 - op2;
    if (unlikely((op1 ^ op2) & (res ^ op1) & (1ULL << 63))) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    return res;
}

uint64_t helper_sublv (uint64_t op1, uint64_t op2)
{
    uint32_t res;
    res = op1 - op2;
    if (unlikely((op1 ^ op2) & (res ^ op1) & (1UL << 31))) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    return res;
}

uint64_t helper_mullv (uint64_t op1, uint64_t op2)
{
    int64_t res = (int64_t)op1 * (int64_t)op2;

    if (unlikely((int32_t)res != res)) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    return (int64_t)((int32_t)res);
}

uint64_t helper_mulqv (uint64_t op1, uint64_t op2)
{
    uint64_t tl, th;

    muls64(&tl, &th, op1, op2);
    /* If th != 0 && th != -1, then we had an overflow */
    if (unlikely((th + 1) > 1)) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    return tl;
}

uint64_t helper_umulh (uint64_t op1, uint64_t op2)
{
    uint64_t tl, th;

    mulu64(&tl, &th, op1, op2);
    return th;
}

uint64_t helper_ctpop (uint64_t arg)
{
    return ctpop64(arg);
}

uint64_t helper_ctlz (uint64_t arg)
{
    return clz64(arg);
}

uint64_t helper_cttz (uint64_t arg)
{
    return ctz64(arg);
}

static always_inline uint64_t byte_zap (uint64_t op, uint8_t mskb)
{
    uint64_t mask;

    mask = 0;
    mask |= ((mskb >> 0) & 1) * 0x00000000000000FFULL;
    mask |= ((mskb >> 1) & 1) * 0x000000000000FF00ULL;
    mask |= ((mskb >> 2) & 1) * 0x0000000000FF0000ULL;
    mask |= ((mskb >> 3) & 1) * 0x00000000FF000000ULL;
    mask |= ((mskb >> 4) & 1) * 0x000000FF00000000ULL;
    mask |= ((mskb >> 5) & 1) * 0x0000FF0000000000ULL;
    mask |= ((mskb >> 6) & 1) * 0x00FF000000000000ULL;
    mask |= ((mskb >> 7) & 1) * 0xFF00000000000000ULL;

    return op & ~mask;
}

uint64_t helper_mskbl(uint64_t val, uint64_t mask)
{
    return byte_zap(val, 0x01 << (mask & 7));
}

uint64_t helper_insbl(uint64_t val, uint64_t mask)
{
    val <<= (mask & 7) * 8;
    return byte_zap(val, ~(0x01 << (mask & 7)));
}

uint64_t helper_mskwl(uint64_t val, uint64_t mask)
{
    return byte_zap(val, 0x03 << (mask & 7));
}

uint64_t helper_inswl(uint64_t val, uint64_t mask)
{
    val <<= (mask & 7) * 8;
    return byte_zap(val, ~(0x03 << (mask & 7)));
}

uint64_t helper_mskll(uint64_t val, uint64_t mask)
{
    return byte_zap(val, 0x0F << (mask & 7));
}

uint64_t helper_insll(uint64_t val, uint64_t mask)
{
    val <<= (mask & 7) * 8;
    return byte_zap(val, ~(0x0F << (mask & 7)));
}

uint64_t helper_zap(uint64_t val, uint64_t mask)
{
    return byte_zap(val, mask);
}

uint64_t helper_zapnot(uint64_t val, uint64_t mask)
{
    return byte_zap(val, ~mask);
}

uint64_t helper_mskql(uint64_t val, uint64_t mask)
{
    return byte_zap(val, 0xFF << (mask & 7));
}

uint64_t helper_insql(uint64_t val, uint64_t mask)
{
    val <<= (mask & 7) * 8;
    return byte_zap(val, ~(0xFF << (mask & 7)));
}

uint64_t helper_mskwh(uint64_t val, uint64_t mask)
{
    return byte_zap(val, (0x03 << (mask & 7)) >> 8);
}

uint64_t helper_inswh(uint64_t val, uint64_t mask)
{
    val >>= 64 - ((mask & 7) * 8);
    return byte_zap(val, ~((0x03 << (mask & 7)) >> 8));
}

uint64_t helper_msklh(uint64_t val, uint64_t mask)
{
    return byte_zap(val, (0x0F << (mask & 7)) >> 8);
}

uint64_t helper_inslh(uint64_t val, uint64_t mask)
{
    val >>= 64 - ((mask & 7) * 8);
    return byte_zap(val, ~((0x0F << (mask & 7)) >> 8));
}

uint64_t helper_mskqh(uint64_t val, uint64_t mask)
{
    return byte_zap(val, (0xFF << (mask & 7)) >> 8);
}

uint64_t helper_insqh(uint64_t val, uint64_t mask)
{
    val >>= 64 - ((mask & 7) * 8);
    return byte_zap(val, ~((0xFF << (mask & 7)) >> 8));
}

uint64_t helper_cmpbge (uint64_t op1, uint64_t op2)
{
    uint8_t opa, opb, res;
    int i;

    res = 0;
    for (i = 0; i < 8; i++) {
        opa = op1 >> (i * 8);
        opb = op2 >> (i * 8);
        if (opa >= opb)
            res |= 1 << i;
    }
    return res;
}

/* Floating point helpers */

/* F floating (VAX) */
static always_inline uint64_t float32_to_f (float32 fa)
{
    uint64_t r, exp, mant, sig;
    CPU_FloatU a;

    a.f = fa;
    sig = ((uint64_t)a.l & 0x80000000) << 32;
    exp = (a.l >> 23) & 0xff;
    mant = ((uint64_t)a.l & 0x007fffff) << 29;

    if (exp == 255) {
        /* NaN or infinity */
        r = 1; /* VAX dirty zero */
    } else if (exp == 0) {
        if (mant == 0) {
            /* Zero */
            r = 0;
        } else {
            /* Denormalized */
            r = sig | ((exp + 1) << 52) | mant;
        }
    } else {
        if (exp >= 253) {
            /* Overflow */
            r = 1; /* VAX dirty zero */
        } else {
            r = sig | ((exp + 2) << 52);
        }
    }

    return r;
}

static always_inline float32 f_to_float32 (uint64_t a)
{
    uint32_t exp, mant_sig;
    CPU_FloatU r;

    exp = ((a >> 55) & 0x80) | ((a >> 52) & 0x7f);
    mant_sig = ((a >> 32) & 0x80000000) | ((a >> 29) & 0x007fffff);

    if (unlikely(!exp && mant_sig)) {
        /* Reserved operands / Dirty zero */
        helper_excp(EXCP_GEN_OPCDEC, 0);
    }

    if (exp < 3) {
        /* Underflow */
        r.l = 0;
    } else {
        r.l = ((exp - 2) << 23) | mant_sig;
    }

    return r.f;
}

uint32_t helper_f_to_memory (uint64_t a)
{
    uint32_t r;
    r =  (a & 0x00001fffe0000000ull) >> 13;
    r |= (a & 0x07ffe00000000000ull) >> 45;
    r |= (a & 0xc000000000000000ull) >> 48;
    return r;
}

uint64_t helper_memory_to_f (uint32_t a)
{
    uint64_t r;
    r =  ((uint64_t)(a & 0x0000c000)) << 48;
    r |= ((uint64_t)(a & 0x003fffff)) << 45;
    r |= ((uint64_t)(a & 0xffff0000)) << 13;
    if (!(a & 0x00004000))
        r |= 0x7ll << 59;
    return r;
}

uint64_t helper_addf (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = f_to_float32(a);
    fb = f_to_float32(b);
    fr = float32_add(fa, fb, &FP_STATUS);
    return float32_to_f(fr);
}

uint64_t helper_subf (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = f_to_float32(a);
    fb = f_to_float32(b);
    fr = float32_sub(fa, fb, &FP_STATUS);
    return float32_to_f(fr);
}

uint64_t helper_mulf (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = f_to_float32(a);
    fb = f_to_float32(b);
    fr = float32_mul(fa, fb, &FP_STATUS);
    return float32_to_f(fr);
}

uint64_t helper_divf (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = f_to_float32(a);
    fb = f_to_float32(b);
    fr = float32_div(fa, fb, &FP_STATUS);
    return float32_to_f(fr);
}

uint64_t helper_sqrtf (uint64_t t)
{
    float32 ft, fr;

    ft = f_to_float32(t);
    fr = float32_sqrt(ft, &FP_STATUS);
    return float32_to_f(fr);
}


/* G floating (VAX) */
static always_inline uint64_t float64_to_g (float64 fa)
{
    uint64_t r, exp, mant, sig;
    CPU_DoubleU a;

    a.d = fa;
    sig = a.ll & 0x8000000000000000ull;
    exp = (a.ll >> 52) & 0x7ff;
    mant = a.ll & 0x000fffffffffffffull;

    if (exp == 2047) {
        /* NaN or infinity */
        r = 1; /* VAX dirty zero */
    } else if (exp == 0) {
        if (mant == 0) {
            /* Zero */
            r = 0;
        } else {
            /* Denormalized */
            r = sig | ((exp + 1) << 52) | mant;
        }
    } else {
        if (exp >= 2045) {
            /* Overflow */
            r = 1; /* VAX dirty zero */
        } else {
            r = sig | ((exp + 2) << 52);
        }
    }

    return r;
}

static always_inline float64 g_to_float64 (uint64_t a)
{
    uint64_t exp, mant_sig;
    CPU_DoubleU r;

    exp = (a >> 52) & 0x7ff;
    mant_sig = a & 0x800fffffffffffffull;

    if (!exp && mant_sig) {
        /* Reserved operands / Dirty zero */
        helper_excp(EXCP_GEN_OPCDEC, 0);
    }

    if (exp < 3) {
        /* Underflow */
        r.ll = 0;
    } else {
        r.ll = ((exp - 2) << 52) | mant_sig;
    }

    return r.d;
}

uint64_t helper_g_to_memory (uint64_t a)
{
    uint64_t r;
    r =  (a & 0x000000000000ffffull) << 48;
    r |= (a & 0x00000000ffff0000ull) << 16;
    r |= (a & 0x0000ffff00000000ull) >> 16;
    r |= (a & 0xffff000000000000ull) >> 48;
    return r;
}

uint64_t helper_memory_to_g (uint64_t a)
{
    uint64_t r;
    r =  (a & 0x000000000000ffffull) << 48;
    r |= (a & 0x00000000ffff0000ull) << 16;
    r |= (a & 0x0000ffff00000000ull) >> 16;
    r |= (a & 0xffff000000000000ull) >> 48;
    return r;
}

uint64_t helper_addg (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = g_to_float64(a);
    fb = g_to_float64(b);
    fr = float64_add(fa, fb, &FP_STATUS);
    return float64_to_g(fr);
}

uint64_t helper_subg (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = g_to_float64(a);
    fb = g_to_float64(b);
    fr = float64_sub(fa, fb, &FP_STATUS);
    return float64_to_g(fr);
}

uint64_t helper_mulg (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = g_to_float64(a);
    fb = g_to_float64(b);
    fr = float64_mul(fa, fb, &FP_STATUS);
    return float64_to_g(fr);
}

uint64_t helper_divg (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = g_to_float64(a);
    fb = g_to_float64(b);
    fr = float64_div(fa, fb, &FP_STATUS);
    return float64_to_g(fr);
}

uint64_t helper_sqrtg (uint64_t a)
{
    float64 fa, fr;

    fa = g_to_float64(a);
    fr = float64_sqrt(fa, &FP_STATUS);
    return float64_to_g(fr);
}


/* S floating (single) */
static always_inline uint64_t float32_to_s (float32 fa)
{
    CPU_FloatU a;
    uint64_t r;

    a.f = fa;

    r = (((uint64_t)(a.l & 0xc0000000)) << 32) | (((uint64_t)(a.l & 0x3fffffff)) << 29);
    if (((a.l & 0x7f800000) != 0x7f800000) && (!(a.l & 0x40000000)))
        r |= 0x7ll << 59;
    return r;
}

static always_inline float32 s_to_float32 (uint64_t a)
{
    CPU_FloatU r;
    r.l = ((a >> 32) & 0xc0000000) | ((a >> 29) & 0x3fffffff);
    return r.f;
}

uint32_t helper_s_to_memory (uint64_t a)
{
    /* Memory format is the same as float32 */
    float32 fa = s_to_float32(a);
    return *(uint32_t*)(&fa);
}

uint64_t helper_memory_to_s (uint32_t a)
{
    /* Memory format is the same as float32 */
    return float32_to_s(*(float32*)(&a));
}

uint64_t helper_adds (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = s_to_float32(a);
    fb = s_to_float32(b);
    fr = float32_add(fa, fb, &FP_STATUS);
    return float32_to_s(fr);
}

uint64_t helper_subs (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = s_to_float32(a);
    fb = s_to_float32(b);
    fr = float32_sub(fa, fb, &FP_STATUS);
    return float32_to_s(fr);
}

uint64_t helper_muls (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = s_to_float32(a);
    fb = s_to_float32(b);
    fr = float32_mul(fa, fb, &FP_STATUS);
    return float32_to_s(fr);
}

uint64_t helper_divs (uint64_t a, uint64_t b)
{
    float32 fa, fb, fr;

    fa = s_to_float32(a);
    fb = s_to_float32(b);
    fr = float32_div(fa, fb, &FP_STATUS);
    return float32_to_s(fr);
}

uint64_t helper_sqrts (uint64_t a)
{
    float32 fa, fr;

    fa = s_to_float32(a);
    fr = float32_sqrt(fa, &FP_STATUS);
    return float32_to_s(fr);
}


/* T floating (double) */
static always_inline float64 t_to_float64 (uint64_t a)
{
    /* Memory format is the same as float64 */
    CPU_DoubleU r;
    r.ll = a;
    return r.d;
}

static always_inline uint64_t float64_to_t (float64 fa)
{
    /* Memory format is the same as float64 */
    CPU_DoubleU r;
    r.d = fa;
    return r.ll;
}

uint64_t helper_addt (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = t_to_float64(a);
    fb = t_to_float64(b);
    fr = float64_add(fa, fb, &FP_STATUS);
    return float64_to_t(fr);
}

uint64_t helper_subt (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = t_to_float64(a);
    fb = t_to_float64(b);
    fr = float64_sub(fa, fb, &FP_STATUS);
    return float64_to_t(fr);
}

uint64_t helper_mult (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = t_to_float64(a);
    fb = t_to_float64(b);
    fr = float64_mul(fa, fb, &FP_STATUS);
    return float64_to_t(fr);
}

uint64_t helper_divt (uint64_t a, uint64_t b)
{
    float64 fa, fb, fr;

    fa = t_to_float64(a);
    fb = t_to_float64(b);
    fr = float64_div(fa, fb, &FP_STATUS);
    return float64_to_t(fr);
}

uint64_t helper_sqrtt (uint64_t a)
{
    float64 fa, fr;

    fa = t_to_float64(a);
    fr = float64_sqrt(fa, &FP_STATUS);
    return float64_to_t(fr);
}


/* Sign copy */
uint64_t helper_cpys(uint64_t a, uint64_t b)
{
    return (a & 0x8000000000000000ULL) | (b & ~0x8000000000000000ULL);
}

uint64_t helper_cpysn(uint64_t a, uint64_t b)
{
    return ((~a) & 0x8000000000000000ULL) | (b & ~0x8000000000000000ULL);
}

uint64_t helper_cpyse(uint64_t a, uint64_t b)
{
    return (a & 0xFFF0000000000000ULL) | (b & ~0xFFF0000000000000ULL);
}


/* Comparisons */
uint64_t helper_cmptun (uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = t_to_float64(a);
    fb = t_to_float64(b);

    if (float64_is_nan(fa) || float64_is_nan(fb))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmpteq(uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = t_to_float64(a);
    fb = t_to_float64(b);

    if (float64_eq(fa, fb, &FP_STATUS))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmptle(uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = t_to_float64(a);
    fb = t_to_float64(b);

    if (float64_le(fa, fb, &FP_STATUS))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmptlt(uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = t_to_float64(a);
    fb = t_to_float64(b);

    if (float64_lt(fa, fb, &FP_STATUS))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmpgeq(uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = g_to_float64(a);
    fb = g_to_float64(b);

    if (float64_eq(fa, fb, &FP_STATUS))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmpgle(uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = g_to_float64(a);
    fb = g_to_float64(b);

    if (float64_le(fa, fb, &FP_STATUS))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmpglt(uint64_t a, uint64_t b)
{
    float64 fa, fb;

    fa = g_to_float64(a);
    fb = g_to_float64(b);

    if (float64_lt(fa, fb, &FP_STATUS))
        return 0x4000000000000000ULL;
    else
        return 0;
}

uint64_t helper_cmpfeq (uint64_t a)
{
    return !(a & 0x7FFFFFFFFFFFFFFFULL);
}

uint64_t helper_cmpfne (uint64_t a)
{
    return (a & 0x7FFFFFFFFFFFFFFFULL);
}

uint64_t helper_cmpflt (uint64_t a)
{
    return (a & 0x8000000000000000ULL) && (a & 0x7FFFFFFFFFFFFFFFULL);
}

uint64_t helper_cmpfle (uint64_t a)
{
    return (a & 0x8000000000000000ULL) || !(a & 0x7FFFFFFFFFFFFFFFULL);
}

uint64_t helper_cmpfgt (uint64_t a)
{
    return !(a & 0x8000000000000000ULL) && (a & 0x7FFFFFFFFFFFFFFFULL);
}

uint64_t helper_cmpfge (uint64_t a)
{
    return !(a & 0x8000000000000000ULL) || !(a & 0x7FFFFFFFFFFFFFFFULL);
}


/* Floating point format conversion */
uint64_t helper_cvtts (uint64_t a)
{
    float64 fa;
    float32 fr;

    fa = t_to_float64(a);
    fr = float64_to_float32(fa, &FP_STATUS);
    return float32_to_s(fr);
}

uint64_t helper_cvtst (uint64_t a)
{
    float32 fa;
    float64 fr;

    fa = s_to_float32(a);
    fr = float32_to_float64(fa, &FP_STATUS);
    return float64_to_t(fr);
}

uint64_t helper_cvtqs (uint64_t a)
{
    float32 fr = int64_to_float32(a, &FP_STATUS);
    return float32_to_s(fr);
}

uint64_t helper_cvttq (uint64_t a)
{
    float64 fa = t_to_float64(a);
    return float64_to_int64_round_to_zero(fa, &FP_STATUS);
}

uint64_t helper_cvtqt (uint64_t a)
{
    float64 fr = int64_to_float64(a, &FP_STATUS);
    return float64_to_t(fr);
}

uint64_t helper_cvtqf (uint64_t a)
{
    float32 fr = int64_to_float32(a, &FP_STATUS);
    return float32_to_f(fr);
}

uint64_t helper_cvtgf (uint64_t a)
{
    float64 fa;
    float32 fr;

    fa = g_to_float64(a);
    fr = float64_to_float32(fa, &FP_STATUS);
    return float32_to_f(fr);
}

uint64_t helper_cvtgq (uint64_t a)
{
    float64 fa = g_to_float64(a);
    return float64_to_int64_round_to_zero(fa, &FP_STATUS);
}

uint64_t helper_cvtqg (uint64_t a)
{
    float64 fr;
    fr = int64_to_float64(a, &FP_STATUS);
    return float64_to_g(fr);
}

uint64_t helper_cvtlq (uint64_t a)
{
    return (int64_t)((int32_t)((a >> 32) | ((a >> 29) & 0x3FFFFFFF)));
}

static always_inline uint64_t __helper_cvtql (uint64_t a, int s, int v)
{
    uint64_t r;

    r = ((uint64_t)(a & 0xC0000000)) << 32;
    r |= ((uint64_t)(a & 0x7FFFFFFF)) << 29;

    if (v && (int64_t)((int32_t)r) != (int64_t)r) {
        helper_excp(EXCP_GEN_ARITH, EXCP_ARITH_OVERFLOW);
    }
    if (s) {
        /* TODO */
    }
    return r;
}

uint64_t helper_cvtql (uint64_t a)
{
    return __helper_cvtql(a, 0, 0);
}

uint64_t helper_cvtqlv (uint64_t a)
{
    return __helper_cvtql(a, 0, 1);
}

uint64_t helper_cvtqlsv (uint64_t a)
{
    return __helper_cvtql(a, 1, 1);
}

/* PALcode support special instructions */
#if !defined (CONFIG_USER_ONLY)
void helper_hw_rei (void)
{
#if 0
    /* FIXME: For 21064/21164 only ?  */
    env->pc = env->any.ipr[IPR_EXC_ADDR] & ~3;
    env->any.ipr[IPR_EXC_ADDR] = env->any.ipr[IPR_EXC_ADDR] & 1;
    /* XXX: re-enable interrupts and memory mapping */
#else
    cpu_abort(env, "hw_rei not implemented\n");
#endif
}

void helper_hw_ret (uint64_t a)
{
    switch (env->pal_emul) {
    case PAL_21264:
        if (!(a & 1) && env->a21264.isum) {
#if 0
            qemu_log("pal mode ret interrupt ier=%016llx, isum=%016llx ir=%x\n",
                     env->a21264.ier, env->a21264.isum,
                     env->interrupt_request);
#endif
            /* Very fast interrupt delivery!  */
            env->exc_addr = a;
            env->pc = env->pal_base + EXCP_21264_INTERRUPT;
            env->interrupt_request &= ~CPU_INTERRUPT_HARD;
            break;
        }
        env->pc = a & ~3;
        if (env->pal_mode != (a & 1)) {
            env->pal_mode = a & 1;
            if (!env->pal_mode)
                env->mmu_code_index = env->mmu_data_index;
            else
                env->mmu_code_index = MMU_PAL_IDX;
            if (env->a21264.sde1 && !(a & 1))
                swap_shadow_21264(env);
        }
        break;
    case PAL_NONE:
        cpu_abort(env, "hw_ret: not supported by pal emulation\n");
    }
}

uint64_t helper_mfpr (int iprn, uint64_t val)
{
    switch (env->pal_emul) {
    case PAL_21264:
      return cpu_alpha_mfpr_21264(env, iprn);
      break;
    case PAL_NONE:
        cpu_abort(env, "hw_mfpr: not supported by pal emulation\n");
    }
    return val;
}

void helper_mtpr (int iprn, uint64_t val)
{
    switch (env->pal_emul) {
    case PAL_21264:
      cpu_alpha_mtpr_21264(env, iprn, val);
      return;
    case PAL_NONE:
        cpu_abort(env, "hw_mtpr: not supported by pal emulation\n");
    }
}
#endif

/*****************************************************************************/
/* Softmmu support */
#if !defined (CONFIG_USER_ONLY)

struct hw_virt2phys_param {
    unsigned char op;
    unsigned char en_sh; /* KRE/KWE bit pos */
    unsigned char fo_sh; /* FOR/FOW bit pos */
};

static uint64_t hw_virt2phys (uint64_t virtaddr, uint32_t v2p_flags,
                              struct hw_virt2phys_param p)
{
    int mmu_idx;
    struct alpha_pte pte;

    mmu_idx = v2p_flags & ALPHA_HW_MMUIDX_MASK;
    pte = cpu_alpha_mmu_v2p_21264(env, virtaddr, 0);
    if (!(pte.fl & ALPHA_PTE_V)) {
        if (v2p_flags & ALPHA_HW_V) {
            /* Virtual pte access.  */
            env->exception_index = env->a21264.iva_48 ?
                EXCP_21264_DTBM_DOUBLE_4 : EXCP_21264_DTBM_DOUBLE_3;
        } else {
            env->exception_index = EXCP_21264_DTBM_SINGLE;
            env->a21264.mm_stat = (p.op << 4)
                | (pte.asn == PTE_ASN_BAD_VA ? 2 : 0);
            env->a21264.va = virtaddr;
        }
        cpu_alpha_mmu_dfault_21264(env, virtaddr);
        cpu_loop_exit();
    }
    if ((v2p_flags & ALPHA_HW_W)
        && (!((pte.fl >> (mmu_idx + p.en_sh)) & 1)
            || ((pte.fl >> p.fo_sh) & 1))) {
        env->exception_index = EXCP_21264_DFAULT;
        env->a21264.mm_stat = (p.op << 4)
            | ((pte.fl >> (mmu_idx + p.en_sh)) & 1 ? 0 : 2)
            | ((pte.fl >> p.fo_sh) & 1 ? 4 : 0);
        env->a21264.va = virtaddr;
        cpu_alpha_mmu_dfault_21264(env, virtaddr);
        cpu_loop_exit();
    }
    return (((uint64_t)pte.pa) << 13) | (virtaddr & ~TARGET_PAGE_MASK);
}

static const struct hw_virt2phys_param hw_ld_param = { 0x03, 8, 1 };

#define HELPER_21264_hw_ldX(S)                                          \
uint64_t helper_21264_hw_ld##S (uint64_t va, uint32_t v2p_flags)        \
{                                                                       \
    uint64_t tlb_addr, pa;                                              \
    int index, mmu_idx;                                                 \
                                                                        \
    if (v2p_flags & ALPHA_HW_A)                                         \
        mmu_idx = env->a21264.altmode;                                  \
    else                                                                \
        mmu_idx = v2p_flags & ALPHA_HW_MMUIDX_MASK;                     \
    index = (va >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);              \
    tlb_addr = env->tlb_table[mmu_idx][index].addr_read;                \
    if ((va & TARGET_PAGE_MASK) ==                                      \
        (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {           \
        pa = va + env->tlb_table[mmu_idx][index].addend;                \
        return ld##S##_raw((uint8_t *)pa);                              \
    } else {                                                            \
        pa = hw_virt2phys(va, v2p_flags | mmu_idx, hw_ld_param);        \
        return ld##S##_phys(pa);                                        \
    }                                                                   \
}

HELPER_21264_hw_ldX(q)
HELPER_21264_hw_ldX(l)

static const struct hw_virt2phys_param hw_st_param = { 0x07, 12, 2 };

#define HELPER_21264_hw_stX(S)                                          \
void helper_21264_hw_st##S(uint64_t va, uint64_t val,                   \
                           uint32_t v2p_flags)                          \
{                                                                       \
    uint64_t tlb_addr, pa;                                              \
    int index, mmu_idx;                                                 \
                                                                        \
    if (v2p_flags & ALPHA_HW_A)                                         \
        mmu_idx = env->a21264.altmode;                                  \
    else                                                                \
        mmu_idx = v2p_flags & ALPHA_HW_MMUIDX_MASK;                     \
    index = (va >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);              \
    tlb_addr = env->tlb_table[mmu_idx][index].addr_write;               \
    if ((va & TARGET_PAGE_MASK) ==                                      \
        (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {           \
        pa = va + env->tlb_table[mmu_idx][index].addend;                \
        st##S##_raw((uint8_t *)pa, val);                                \
    } else {                                                            \
        pa = hw_virt2phys(va, v2p_flags | mmu_idx, hw_st_param);        \
        st##S##_phys(pa, val);                                          \
    }                                                                   \
}

HELPER_21264_hw_stX(q)
HELPER_21264_hw_stX(l)

uint64_t helper_ldl_phys(uint64_t addr)
{
    return ldl_phys(addr);
}

uint64_t helper_ldq_phys(uint64_t addr)
{
    return ldq_phys(addr);
}

uint64_t helper_ldl_l_phys(uint64_t addr)
{
    env->lock = addr;
    return ldl_phys(addr);
}

uint64_t helper_ldq_l_phys(uint64_t addr)
{
    env->lock = addr;
    return ldl_raw(addr);
}

uint64_t helper_ldl_data(uint64_t addr)
{
    /* FIXME: ldl_data won't work in case of fault  */
    cpu_abort(env, "ldl_data not implemented\n");
    return ldl_data(addr);
}

uint64_t helper_ldq_data(uint64_t addr)
{
    /* FIXME: ldq_data won't work in case of fault  */
    cpu_abort(env, "ldq_data not implemented\n");
    return ldq_data(addr);
}

void helper_stl_phys(uint64_t val, uint64_t addr)
{
    stl_phys(addr, val);
}

void helper_stq_phys(uint64_t val, uint64_t addr)
{
    stq_phys(addr, val);
}

uint64_t helper_stl_c_phys(uint64_t val, uint64_t addr)
{
    uint64_t ret;

    if (addr == env->lock) {
        stl_phys(addr, val);
        ret = 0;
    } else
        ret = 1;

    env->lock = 1;

    return ret;
}

uint64_t helper_stq_c_phys(uint64_t val, uint64_t addr)
{
    uint64_t ret;

    if (addr == env->lock) {
        stq_phys(addr, val);
        ret = 0;
    } else
        ret = 1;

    env->lock = 1;

    return ret;
}

#define MMUSUFFIX _mmu

#define SHIFT 0
#include "softmmu_template.h"

#define SHIFT 1
#include "softmmu_template.h"

#define SHIFT 2
#include "softmmu_template.h"

#define SHIFT 3
#include "softmmu_template.h"

/* try to fill the TLB and return an exception if error. If retaddr is
   NULL, it means that the function was called in C code (i.e. not
   from generated code or from helper.c) */
/* XXX: fix it to restore all registers */
void tlb_fill (target_ulong addr, int rwx, int mmu_idx, void *retaddr)
{
    CPUState *saved_env;
    int ret;

    /* XXX: hack to restore env in all cases, even if not called from
       generated code */
    saved_env = env;
    env = cpu_single_env;

    if (rwx == 2 && mmu_idx == MMU_PAL_IDX)
        cpu_alpha_mmu_fault_pal(env, addr);
    else {
        switch (env->pal_emul) {
        case PAL_21264:
            ret = cpu_alpha_mmu_fault_21264(env, addr, rwx, mmu_idx, retaddr);
            if (!likely(ret == 0))
                /* Exception index and error code are already set */
                cpu_loop_exit();
            break;
        case PAL_NONE:
            cpu_abort(env, "tlb_fill: not supported by pal emulation\n");
        }
    }
    env = saved_env;
}

#endif
