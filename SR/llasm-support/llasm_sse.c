//part of static recompiler -- do not edit

// SSE, SSE2 and MMX instructions. Every instruction is one call: x86_sse_NAME(cpu, d, s, imm) with operand descriptors
// (SSE_XMM(n) 0-7, SSE_MM(n) 8-15, SSE_GPR(n) 16-23 in the x86 order eax ecx edx ebx esp ebp esi edi, which is the order of
// _cpu; anything else is a guest address). The arithmetic uses vector types of the compiler: WebAssembly SIMD with -msimd128,
// the host's vector unit in a native build. Integer operations take their width from the destination
// (64 bits for an MMX register, 128 for an XMM register or memory of a 128-bit form). MXCSR: the rounding mode is honored by the
// conversions that round; exceptions are always masked, denormals are kept. 80-bit x87 state and MMX do not alias here (the
// program clears with emms between them as it must on x86 anyway).

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "llasm_cpu.h"

#define SSE_XMM(n) (n)
#define SSE_MM(n) (8 + (n))
#define SSE_GPR(n) (16 + (n))

typedef float v4f __attribute__((vector_size(16)));
typedef double v2d __attribute__((vector_size(16)));
typedef int32_t v4i __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));
typedef int64_t v2l __attribute__((vector_size(16)));
typedef uint64_t v2q __attribute__((vector_size(16)));
typedef int16_t v8s __attribute__((vector_size(16)));
typedef uint16_t v8w __attribute__((vector_size(16)));
typedef int8_t v16c __attribute__((vector_size(16)));
typedef uint8_t v16b __attribute__((vector_size(16)));

typedef union {
    uint8_t b[16];
    v4f f; v2d d; v4i i; v4u u; v2l l; v2q q; v8s s; v8w w; v16c c; v16b ub;
} V;

static inline int is_xmm(uint32_t d) { return d < 8; }
static inline int is_mm(uint32_t d) { return d >= 8 && d < 16; }
static inline int is_gpr(uint32_t d) { return d >= 16 && d < 24; }

static inline uint32_t *gpr(CPU, uint32_t d) { return &cpu->_eax + (d - 16); }

// the bytes of an operand (a register or the guest memory)
static inline void *where(CPU, uint32_t d)
{
    if (d < 8) return &cpu->_xmm[d];
    if (d < 16) return &cpu->_mm[d - 8];
    if (d < 24) return gpr(cpu, d);
    return REG2PTR(d);
}

// the width of a full operand in bytes: 8 for MMX, else 16
static inline int width(uint32_t d) { return is_mm(d) ? 8 : 16; }

static inline V get(CPU, uint32_t d, int bytes)
{
    V v;

    memset(&v, 0, sizeof(v));
    memcpy(v.b, where(cpu, d), (size_t)bytes);
    return v;
}

static inline V get_full(CPU, uint32_t d) { return get(cpu, d, width(d)); }

static inline void put(CPU, uint32_t d, V v, int bytes) { memcpy(where(cpu, d), v.b, (size_t)bytes); }

static inline void put_full(CPU, uint32_t d, V v) { put(cpu, d, v, width(d)); }

// the source of an operation whose destination is d (an MMX operation reads 8 bytes, an XMM one 16)
static inline V src_for(CPU, uint32_t d, uint32_t s) { return get(cpu, s, width(d)); }

static inline uint32_t u32_of(CPU, uint32_t d)
{
    uint32_t v;

    memcpy(&v, where(cpu, d), 4);
    return v;
}

