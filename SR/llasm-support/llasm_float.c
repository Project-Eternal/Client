// part of static recompiler -- do not edit

/**
 *
 *  Copyright (C) 2019-2026 Roman Pauer
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#include "llasm_cpu.h"
#if !defined(__USE_ISOC99)
    #define __USE_ISOC99
#endif
#if !defined(_USE_MATH_DEFINES)
    #define _USE_MATH_DEFINES
#endif
#include <math.h>
#include <string.h>

#if defined(__SSE2__)
    #include <emmintrin.h>
#endif

#if defined(_MSC_VER)

#undef BIG_ENDIAN_FLOAT_WORD_ORDER
#undef BIG_ENDIAN_BYTE_ORDER

#elif defined(__BYTE_ORDER__)

#if (defined(__FLOAT_WORD_ORDER__) && (__FLOAT_WORD_ORDER__ == __ORDER_BIG_ENDIAN__)) || (!defined(__FLOAT_WORD_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
#define BIG_ENDIAN_FLOAT_WORD_ORDER
#else
#undef BIG_ENDIAN_FLOAT_WORD_ORDER
#endif

#if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define BIG_ENDIAN_BYTE_ORDER
#else
#undef BIG_ENDIAN_BYTE_ORDER
#endif

#else

#include <endian.h>
#if (__FLOAT_WORD_ORDER == __BIG_ENDIAN)
#define BIG_ENDIAN_FLOAT_WORD_ORDER
#else
#undef BIG_ENDIAN_FLOAT_WORD_ORDER
#endif

#if (__BYTE_ORDER == __BIG_ENDIAN)
#define BIG_ENDIAN_BYTE_ORDER
#else
#undef BIG_ENDIAN_BYTE_ORDER
#endif

#endif

typedef union {
    float f;
    int32_t i;
} float_int;

typedef union {
    double d;
    struct {
#ifdef BIG_ENDIAN_FLOAT_WORD_ORDER
        uint32_t high;
        uint32_t low;
#else
        uint32_t low;
        uint32_t high;
#endif
    };
} double_int;

typedef union {
    int64_t i;
    struct {
#ifdef BIG_ENDIAN_BYTE_ORDER
        uint32_t high;
        uint32_t low;
#else
        uint32_t low;
        uint32_t high;
#endif
    };
} int_int;

typedef struct {
    uint32_t low;
    uint32_t high;
} le_int;


const static double const_0_0 = 0.0;
const static double const_1_0 = 1.0;
const static double const_lg2 = 0.30102999566398119521; // log10l(2.0l)
const static double const_ln2 = M_LN2;

#define st cpu->_st
#define st_result cpu->_st_result

#define st_top cpu->_st_top
#define st_sw_cond cpu->_st_sw_cond
#define st_cw cpu->_st_cw

#define X87_CF 0x0100
#define X87_ZF 0x4000

#define X87_C0 0x0100
#define X87_C1 0x0200
#define X87_C2 0x0400
#define X87_C3 0x4000

#define X87_CX 0x4700

#define X87_RC_SHIFT 10

#define CLEAR_X87_FLAG(x) { st_sw_cond &= (~(x)) & X87_CX; }

/* loads, stores and arithmetic change only C1: C3, C2 and C0 keep the result of an earlier compare (compiled code reads them
   with fnstsw after an fst, e.g. 0x76d08c: fcom, fst, fnstsw) */
#define CLEAR_X87_FLAGS CLEAR_X87_FLAG(X87_C1)

#define X87_CMP(x, y)  { \
    if ( (x) < (y) ) { \
        st_sw_cond = X87_CF; \
    } else if ( (x) == (y) ) { \
        st_sw_cond = X87_ZF; \
    } else if ( (x) > (y) ) { \
        st_sw_cond = 0; \
    } else { \
        st_sw_cond = X87_ZF | X87_C2 | X87_CF; /* unordered (a NaN): C3, C2 and C0 */ \
    } \
}

#define ST(i) st[(st_top+i) & 7]
#define ST0 st[st_top]
#define ST1 ST(1)