static inline uint64_t u64_of(CPU, uint32_t d)
{
    uint64_t v;

    memcpy(&v, where(cpu, d), 8);
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------
// moves

// 128-bit moves (movaps movups movapd movupd movdqa movdqu)
EXTERNC uint32_t CCALL x86_sse_mov128(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    memmove(where(cpu, d), where(cpu, s), 16);
    return 0;
}

// movss: from memory the upper lanes become zero, between registers they stay
EXTERNC uint32_t CCALL x86_sse_movss(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    if (is_xmm(d) && !is_xmm(s))
    {
        V v = get(cpu, s, 4);

        put(cpu, d, v, 16);
    }
    else memmove(where(cpu, d), where(cpu, s), 4);
    return 0;
}

// movsd (the SSE2 one) and movq to an XMM register: from memory or (movq) a register the upper half becomes zero
EXTERNC uint32_t CCALL x86_sse_movsd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    if (is_xmm(d) && !is_xmm(s))
    {
        V v = get(cpu, s, 8);

        put(cpu, d, v, 16);
    }
    else memmove(where(cpu, d), where(cpu, s), 8);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_movq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    if (is_xmm(d))
    {
        V v = get(cpu, s, 8);

        put(cpu, d, v, 16);   // (movq xmm, xmm/m64 zeroes the upper half too)
    }
    else memmove(where(cpu, d), where(cpu, s), 8);
    return 0;
}

// movlps/movlpd: the low half only
EXTERNC uint32_t CCALL x86_sse_movlo64(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    memmove(where(cpu, d), where(cpu, s), 8);
    return 0;
}

// movhps/movhpd: the high half of the register to or from memory
EXTERNC uint32_t CCALL x86_sse_movhi64(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    if (is_xmm(d)) memcpy(cpu->_xmm[d].u8 + 8, where(cpu, s), 8);
    else memcpy(where(cpu, d), cpu->_xmm[s].u8 + 8, 8);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_movhlps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    cpu->_xmm[d].u64[0] = cpu->_xmm[s].u64[1];
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_movlhps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    cpu->_xmm[d].u64[1] = cpu->_xmm[s].u64[0];
    return 0;
}

// movd: 32 bits; into an XMM or MMX register the rest becomes zero
EXTERNC uint32_t CCALL x86_sse_movd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    uint32_t v = u32_of(cpu, s);

    (void) imm;
    if (is_xmm(d) || is_mm(d))
    {
        V r;

        memset(&r, 0, sizeof(r));
        r.u[0] = v;
        put_full(cpu, d, r);
    }
    else memcpy(where(cpu, d), &v, 4);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_movmskps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    uint32_t r = 0;
    int i;

    (void) imm;
    for (i = 0; i < 4; i++) r |= (cpu->_xmm[s].u32[i] >> 31) << i;
    *gpr(cpu, d) = r;
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_movmskpd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    *gpr(cpu, d) = (uint32_t) (cpu->_xmm[s].u64[0] >> 63) | (uint32_t) ((cpu->_xmm[s].u64[1] >> 63) << 1);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_pmovmskb(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V v = get_full(cpu, s);
    uint32_t r = 0;
    int i;

    (void) imm;
    for (i = 0; i < width(s); i++) r |= (uint32_t) (v.b[i] >> 7) << i;
    *gpr(cpu, d) = r;
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_ldmxcsr(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) s; (void) imm;
    cpu->_mxcsr = u32_of(cpu, d);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_stmxcsr(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) s; (void) imm;
    memcpy(where(cpu, d), &cpu->_mxcsr, 4);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_nop(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) cpu; (void) d; (void) s; (void) imm;
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_fence(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) cpu; (void) d; (void) s; (void) imm;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// floating point arithmetic: packed (ps, pd) and scalar (ss, sd; the other lanes stay)

static inline float minf_x86(float a, float b) { return a < b ? a : b; }   // (x86: the second operand when unordered or equal)
static inline float maxf_x86(float a, float b) { return a > b ? a : b; }
static inline double mind_x86(double a, double b) { return a < b ? a : b; }
static inline double maxd_x86(double a, double b) { return a > b ? a : b; }

#define PS_OP(name, expr) \
EXTERNC uint32_t CCALL x86_sse_##name##ps(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r; \
    int i; \
    (void) imm; (void) i; (void) a; \
    expr; \
    put(cpu, d, r, 16); \
    return 0; \
} \
EXTERNC uint32_t CCALL x86_sse_##name##ss(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get(cpu, d, 16), b = get(cpu, s, 4), r; \
    int i; \
    (void) imm; (void) i; (void) a; \
    expr; \
    cpu->_xmm[d].f[0] = r.f[0]; \
    return 0; \
}