#define PUSH_REGS { { CLEAR_X87_FLAG(X87_C1); st_top = (st_top + 7) & 7; } }
#define POP_REGS { st_top = (st_top + 1) & 7; CLEAR_X87_FLAG(X87_C1); }
#define POP2_REGS { st_top = (st_top + 2) & 7; CLEAR_X87_FLAG(X87_C1); }


// fpu instructions

EXTERNC void CCALL x87_fabs_void(CPU)
{
    ST0 = fabs(ST0);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fadd_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    ST0 += num.f;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fadd_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    ST0 += num.d;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fadd_st(CPU, int num)
{
    ST0 += ST(num);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fadd_to_st(CPU, int num)
{
    ST(num) += ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_faddp_st(CPU, int num)
{
    ST(num) += ST0;
    POP_REGS;
}

EXTERNC void CCALL x87_fchs_void(CPU)
{
    ST0 = -ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fcom_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    X87_CMP(ST0, num.f)
}

EXTERNC void CCALL x87_fcom_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    X87_CMP(ST0, num.d)
}

EXTERNC void CCALL x87_fcomp_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    X87_CMP(ST0, num.f)
    POP_REGS;
}

EXTERNC void CCALL x87_fcomp_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    X87_CMP(ST0, num.d)
    POP_REGS;
}

EXTERNC void CCALL x87_fcos_void(CPU)
{
    ST0 = cos(ST0);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdiv_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    ST0 /= num.f;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdiv_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    ST0 /= num.d;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdiv_st(CPU, int num)
{
    ST0 /= ST(num);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdiv_to_st(CPU, int num)
{
    ST(num) /= ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdivp_st(CPU, int num)
{
    ST(num) /= ST0;
    POP_REGS;
}

EXTERNC void CCALL x87_fdivr_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    ST0 = num.f / ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdivr_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    ST0 = num.d / ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fdivr_st(CPU, int num)
{
    ST0 = ST(num) / ST0;
}

EXTERNC void CCALL x87_fdivrp_st(CPU, int num)
{
    ST(num) = ST0 / ST(num);
    POP_REGS;
}

EXTERNC void CCALL x87_fiadd_int32(CPU, int32_t num)
{
    ST0 += num;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fidiv_int32(CPU, int32_t num)
{
    ST0 /= num;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fidivr_int32(CPU, int32_t num)
{
    ST0 = num / ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fimul_int32(CPU, int32_t num)
{
    ST0 *= num;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fild_int32(CPU, int32_t num)
{
    PUSH_REGS;
    ST0 = num;
}

EXTERNC void CCALL x87_fild_int64(CPU, uint32_t num_low, uint32_t num_high)
{
    int_int num;

    num.low = num_low;
    num.high = num_high;
    PUSH_REGS;
    ST0 = (double)num.i;
}

EXTERNC void CCALL x87_fninit_void(CPU)
{
    st_top = 0;
    st_sw_cond = 0;
    st_cw = 0x037f;
}

EXTERNC int32_t CCALL x87_fist_int32(CPU)
{
    double dval, orig;
    int32_t ival;

    switch ((st_cw >> X87_RC_SHIFT) & 3)
    {
    case 0: // Round to nearest (even)
        orig = ST0;
        dval = floor(orig);
        if (orig - dval > 0.5)
        {
            dval += 1.0;
        }
        else if (!(orig - dval < 0.5))
        {
            CLEAR_X87_FLAGS;
            if ((dval < 2147483648.0) && (dval > -2147483648.0))
            {
                ival = (int32_t) dval;
                ival += ival & 1;
            }
            else
            {
                ival = (int32_t) 0x80000000;
            }
            return ival;
        }
        break;
    case 1: // Round down (toward -infinity)
        dval = floor(ST0);
        break;
    case 2: // Round up (toward +infinity)
        dval = ceil(ST0);
        break;
    case 3: // Round toward zero (Truncate)
        dval = trunc(ST0);
        break;
    }
    CLEAR_X87_FLAGS;
    return ((dval < 2147483648.0) && (dval > -2147483648.0))?((int32_t) dval):((int32_t) 0x80000000);
}

EXTERNC int16_t CCALL x87_fistp_int16(CPU)
{
    double dval, orig;
    int16_t ival;

    switch ((st_cw >> X87_RC_SHIFT) & 3)
    {
    case 0: // Round to nearest (even)
        orig = ST0;
        dval = floor(orig);
        if (orig - dval > 0.5)
        {
            dval += 1.0;
        }
        else if (!(orig - dval < 0.5))
        {
            POP_REGS;
            if ((dval < 32768.0) && (dval > -32768.0))
            {
                ival = (int16_t) dval;
                ival += ival & 1;
            }
            else
            {
                ival = (int16_t) 0x8000;
            }
            return ival;
        }
        break;
    case 1: // Round down (toward -infinity)
        dval = floor(ST0);
        break;
    case 2: // Round up (toward +infinity)
        dval = ceil(ST0);
        break;
    case 3: // Round toward zero (Truncate)
        dval = trunc(ST0);
        break;
    }
    POP_REGS;
    return ((dval < 32768.0) && (dval > -32768.0))?((int16_t) dval):((int16_t) 0x8000);
}

EXTERNC int32_t CCALL x87_fistp_int32(CPU)
{
    double dval, orig;
    int32_t ival;

    switch ((st_cw >> X87_RC_SHIFT) & 3)
    {
    case 0: // Round to nearest (even)
        orig = ST0;
        dval = floor(orig);
        if (orig - dval > 0.5)
        {
            dval += 1.0;
        }
        else if (!(orig - dval < 0.5))
        {
            POP_REGS;
            if ((dval < 2147483648.0) && (dval > -2147483648.0))
            {
                ival = (int32_t) dval;
                ival += ival & 1;
            }
            else
            {
                ival = (int32_t) 0x80000000;
            }
            return ival;
        }
        break;
    case 1: // Round down (toward -infinity)
        dval = floor(ST0);
        break;
    case 2: // Round up (toward +infinity)
        dval = ceil(ST0);
        break;
    case 3: // Round toward zero (Truncate)
        dval = trunc(ST0);
        break;
    }
    POP_REGS;
    return ((dval < 2147483648.0) && (dval > -2147483648.0))?((int32_t) dval):((int32_t) 0x80000000);
}

EXTERNC uint32_t CCALL x87_fistp_int64(CPU)
{
    double orig, dval;
#ifdef BIG_ENDIAN_BYTE_ORDER
    int_int uval;
    le_int *presult;
    #define ival uval.i
#else
    int64_t ival;
#endif

    orig = ST0;
    POP_REGS;

    if ((orig >= 9223372036854775808.0) || (orig <= -9223372036854775808.0))
    {
#ifdef BIG_ENDIAN_BYTE_ORDER
        uval.i = INT64_C(0x8000000000000000);

        presult = (le_int *)&(st_result);

        presult->low = uval.low;
        presult->high = uval.high;

        return PTR2REG(presult);
#else
        st_result = INT64_C(0x8000000000000000);
        return PTR2REG(&(st_result));
#endif
    }

    switch ((st_cw >> X87_RC_SHIFT) & 3)
    {
    case 0: // Round to nearest (even)
        dval = floor(orig);
        ival = (int64_t) dval;
        if (orig - dval > 0.5)
        {
            ival++;
        }
        else if (!(orig - dval < 0.5))
        {
            ival += ival & 1;
        }
        break;
    case 1: // Round down (toward -infinity)
        ival = (int64_t) floor(orig);
        break;
    case 2: // Round up (toward +infinity)
        ival = (int64_t) ceil(orig);
        break;
    case 3: // Round toward zero (Truncate)
        ival = (int64_t) trunc(orig);
        break;
    }

#ifdef BIG_ENDIAN_BYTE_ORDER
    #undef ival

    presult = (le_int *)&(st_result);

    presult->low = uval.low;
    presult->high = uval.high;

    return PTR2REG(presult);
#else
    st_result = ival;
    return PTR2REG(&(st_result));
#endif
}

EXTERNC void CCALL x87_fisub_int32(CPU, int32_t num)
{
    ST0 -= num;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fisubr_int32(CPU, int32_t num)
{
    ST0 = num - ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fld_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    PUSH_REGS;
    ST0 = num.f;
}

EXTERNC void CCALL x87_fld_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int *pst0;

    PUSH_REGS;
    pst0 = (double_int *) &(ST0);
    pst0->low = num_low;
    pst0->high = num_high;
}

EXTERNC void CCALL x87_fld_st(CPU, int num)
{
    double newval;

    newval = ST(num);
    PUSH_REGS;
    ST0 = newval;
}

EXTERNC void CCALL x87_fld1_void(CPU)
{
    PUSH_REGS;
    ST0 = const_1_0;
}

EXTERNC void CCALL x87_fldlg2_void(CPU)
{
    PUSH_REGS;
    ST0 = const_lg2;
}

EXTERNC void CCALL x87_fldln2_void(CPU)
{
    PUSH_REGS;
    ST0 = const_ln2;
}

EXTERNC void CCALL x87_fldz_void(CPU)
{
    PUSH_REGS;
    ST0 = const_0_0;
}

EXTERNC void CCALL x87_fldcw_uint16(CPU, uint32_t new_cw)
{
    st_cw = new_cw & 0xffff;
}

EXTERNC void CCALL x87_fmul_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    ST0 *= num.f;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fmul_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    ST0 *= num.d;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fmul_st(CPU, int num)
{
    ST0 *= ST(num);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fmul_to_st(CPU, int num)
{
    ST(num) *= ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fmulp_st(CPU, int num)
{
    ST(num) *= ST0;
    POP_REGS;
}

EXTERNC void CCALL x87_fptan_void(CPU)
{
    ST0 = tan(ST0);
    PUSH_REGS;
    ST0 = const_1_0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_frstor_void(CPU, const uint8_t *state)
{
    uint32_t sw;
    int index;
    double_int value;
    int_int mantissa;
    uint32_t exponent, sign;

    st_cw = *(uint32_t *)(state) & 0xffff;
    sw = *(uint32_t *)(state + 4);
    st_top = (sw >> 11) & 7;
    st_sw_cond = sw & X87_CX;

    for (index = 0; index <= 7; index++)
    {
        mantissa.low = *(uint32_t *)(state + 28 + index * 10);
        mantissa.high = *(uint32_t *)(state + 32 + index * 10);
        exponent = *(uint16_t *)(state + 36 + index * 10);

        mantissa.i = (mantissa.i >> 11) & UINT64_C(0x000fffffffffffff);
        sign = (exponent & 0x8000) << 16;
        exponent = (exponent & 0x7fff) + 1023 - 16383; // adjust bias
        if (exponent >= 2048) exponent = 2047;
        else if (exponent < 0) exponent = 0;

        value.low = mantissa.low;
        value.high = mantissa.high | (exponent << 20) | sign;

        ST(index) = value.d;
    }
}

EXTERNC void CCALL x87_fnsave_void(CPU, uint8_t *state)
{
    int index;
    double_int value;
    int_int mantissa;
    uint32_t exponent, sign;

    *(uint32_t *)(state) = st_cw;
    *(uint32_t *)(state + 4) = st_sw_cond | (st_top << 11);
    *(uint32_t *)(state + 8) = 0;

    for (index = 0; index <= 7; index++)
    {
        value.d = ST(index);

        mantissa.low = value.low;
        mantissa.high = value.high;

        mantissa.i = (mantissa.i & UINT64_C(0x000fffffffffffff)) << 11;
        exponent = (value.high >> 20) & 0x07ff;
        sign = (value.high >> 16) & 0x8000;

        if (value.d != 0)
        {
            mantissa.i |= UINT64_C(0x8000000000000000);
            exponent = exponent + 16383 - 1023; // adjust bias
        }

        *(uint32_t *)(state + 28 + index * 10) = mantissa.low;
        *(uint32_t *)(state + 32 + index * 10) = mantissa.high;
        *(uint16_t *)(state + 36 + index * 10) = exponent | sign;
    }

    st_top = 0;
    st_sw_cond = 0;
    st_cw = 0x037f;
}

EXTERNC void CCALL x87_fsin_void(CPU)
{
    ST0 = sin(ST0);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsqrt_void(CPU)
{
    ST0 = sqrt(ST0);
}

EXTERNC int32_t CCALL x87_fst_float(CPU)
{
    float_int ret;

    CLEAR_X87_FLAGS;
    ret.f = (float)ST0;

    return ret.i;
}

EXTERNC uint32_t CCALL x87_fst_double(CPU)
{
#ifdef BIG_ENDIAN_FLOAT_WORD_ORDER
    double_int *pst0;
    le_int *presult;

    CLEAR_X87_FLAGS;
    pst0 = (double_int *)&(ST0);
    presult = (le_int *)&(st_result);

    presult->low = pst0->low;
    presult->high = pst0->high;

    return PTR2REG(presult);
#else
    void *presult;

    CLEAR_X87_FLAGS;
    presult = &(ST0);

    return PTR2REG(presult);
#endif
}

EXTERNC void CCALL x87_fst_st(CPU, int num)
{
    CLEAR_X87_FLAGS;
    ST(num) = ST0;
}

EXTERNC int32_t CCALL x87_fstp_float(CPU)
{
    float_int ret;

    ret.f = (float)ST0;
    POP_REGS;
    return ret.i;
}

EXTERNC uint32_t CCALL x87_fstp_double(CPU)
{
#ifdef BIG_ENDIAN_FLOAT_WORD_ORDER
    double_int *pst0;
    le_int *presult;

    pst0 = (double_int *)&(ST0);
    presult = (le_int *)&(st_result);

    presult->low = pst0->low;
    presult->high = pst0->high;

    POP_REGS;
    return PTR2REG(presult);
#else
    void *presult;

    presult = &(ST0);
    POP_REGS;
    return PTR2REG(presult);
#endif
}

EXTERNC void CCALL x87_fstp_st(CPU, int num)
{
    ST(num) = ST0;
    POP_REGS;
}

EXTERNC uint32_t CCALL x87_fnstcw_void(CPU)
{
    return st_cw;
}

EXTERNC uint32_t CCALL x87_fnstsw_void(CPU)
{
    return st_sw_cond | (st_top << 11);
}

EXTERNC void CCALL x87_fsub_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    ST0 -= num.f;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsub_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    ST0 -= num.d;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsub_st(CPU, int num)
{
    ST0 -= ST(num);
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsub_to_st(CPU, int num)
{
    ST(num) -= ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsubp_st(CPU, int num)
{
    ST(num) -= ST0;
    POP_REGS;
}

EXTERNC void CCALL x87_fsubr_float(CPU, uint32_t num_bits)
{
    float_int num;

    num.i = (int32_t) num_bits;
    ST0 = num.f - ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsubr_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    num.low = num_low;
    num.high = num_high;
    ST0 = num.d - ST0;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fsubr_st(CPU, int num)
{
    ST0 = ST(num) - ST0;
}

EXTERNC void CCALL x87_fsubrp_st(CPU, int num)
{
    ST(num) = ST0 - ST(num);
    POP_REGS;
}

EXTERNC void CCALL x87_fucom_st(CPU, int num)
{
    X87_CMP(ST0, ST(num))
}

EXTERNC void CCALL x87_fucomp_st(CPU, int num)
{
    X87_CMP(ST0, ST(num))
    POP_REGS;
}

EXTERNC void CCALL x87_fucompp_void(CPU)
{
    X87_CMP(ST0, ST1)
    POP2_REGS;
}

EXTERNC void CCALL x87_fxch_st(CPU, int num)
{
    double tmpst;

    tmpst = ST0;
    ST0 = ST(num);
    ST(num) = tmpst;
    CLEAR_X87_FLAGS;
}

EXTERNC void CCALL x87_fyl2x_void(CPU)
{
    ST1 *= log2(ST0);
    POP_REGS;
}


// math functions

// double acos(double x);
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_facos_void(CPU)
{
    ST0 = acos(ST0);
}

// double asin(double x);
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_fasin_void(CPU)
{
    ST0 = asin(ST0);
}

// double atan2(double y, double x);
//  - arc tangent of y/x
// y = ST0
// x = ST1
// return value = ST0
EXTERNC void CCALL x87_fatan2_void(CPU)
{
    ST1 = atan2(ST0, ST1);
    POP_REGS;
}

// double atan2(double y, double x);
//  - arc tangent of y/x
// y = ST1
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_fatan2r_void(CPU)
{
    ST1 = atan2(ST1, ST0);
    POP_REGS;
}

// double log(double x);
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_flog_void(CPU)
{
    ST0 = log(ST0);
}

// double log10(double x);
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_flog10_void(CPU)
{
    ST0 = log10(ST0);
}

// double floor(double x);
// x = stack
// return value = ST0
EXTERNC void CCALL x87_floor_double(CPU, uint32_t num_low, uint32_t num_high)
{
    double_int num;

    PUSH_REGS;
    num.low = num_low;
    num.high = num_high;
    ST0 = floor(num.d);
}

// double fmod(double x, double y);
//  - the  floating-point remainder of dividing x by y
// x = ST0
// y = ST1
// return value = ST0
EXTERNC void CCALL x87_fmod_void(CPU)
{
    ST1 = fmod(ST0, ST1);
    POP_REGS;
}

// double fmod(double x, double y);
//  - the  floating-point remainder of dividing x by y
// x = ST1
// y = ST0
// return value = ST0
EXTERNC void CCALL x87_fmodr_void(CPU)
{
    ST1 = fmod(ST1, ST0);
    POP_REGS;
}

// double pow(double x, double y);
//  - the value of x raised to the power of y
// x = ST0
// y = ST1
// return value = ST0
EXTERNC void CCALL x87_fpow_void(CPU)
{
    ST1 = pow(ST0, ST1);
    POP_REGS;
}

// double pow(double x, double y);
//  - the value of x raised to the power of y
// x = ST1
// y = ST0
// return value = ST0
EXTERNC void CCALL x87_fpowr_void(CPU)
{
    ST1 = pow(ST1, ST0);
    POP_REGS;
}

// double round(double x);
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_fround_void(CPU)
{
    ST0 = round(ST0);
}

// double tan(double x);
// x = ST0
// return value = ST0
EXTERNC void CCALL x87_ftan_void(CPU)
{
    ST0 = tan(ST0);
}

// truncate toward zero
EXTERNC int32_t CCALL x87_ftol_int32(CPU)
{
#if defined(__SSE2__)
    int32_t result;

    result = _mm_cvttsd_si32(_mm_load1_pd(&(ST0)));
    POP_REGS;
    return result;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    int32_t result;

    result = (int32_t)trunc(ST0);
    POP_REGS;
    return result;
#else
    const static double doublemagic = 6755399441055744.0; // 2^52 * 1.5

    double_int result;
    double num1, num2;

    num1 = ST0;
    POP_REGS;

    if (num1 < 0)
    {
        if (num1 <= -2147483648.0) return (int32_t) 0x80000000;

        result.d = num1 + doublemagic;  // fast conversion to int,
        num2 = (double)(int32_t)result.low; // result.low contains the result (rounded up or down)

        if (num2 < num1) // compare result with original value and if the result was rounded toward negative infinity, then increase result (truncate toward 0)
        {
            result.low++;
        }

        return (int32_t)result.low;
    }
    else
    {
        if (num1 >= 2147483648.0) return (int32_t) 0x80000000;

        result.d = num1 + doublemagic;  // fast conversion to int,
        if (0 > (int32_t)result.low) return (int32_t) 0x7fffffff;
        num2 = (double)(int32_t)result.low; // result.low contains the result (rounded up or down)

        if (num2 > num1) // compare result with original value and if the result was rounded toward positive infinity, then decrease result (truncate toward 0)
        {
            result.low--;
        }

        return (int32_t)result.low;
    }
#endif
}

// truncate toward zero
EXTERNC uint32_t CCALL x87_ftol_int64(CPU)
{
    double orig;

    orig = ST0;
    POP_REGS;

#ifdef BIG_ENDIAN_BYTE_ORDER
    int_int ret;
    le_int *presult;

    ret.i = ((orig < 9223372036854775808.0) && (orig > -9223372036854775808.0))?((int64_t) trunc(orig)):INT64_C(0x8000000000000000);

    presult = (le_int *)&(st_result);

    presult->low = ret.low;
    presult->high = ret.high;

    return PTR2REG(presult);
#else
    st_result = ((orig < 9223372036854775808.0) && (orig > -9223372036854775808.0))?((int64_t) trunc(orig)):INT64_C(0x8000000000000000);
    return PTR2REG(&(st_result));
#endif
}



// ---------------------------------------------------------------------
// instructions used by the 32-bit Windows C runtime
// the emulated registers are doubles, so 80-bit values lose precision

const static double const_pi = M_PI;
const static double const_l2e = M_LOG2E;
const static double const_l2t = 3.32192809488736234787; // log2(10)

static double x87_tword_to_double(const uint8_t *p)
{
    uint64_t mant;
    uint32_t sign_exp;
    int exp;
    double result;
    int i;

    mant = 0;
    for (i = 7; i >= 0; i--)
    {
        mant = (mant << 8) | p[i];
    }
    sign_exp = p[8] | ((uint32_t)p[9] << 8);
    exp = sign_exp & 0x7fff;

    if (exp == 0x7fff)
    {
        result = ((mant << 1) == 0) ? INFINITY : NAN;
    }
    else if (mant == 0)
    {
        result = 0.0;
    }
    else
    {
        // the 64-bit mantissa has its integer bit explicit
        result = ldexp((double)mant, ((exp == 0) ? 1 : exp) - 16383 - 63);
    }

    return (sign_exp & 0x8000) ? -result : result;
}

static void x87_double_to_tword(double value, uint8_t *p)
{
    double_int v;
    uint64_t mant;
    uint32_t sign_exp;
    int exp, biased;
    int i;

    v.d = value;
    {
        uint64_t bits;

        bits = ((uint64_t)v.high << 32) | v.low;
        sign_exp = (uint32_t)(bits >> 63) << 15;
        biased = (int)((bits >> 52) & 0x7ff);
        bits &= UINT64_C(0xfffffffffffff);

        if (biased == 0x7ff)
        {
            exp = 0x7fff;
            mant = UINT64_C(0x8000000000000000) | (bits << 11);
        }
        else if (biased == 0)
        {
            if (bits == 0)
            {
                exp = 0;
                mant = 0;
            }
            else
            {
                // denormal double: normalize
                int shift;

                shift = 0;
                while (!(bits & UINT64_C(0x8000000000000000)))
                {
                    bits <<= 1;
                    shift++;
                }
                mant = bits;
                exp = 15372 - shift;
            }
        }
        else
        {
            exp = biased - 1023 + 16383;
            mant = UINT64_C(0x8000000000000000) | (bits << 11);
        }
    }

    sign_exp |= (uint32_t)exp;

    for (i = 0; i < 8; i++)
    {
        p[i] = (uint8_t)(mant >> (8 * i));
    }
    p[8] = (uint8_t)sign_exp;
    p[9] = (uint8_t)(sign_exp >> 8);
}

static uint32_t x87_read_le16(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}

static void x87_write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

EXTERNC void CCALL x87_fldpi_void(CPU)
{
    PUSH_REGS;
    ST0 = const_pi;
}

EXTERNC void CCALL x87_fldl2e_void(CPU)
{
    PUSH_REGS;
    ST0 = const_l2e;
}

EXTERNC void CCALL x87_fldl2t_void(CPU)
{
    PUSH_REGS;
    ST0 = const_l2t;
}

// 80-bit extended real in memory, address in a register
EXTERNC void CCALL x87_fld_tword(CPU, uint32_t address)
{
    double value;

    value = x87_tword_to_double((const uint8_t *)REG2PTR(address));
    PUSH_REGS;
    ST0 = value;
}

EXTERNC void CCALL x87_fstp_tword(CPU, uint32_t address)
{
    x87_double_to_tword(ST0, (uint8_t *)REG2PTR(address));
    POP_REGS;
}

// st(num) = st0 - st(num)
EXTERNC void CCALL x87_fsubr_to_st(CPU, int num)
{
    ST(num) = ST0 - ST(num);
    CLEAR_X87_FLAGS;
}

// st(num) = st0 / st(num)
EXTERNC void CCALL x87_fdivr_to_st(CPU, int num)
{
    ST(num) = ST0 / ST(num);
    CLEAR_X87_FLAGS;
}

// 2^st0 - 1
EXTERNC void CCALL x87_f2xm1_void(CPU)
{
    ST0 = expm1(ST0 * M_LN2);
    CLEAR_X87_FLAGS;
}

// st1 = atan2(st1, st0), pop
EXTERNC void CCALL x87_fpatan_void(CPU)
{
    ST1 = atan2(ST1, ST0);
    POP_REGS;
}

// partial remainder, truncated quotient; the emulation always completes in one step (C2 = 0)
static void x87_fprem_common(CPU, int nearest)
{
    double x, y, quotient;
    uint32_t q;

    x = ST0;
    y = ST1;

    if (nearest)
    {
        ST0 = remainder(x, y);
        quotient = nearbyint(x / y);
    }
    else
    {
        ST0 = fmod(x, y);
        quotient = trunc(x / y);
    }

    q = (quotient == quotient && fabs(quotient) < 4294967296.0) ? (uint32_t)fabs(quotient) : 0;

    st_sw_cond = 0;
    if (q & 1) st_sw_cond |= X87_C1;
    if (q & 2) st_sw_cond |= X87_C3;
    if (q & 4) st_sw_cond |= X87_C0;
}

EXTERNC void CCALL x87_fprem_void(CPU)
{
    x87_fprem_common(cpu, 0);
}

EXTERNC void CCALL x87_fprem1_void(CPU)
{
    x87_fprem_common(cpu, 1);
}

// round to integer according to the rounding control field
EXTERNC void CCALL x87_frndint_void(CPU)
{
    switch ((st_cw >> X87_RC_SHIFT) & 3)
    {
    case 0: // Round to nearest (even)
        ST0 = nearbyint(ST0);
        break;
    case 1: // Round down (toward -infinity)
        ST0 = floor(ST0);
        break;
    case 2: // Round up (toward +infinity)
        ST0 = ceil(ST0);
        break;
    default: // Round toward zero (Truncate)
        ST0 = trunc(ST0);
        break;
    }
    CLEAR_X87_FLAGS;
}

// st0 = st0 * 2^trunc(st1)
EXTERNC void CCALL x87_fscale_void(CPU)
{
    double scale;
    int exponent;

    scale = trunc(ST1);
    exponent = (scale > 100000.0) ? 100000 : ((scale < -100000.0) ? -100000 : (int)scale);
    ST0 = ldexp(ST0, exponent);
    CLEAR_X87_FLAGS;
}

// st0 = sin(st0), then push cos
EXTERNC void CCALL x87_fsincos_void(CPU)
{
    double value;

    value = ST0;
    ST0 = sin(value);
    PUSH_REGS;
    ST0 = cos(value);
    CLEAR_X87_FLAGS;
}

// classify st0 in the condition bits (an empty register cannot be told from a value here)
EXTERNC void CCALL x87_fxam_void(CPU)
{
    double value;
    uint32_t cond;

    value = ST0;

    switch (fpclassify(value))
    {
    case FP_NAN:
        cond = X87_C0;
        break;
    case FP_INFINITE:
        cond = X87_C2 | X87_C0;
        break;
    case FP_ZERO:
        cond = X87_C3;
        break;
    case FP_SUBNORMAL:
        cond = X87_C3 | X87_C2;
        break;
    default:
        cond = X87_C2;
        break;
    }
    if (signbit(value)) cond |= X87_C1;

    st_sw_cond = cond;
}

// environment: 28 bytes (protected mode, 32-bit operand size)
#define X87_ENV_SIZE 28
#define X87_REG_AREA X87_ENV_SIZE

static void x87_store_env(CPU, uint8_t *p)
{
    memset(p, 0, X87_ENV_SIZE);
    x87_write_le32(p, st_cw & 0xffff);
    x87_write_le32(p + 4, (st_sw_cond | (st_top << 11)) & 0xffff);
    x87_write_le32(p + 8, 0xffff); // tag word: every register empty
}

static void x87_load_env(CPU, const uint8_t *p)
{
    uint32_t sw;

    st_cw = x87_read_le16(p);
    sw = x87_read_le16(p + 4);
    st_sw_cond = sw & X87_CX;
    st_top = (sw >> 11) & 7;
}

EXTERNC void CCALL x87_fnstenv_void(CPU, uint32_t address)
{
    x87_store_env(cpu, (uint8_t *)REG2PTR(address));
    st_cw |= 0x3f; // the instruction masks all exceptions
}

EXTERNC void CCALL x87_fldenv_void(CPU, uint32_t address)
{
    x87_load_env(cpu, (const uint8_t *)REG2PTR(address));
}