#define PD_OP(name, expr) \
EXTERNC uint32_t CCALL x86_sse_##name##pd(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r; \
    int i; \
    (void) imm; (void) i; (void) a; \
    expr; \
    put(cpu, d, r, 16); \
    return 0; \
} \
EXTERNC uint32_t CCALL x86_sse_##name##sd(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get(cpu, d, 16), b = get(cpu, s, 8), r; \
    int i; \
    (void) imm; (void) i; (void) a; \
    expr; \
    cpu->_xmm[d].d[0] = r.d[0]; \
    return 0; \
}

PS_OP(add, r.f = a.f + b.f)
PS_OP(sub, r.f = a.f - b.f)
PS_OP(mul, r.f = a.f * b.f)
PS_OP(div, r.f = a.f / b.f)
PS_OP(min, for (i = 0; i < 4; i++) r.f[i] = minf_x86(a.f[i], b.f[i]))
PS_OP(max, for (i = 0; i < 4; i++) r.f[i] = maxf_x86(a.f[i], b.f[i]))
PS_OP(sqrt, for (i = 0; i < 4; i++) r.f[i] = sqrtf(b.f[i]))
// (rcp and rsqrt are approximations on x86, about 12 bits; the exact values are within them)
PS_OP(rcp, for (i = 0; i < 4; i++) r.f[i] = 1.0f / b.f[i])
PS_OP(rsqrt, for (i = 0; i < 4; i++) r.f[i] = 1.0f / sqrtf(b.f[i]))

PD_OP(add, r.d = a.d + b.d)
PD_OP(sub, r.d = a.d - b.d)
PD_OP(mul, r.d = a.d * b.d)
PD_OP(div, r.d = a.d / b.d)
PD_OP(min, for (i = 0; i < 2; i++) r.d[i] = mind_x86(a.d[i], b.d[i]))
PD_OP(max, for (i = 0; i < 2; i++) r.d[i] = maxd_x86(a.d[i], b.d[i]))
PD_OP(sqrt, for (i = 0; i < 2; i++) r.d[i] = sqrt(b.d[i]))

// bitwise operations of all kinds (andps andpd pand...): the width of the destination
#define BIT_OP(name, expr) \
EXTERNC uint32_t CCALL x86_sse_##name(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get_full(cpu, d), b = src_for(cpu, d, s), r; \
    (void) imm; \
    r.q = expr; \
    put_full(cpu, d, r); \
    return 0; \
}

BIT_OP(and, a.q & b.q)
BIT_OP(andn, ~a.q & b.q)
BIT_OP(or, a.q | b.q)
BIT_OP(xor, a.q ^ b.q)

// compares: imm 0 eq, 1 lt, 2 le, 3 unord, 4 neq, 5 nlt, 6 nle, 7 ord
static inline int cmp_pred(double a, double b, uint32_t imm)
{
    int unordered = isnan(a) || isnan(b);

    switch (imm & 7)
    {
        case 0: return !unordered && a == b;
        case 1: return !unordered && a < b;
        case 2: return !unordered && a <= b;
        case 3: return unordered;
        case 4: return unordered || a != b;
        case 5: return unordered || !(a < b);
        case 6: return unordered || !(a <= b);
        default: return !unordered;
    }
}

EXTERNC uint32_t CCALL x86_sse_cmpps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;
    int i;

    for (i = 0; i < 4; i++) r.u[i] = cmp_pred(a.f[i], b.f[i], imm) ? 0xffffffffu : 0;
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cmpss(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    float b;

    memcpy(&b, where(cpu, s), 4);
    cpu->_xmm[d].u32[0] = cmp_pred(cpu->_xmm[d].f[0], b, imm) ? 0xffffffffu : 0;
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cmppd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;
    int i;

    for (i = 0; i < 2; i++) r.q[i] = cmp_pred(a.d[i], b.d[i], imm) ? ~(uint64_t) 0 : 0;
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cmpsd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    double b;

    memcpy(&b, where(cpu, s), 8);
    cpu->_xmm[d].u64[0] = cmp_pred(cpu->_xmm[d].d[0], b, imm) ? ~(uint64_t) 0 : 0;
    return 0;
}

// comiss/ucomiss/comisd/ucomisd: ZF PF CF as an unsigned compare (all three when unordered), OF SF AF cleared
static uint32_t compare_flags(CPU, double a, double b)
{
    uint32_t f;

    if (isnan(a) || isnan(b)) f = ZF | PF | CF;
    else if (a < b) f = CF;
    else if (a == b) f = ZF;
    else f = 0;
    eflags = (eflags & ~(uint32_t) (ZF | PF | CF | OF | SF | AF)) | f;
    return f;
}

EXTERNC uint32_t CCALL x86_sse_comiss(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    float b;

    (void) imm;
    memcpy(&b, where(cpu, s), 4);
    return compare_flags(cpu, cpu->_xmm[d].f[0], b);
}

EXTERNC uint32_t CCALL x86_sse_comisd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    double b;

    (void) imm;
    memcpy(&b, where(cpu, s), 8);
    return compare_flags(cpu, cpu->_xmm[d].d[0], b);
}

// ---------------------------------------------------------------------------------------------------------------------
// shuffles and unpacks

EXTERNC uint32_t CCALL x86_sse_shufps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;

    r.u[0] = a.u[imm & 3];
    r.u[1] = a.u[(imm >> 2) & 3];
    r.u[2] = b.u[(imm >> 4) & 3];
    r.u[3] = b.u[(imm >> 6) & 3];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_shufpd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;

    r.q[0] = a.q[imm & 1];
    r.q[1] = b.q[(imm >> 1) & 1];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_pshufd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;
    int i;

    for (i = 0; i < 4; i++) r.u[i] = b.u[(imm >> (2 * i)) & 3];
    put(cpu, d, r, 16);
    return 0;
}

// pshufw (MMX) / pshuflw / pshufhw
EXTERNC uint32_t CCALL x86_sse_pshufw(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 8), r;
    int i;

    memset(&r, 0, sizeof(r));
    for (i = 0; i < 4; i++) r.w[i] = b.w[(imm >> (2 * i)) & 3];
    put(cpu, d, r, 8);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_pshuflw(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r = b;
    int i;

    for (i = 0; i < 4; i++) r.w[i] = b.w[(imm >> (2 * i)) & 3];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_pshufhw(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r = b;
    int i;

    for (i = 0; i < 4; i++) r.w[4 + i] = b.w[4 + ((imm >> (2 * i)) & 3)];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_unpcklps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;

    (void) imm;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_unpckhps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;

    (void) imm;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    put(cpu, d, r, 16);
    return 0;
}

// unpcklpd = punpcklqdq, unpckhpd = punpckhqdq
EXTERNC uint32_t CCALL x86_sse_unpcklpd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;

    (void) imm;
    r.q[0] = a.q[0]; r.q[1] = b.q[0];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_unpckhpd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), b = get(cpu, s, 16), r;

    (void) imm;
    r.q[0] = a.q[1]; r.q[1] = b.q[1];
    put(cpu, d, r, 16);
    return 0;
}

// punpckl/h bw wd dq: interleave elements of size `size` from the low or the high half of the destination's width
static uint32_t punpck(CPU, uint32_t d, uint32_t s, int size, int high)
{
    int bytes = width(d), half = bytes / 2, n = half / size, i;
    V a = get_full(cpu, d), b = src_for(cpu, d, s), r;

    memset(&r, 0, sizeof(r));
    for (i = 0; i < n; i++)
    {
        memcpy(r.b + (2 * i) * size, a.b + (high ? half : 0) + i * size, (size_t) size);
        memcpy(r.b + (2 * i + 1) * size, b.b + (high ? half : 0) + i * size, (size_t) size);
    }
    put_full(cpu, d, r);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_punpcklbw(CPU, uint32_t d, uint32_t s, uint32_t imm) { (void) imm; return punpck(cpu, d, s, 1, 0); }
EXTERNC uint32_t CCALL x86_sse_punpcklwd(CPU, uint32_t d, uint32_t s, uint32_t imm) { (void) imm; return punpck(cpu, d, s, 2, 0); }
EXTERNC uint32_t CCALL x86_sse_punpckldq(CPU, uint32_t d, uint32_t s, uint32_t imm) { (void) imm; return punpck(cpu, d, s, 4, 0); }
EXTERNC uint32_t CCALL x86_sse_punpckhbw(CPU, uint32_t d, uint32_t s, uint32_t imm) { (void) imm; return punpck(cpu, d, s, 1, 1); }
EXTERNC uint32_t CCALL x86_sse_punpckhwd(CPU, uint32_t d, uint32_t s, uint32_t imm) { (void) imm; return punpck(cpu, d, s, 2, 1); }
EXTERNC uint32_t CCALL x86_sse_punpckhdq(CPU, uint32_t d, uint32_t s, uint32_t imm) { (void) imm; return punpck(cpu, d, s, 4, 1); }

// ---------------------------------------------------------------------------------------------------------------------
// conversions

// rounding of the conversions that round, by MXCSR.RC (0 nearest even, 1 down, 2 up, 3 toward zero)
static inline double round_mxcsr(CPU, double v)
{
    switch ((cpu->_mxcsr >> 13) & 3)
    {
        case 0: return nearbyint(v);   // (the default environment rounds to nearest even)
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}

// an int32 (exact in a double) to float, rounded by MXCSR.RC as the conversions of x86 do
static inline float i32_to_f32(CPU, int32_t v)
{
    double exact = (double) v;
    float f = (float) exact;   // (nearest even)

    switch ((cpu->_mxcsr >> 13) & 3)
    {
        case 1: if ((double) f > exact) f = nextafterf(f, -INFINITY); break;
        case 2: if ((double) f < exact) f = nextafterf(f, INFINITY); break;
        case 3: if (fabs((double) f) > fabs(exact)) f = nextafterf(f, 0.0f); break;
        default: break;
    }
    return f;
}

// a double to int32 as x86 does: the "integer indefinite" 0x80000000 for NaN and values out of range
static inline int32_t to_i32(double v)
{
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return (int32_t) 0x80000000u;
    return (int32_t) v;
}

EXTERNC uint32_t CCALL x86_sse_cvtpi2ps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 8);

    (void) imm;
    cpu->_xmm[d].f[0] = i32_to_f32(cpu, b.i[0]);
    cpu->_xmm[d].f[1] = i32_to_f32(cpu, b.i[1]);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtps2pi(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 8), r;

    (void) imm;
    memset(&r, 0, sizeof(r));
    r.i[0] = to_i32(round_mxcsr(cpu, b.f[0]));
    r.i[1] = to_i32(round_mxcsr(cpu, b.f[1]));
    put(cpu, d, r, 8);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvttps2pi(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 8), r;

    (void) imm;
    memset(&r, 0, sizeof(r));
    r.i[0] = to_i32(trunc(b.f[0]));
    r.i[1] = to_i32(trunc(b.f[1]));
    put(cpu, d, r, 8);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtdq2ps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;
    int i;

    (void) imm;
    for (i = 0; i < 4; i++) r.f[i] = i32_to_f32(cpu, b.i[i]);
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtps2dq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;
    int i;

    (void) imm;
    for (i = 0; i < 4; i++) r.i[i] = to_i32(round_mxcsr(cpu, b.f[i]));
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvttps2dq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;
    int i;

    (void) imm;
    for (i = 0; i < 4; i++) r.i[i] = to_i32(trunc(b.f[i]));
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtdq2pd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 8), r;

    (void) imm;
    r.d[0] = (double) b.i[0];
    r.d[1] = (double) b.i[1];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtpd2dq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;

    (void) imm;
    memset(&r, 0, sizeof(r));
    r.i[0] = to_i32(round_mxcsr(cpu, b.d[0]));
    r.i[1] = to_i32(round_mxcsr(cpu, b.d[1]));
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvttpd2dq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;

    (void) imm;
    memset(&r, 0, sizeof(r));
    r.i[0] = to_i32(trunc(b.d[0]));
    r.i[1] = to_i32(trunc(b.d[1]));
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtps2pd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 8), r;

    (void) imm;
    r.d[0] = b.f[0];
    r.d[1] = b.f[1];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtpd2ps(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get(cpu, s, 16), r;

    (void) imm;
    memset(&r, 0, sizeof(r));
    r.f[0] = (float) b.d[0];
    r.f[1] = (float) b.d[1];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtss2sd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    float b;

    (void) imm;
    memcpy(&b, where(cpu, s), 4);
    cpu->_xmm[d].d[0] = b;
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtsd2ss(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    double b;

    (void) imm;
    memcpy(&b, where(cpu, s), 8);
    cpu->_xmm[d].f[0] = (float) b;
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtsi2ss(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    cpu->_xmm[d].f[0] = i32_to_f32(cpu, (int32_t) u32_of(cpu, s));
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtsi2sd(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    (void) imm;
    cpu->_xmm[d].d[0] = (double) (int32_t) u32_of(cpu, s);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtss2si(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    float b;

    (void) imm;
    memcpy(&b, where(cpu, s), 4);
    *gpr(cpu, d) = (uint32_t) to_i32(round_mxcsr(cpu, b));
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvttss2si(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    float b;

    (void) imm;
    memcpy(&b, where(cpu, s), 4);
    *gpr(cpu, d) = (uint32_t) to_i32(trunc(b));
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvtsd2si(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    double b;

    (void) imm;
    memcpy(&b, where(cpu, s), 8);
    *gpr(cpu, d) = (uint32_t) to_i32(round_mxcsr(cpu, b));
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_cvttsd2si(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    double b;

    (void) imm;
    memcpy(&b, where(cpu, s), 8);
    *gpr(cpu, d) = (uint32_t) to_i32(trunc(b));
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// integer SIMD (MMX on MMX registers, SSE2 on XMM registers)

#define INT_OP(name, expr) \
EXTERNC uint32_t CCALL x86_sse_##name(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get_full(cpu, d), b = src_for(cpu, d, s), r; \
    int i; \
    (void) imm; (void) i; (void) a; \
    expr; \
    put_full(cpu, d, r); \
    return 0; \
}

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t) v; }
static inline uint16_t satu16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t) v; }
static inline int8_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t) v; }
static inline uint8_t satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t) v; }

INT_OP(paddb, r.ub = a.ub + b.ub)
INT_OP(paddw, r.w = a.w + b.w)
INT_OP(paddd, r.u = a.u + b.u)
INT_OP(paddq, r.q = a.q + b.q)
INT_OP(psubb, r.ub = a.ub - b.ub)
INT_OP(psubw, r.w = a.w - b.w)
INT_OP(psubd, r.u = a.u - b.u)
INT_OP(psubq, r.q = a.q - b.q)
INT_OP(paddsb, for (i = 0; i < 16; i++) r.c[i] = sat8(a.c[i] + b.c[i]))
INT_OP(paddsw, for (i = 0; i < 8; i++) r.s[i] = sat16(a.s[i] + b.s[i]))
INT_OP(paddusb, for (i = 0; i < 16; i++) r.ub[i] = satu8(a.ub[i] + b.ub[i]))
INT_OP(paddusw, for (i = 0; i < 8; i++) r.w[i] = satu16(a.w[i] + b.w[i]))
INT_OP(psubsb, for (i = 0; i < 16; i++) r.c[i] = sat8(a.c[i] - b.c[i]))
INT_OP(psubsw, for (i = 0; i < 8; i++) r.s[i] = sat16(a.s[i] - b.s[i]))
INT_OP(psubusb, for (i = 0; i < 16; i++) r.ub[i] = satu8(a.ub[i] - b.ub[i]))
INT_OP(psubusw, for (i = 0; i < 8; i++) r.w[i] = satu16(a.w[i] - b.w[i]))
INT_OP(pmullw, for (i = 0; i < 8; i++) r.s[i] = (int16_t) (a.s[i] * b.s[i]))
INT_OP(pmulhw, for (i = 0; i < 8; i++) r.s[i] = (int16_t) ((a.s[i] * b.s[i]) >> 16))
INT_OP(pmulhuw, for (i = 0; i < 8; i++) r.w[i] = (uint16_t) (((uint32_t) a.w[i] * b.w[i]) >> 16))
INT_OP(pmuludq, for (i = 0; i < 2; i++) r.q[i] = (uint64_t) a.u[2 * i] * b.u[2 * i])
INT_OP(pmaddwd, for (i = 0; i < 4; i++) r.u[i] = (uint32_t) ((int64_t) a.s[2 * i] * b.s[2 * i] + (int64_t) a.s[2 * i + 1] * b.s[2 * i + 1]))
INT_OP(pavgb, for (i = 0; i < 16; i++) r.ub[i] = (uint8_t) ((a.ub[i] + b.ub[i] + 1) >> 1))
INT_OP(pavgw, for (i = 0; i < 8; i++) r.w[i] = (uint16_t) ((a.w[i] + b.w[i] + 1) >> 1))
INT_OP(pmaxsw, for (i = 0; i < 8; i++) r.s[i] = a.s[i] > b.s[i] ? a.s[i] : b.s[i])
INT_OP(pminsw, for (i = 0; i < 8; i++) r.s[i] = a.s[i] < b.s[i] ? a.s[i] : b.s[i])
INT_OP(pmaxub, for (i = 0; i < 16; i++) r.ub[i] = a.ub[i] > b.ub[i] ? a.ub[i] : b.ub[i])
INT_OP(pminub, for (i = 0; i < 16; i++) r.ub[i] = a.ub[i] < b.ub[i] ? a.ub[i] : b.ub[i])
INT_OP(pcmpeqb, for (i = 0; i < 16; i++) r.ub[i] = a.ub[i] == b.ub[i] ? 0xff : 0)
INT_OP(pcmpeqw, for (i = 0; i < 8; i++) r.w[i] = a.w[i] == b.w[i] ? 0xffff : 0)
INT_OP(pcmpeqd, for (i = 0; i < 4; i++) r.u[i] = a.u[i] == b.u[i] ? 0xffffffffu : 0)
INT_OP(pcmpgtb, for (i = 0; i < 16; i++) r.ub[i] = a.c[i] > b.c[i] ? 0xff : 0)
INT_OP(pcmpgtw, for (i = 0; i < 8; i++) r.w[i] = a.s[i] > b.s[i] ? 0xffff : 0)
INT_OP(pcmpgtd, for (i = 0; i < 4; i++) r.u[i] = a.i[i] > b.i[i] ? 0xffffffffu : 0)

EXTERNC uint32_t CCALL x86_sse_psadbw(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get_full(cpu, d), b = src_for(cpu, d, s), r;
    int half, i;

    (void) imm;
    memset(&r, 0, sizeof(r));
    for (half = 0; half < width(d) / 8; half++)
    {
        uint32_t sum = 0;

        for (i = 0; i < 8; i++) sum += (uint32_t) abs((int) a.ub[half * 8 + i] - (int) b.ub[half * 8 + i]);
        r.q[half] = sum;
    }
    put_full(cpu, d, r);
    return 0;
}

// packs: the destination's elements, then the source's, narrowed with saturation
#define PACK_OP(name, from_t, from_f, to_f, n_per_half, sat) \
EXTERNC uint32_t CCALL x86_sse_##name(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get_full(cpu, d), b = src_for(cpu, d, s), r; \
    int n = (width(d) / (int) sizeof(from_t)), i; \
    (void) imm; \
    memset(&r, 0, sizeof(r)); \
    for (i = 0; i < n; i++) { r.to_f[i] = sat(a.from_f[i]); r.to_f[n + i] = sat(b.from_f[i]); } \
    put_full(cpu, d, r); \
    return 0; \
}

PACK_OP(packsswb, int16_t, s, c, 8, sat8)
PACK_OP(packuswb, int16_t, s, ub, 8, satu8)
PACK_OP(packssdw, int32_t, i, s, 4, sat16)

// shifts: by an immediate (s == 0xffffffff, the count in imm) or by the low 64 bits of an operand
static inline uint64_t shift_count(CPU, uint32_t s, uint32_t imm)
{
    return s == 0xffffffffu ? imm : u64_of(cpu, s);
}

#define SHIFT_OP(name, lanes, field, bits, expr) \
EXTERNC uint32_t CCALL x86_sse_##name(CPU, uint32_t d, uint32_t s, uint32_t imm) \
{ \
    V a = get_full(cpu, d), r; \
    uint64_t c = shift_count(cpu, s, imm); \
    int i; \
    for (i = 0; i < lanes; i++) r.field[i] = expr; \
    put_full(cpu, d, r); \
    return 0; \
}

SHIFT_OP(psllw, 8, w, 16, c > 15 ? 0 : (uint16_t) (a.w[i] << c))
SHIFT_OP(pslld, 4, u, 32, c > 31 ? 0 : a.u[i] << c)
SHIFT_OP(psllq, 2, q, 64, c > 63 ? 0 : a.q[i] << c)
SHIFT_OP(psrlw, 8, w, 16, c > 15 ? 0 : (uint16_t) (a.w[i] >> c))
SHIFT_OP(psrld, 4, u, 32, c > 31 ? 0 : a.u[i] >> c)
SHIFT_OP(psrlq, 2, q, 64, c > 63 ? 0 : a.q[i] >> c)
SHIFT_OP(psraw, 8, s, 16, (int16_t) (a.s[i] >> (c > 15 ? 15 : c)))
SHIFT_OP(psrad, 4, i, 32, a.i[i] >> (c > 31 ? 31 : c))

// byte shifts of a whole XMM register (imm bytes)
EXTERNC uint32_t CCALL x86_sse_pslldq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), r;
    int i;

    (void) s;
    memset(&r, 0, sizeof(r));
    for (i = 0; i < 16; i++) if ((uint32_t) i >= imm && imm < 16) r.b[i] = a.b[i - imm];
    put(cpu, d, r, 16);
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_psrldq(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get(cpu, d, 16), r;
    int i;

    (void) s;
    memset(&r, 0, sizeof(r));
    for (i = 0; i < 16; i++) if (i + imm < 16) r.b[i] = a.b[i + imm];
    put(cpu, d, r, 16);
    return 0;
}

// pextrw r32, xmm/mm, imm / pinsrw xmm/mm, r32/m16, imm
EXTERNC uint32_t CCALL x86_sse_pextrw(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V b = get_full(cpu, s);

    *gpr(cpu, d) = b.w[imm & (is_mm(s) ? 3 : 7)];
    return 0;
}

EXTERNC uint32_t CCALL x86_sse_pinsrw(CPU, uint32_t d, uint32_t s, uint32_t imm)
{
    V a = get_full(cpu, d);
    uint16_t v;

    memcpy(&v, where(cpu, s), 2);
    a.w[imm & (is_mm(d) ? 3 : 7)] = v;
    put_full(cpu, d, a);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// cpuid: the features the program is told about; x86_cpu_features is set by the runtime at the start (0: none, as before SSE
// was translated; else the bits of leaf 1 EDX: MMX 23, FXSR 24, SSE 25, SSE2 26, with CMOV 15, CX8 8, TSC 4, FPU 0)

EXTERNCVAR uint32_t x86_cpu_features;
uint32_t x86_cpu_features;

EXTERNC uint32_t CCALL x86_cpuid(CPU)
{
    uint32_t leaf = eax;

    if (x86_cpu_features == 0)
    {
        eax = ebx = ecx = edx = 0;
        return 0;
    }
    if (leaf == 0)
    {
        eax = 1;
        ebx = 0x756e6547;   // "GenuineIntel"
        edx = 0x49656e69;
        ecx = 0x6c65746e;
    }
    else if (leaf == 1)
    {
        eax = 0x00000f29;   // family 15 (a Pentium 4 era processor: SSE2, nothing newer)
        ebx = 0x00010800;   // one logical processor (the program does not look at the count for its threads)
        ecx = 0;
        edx = x86_cpu_features;
    }
    else if (leaf == 0x80000000u)
    {
        eax = 0x80000000u;
        ebx = ecx = edx = 0;
    }
    else
    {
        eax = ebx = ecx = edx = 0;
    }
    return 0;
}
