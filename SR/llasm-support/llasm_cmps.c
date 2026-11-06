//part of static recompiler -- do not edit

/**
 *
 *  Copyright (C) 2021-2026 Roman Pauer
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

#include <string.h>
#include "llasm_cpu.h"

EXTERNC uint32_t CCALL x86_repe_cmpsb(CPU)
{
    int32_t dir;
    uint32_t srcptr1, srcptr2, counter, srcvalue1, srcvalue2;

    dir = (eflags & DF)?-1:1;

    counter = ecx;
    srcptr1 = esi;
    srcptr2 = edi;

    do {
        srcvalue1 = *((uint8_t *)REG2PTR(srcptr1));
        srcvalue2 = *((uint8_t *)REG2PTR(srcptr2));
        srcptr1 += dir;
        srcptr2 += dir;

        counter--;

        if (srcvalue1 != srcvalue2) break;
    } while (counter != 0);

    ecx = counter;
    esi = srcptr1;
    edi = srcptr2;

    return srcvalue1 | (srcvalue2 << 8);
}

EXTERNC uint32_t CCALL x86_repne_cmpsb(CPU)
{
    int32_t dir;
    uint32_t srcptr1, srcptr2, counter, srcvalue1, srcvalue2;

    dir = (eflags & DF)?-1:1;

    counter = ecx;
    srcptr1 = esi;
    srcptr2 = edi;

    do {
        srcvalue1 = *((uint8_t *)REG2PTR(srcptr1));
        srcvalue2 = *((uint8_t *)REG2PTR(srcptr2));
        srcptr1 += dir;
        srcptr2 += dir;

        counter--;

        if (srcvalue1 == srcvalue2) break;
    } while (counter != 0);

    ecx = counter;
    esi = srcptr1;
    edi = srcptr2;

    return srcvalue1 | (srcvalue2 << 8);
}



// ---------------------------------------------------------------------
// 16/32-bit string compares and the atomic compare/exchange family (32-bit Windows C runtime)

// the value compared last from the second operand of cmpsw/cmpsd, see the first value returned by the same call
EXTERNC uint32_t CCALL x86_cmps_second(CPU)
{
    return (uint32_t)cpu->_st_result;
}

#define CMPS_LOOP(TYPE, STEP, STOP_CONDITION) \
    int32_t dir; \
    uint32_t srcptr1, srcptr2, counter; \
    TYPE srcvalue1, srcvalue2; \
    \
    dir = (eflags & DF)?-(STEP):(STEP); \
    \
    counter = ecx; \
    srcptr1 = esi; \
    srcptr2 = edi; \
    \
    do { \
        srcvalue1 = *((TYPE *)REG2PTR(srcptr1)); \
        srcvalue2 = *((TYPE *)REG2PTR(srcptr2)); \
        srcptr1 += dir; \
        srcptr2 += dir; \
        \
        counter--; \
        \
        if (STOP_CONDITION) break; \
    } while (counter != 0); \
    \
    ecx = counter; \
    esi = srcptr1; \
    edi = srcptr2; \
    \
    cpu->_st_result = srcvalue2; \
    return srcvalue1;

EXTERNC uint32_t CCALL x86_repe_cmpsw(CPU)
{
    CMPS_LOOP(uint16_t, 2, srcvalue1 != srcvalue2)
}

EXTERNC uint32_t CCALL x86_repne_cmpsw(CPU)
{
    CMPS_LOOP(uint16_t, 2, srcvalue1 == srcvalue2)
}

EXTERNC uint32_t CCALL x86_repe_cmpsd(CPU)
{
    CMPS_LOOP(uint32_t, 4, srcvalue1 != srcvalue2)
}

EXTERNC uint32_t CCALL x86_repne_cmpsd(CPU)
{
    CMPS_LOOP(uint32_t, 4, srcvalue1 == srcvalue2)
}

// WebAssembly atomics need natural alignment, x86 does not: a global lock stands in for an unaligned operand
// (one lock for every unaligned atomic operand: the runtimes take it too, for their interlocked functions on the same variables)
volatile char x86_unaligned_lock;

// lock cmpxchg [address], desired with eax = expected: returns the previous value of the memory
EXTERNC uint32_t CCALL x86_cmpxchg32(CPU, uint32_t address, uint32_t expected, uint32_t desired)
{
    uint32_t *ptr;

    ptr = (uint32_t *)REG2PTR(address);
    if (((uintptr_t)ptr & 3) != 0)
    {
        uint32_t current;

        while (__atomic_test_and_set(&x86_unaligned_lock, __ATOMIC_ACQUIRE)) { }
        memcpy(&current, ptr, 4);
        if (current == expected) memcpy(ptr, &desired, 4);
        __atomic_clear(&x86_unaligned_lock, __ATOMIC_RELEASE);
        return current;
    }
    __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

    return expected; // holds the previous value whether or not the exchange happened
}

// lock xadd [address], value: returns the previous value of the memory
EXTERNC uint32_t CCALL x86_xadd32(CPU, uint32_t address, uint32_t value)
{
    uint32_t *ptr = (uint32_t *)REG2PTR(address);

    if (((uintptr_t)ptr & 3) != 0)
    {
        uint32_t current, sum;

        while (__atomic_test_and_set(&x86_unaligned_lock, __ATOMIC_ACQUIRE)) { }
        memcpy(&current, ptr, 4);
        sum = current + value;
        memcpy(ptr, &sum, 4);
        __atomic_clear(&x86_unaligned_lock, __ATOMIC_RELEASE);
        return current;
    }
    return __atomic_fetch_add(ptr, value, __ATOMIC_SEQ_CST);
}

// Atomic read-modify-write for the memory operand of xchg (implicitly locked) and of lock-prefixed add/sub/and/or/xor/inc/dec/not: the previous
// value (zero extended) is returned, the caller computes the flags from it. op: 0 exchange, 1 add, 2 and, 3 or, 4 xor; size 1, 2 or 4 bytes.
// An operand that is not naturally aligned goes through the same global lock as the other helpers.
EXTERNC uint32_t CCALL x86_lock_rmw(CPU, uint32_t address, uint32_t op, uint32_t size, uint32_t value)
{
    void *ptr = REG2PTR(address);

    if (((uintptr_t)ptr & (size - 1)) != 0)
    {
        uint32_t current = 0, result;

        while (__atomic_test_and_set(&x86_unaligned_lock, __ATOMIC_ACQUIRE)) { }
        memcpy(&current, ptr, size);
        switch (op)
        {
            case 0: result = value; break;
            case 1: result = current + value; break;
            case 2: result = current & value; break;
            case 3: result = current | value; break;
            default: result = current ^ value; break;
        }
        memcpy(ptr, &result, size);
        __atomic_clear(&x86_unaligned_lock, __ATOMIC_RELEASE);
        return current;
    }

#define LOCK_RMW_SIZE(TYPE) \
    switch (op) \
    { \
        case 0: return (TYPE)__atomic_exchange_n((TYPE *)ptr, (TYPE)value, __ATOMIC_SEQ_CST); \
        case 1: return (TYPE)__atomic_fetch_add((TYPE *)ptr, (TYPE)value, __ATOMIC_SEQ_CST); \
        case 2: return (TYPE)__atomic_fetch_and((TYPE *)ptr, (TYPE)value, __ATOMIC_SEQ_CST); \
        case 3: return (TYPE)__atomic_fetch_or((TYPE *)ptr, (TYPE)value, __ATOMIC_SEQ_CST); \
        default: return (TYPE)__atomic_fetch_xor((TYPE *)ptr, (TYPE)value, __ATOMIC_SEQ_CST); \
    }

    if (size == 4) { LOCK_RMW_SIZE(uint32_t) }
    if (size == 2) { LOCK_RMW_SIZE(uint16_t) }
    { LOCK_RMW_SIZE(uint8_t) }
#undef LOCK_RMW_SIZE
}

// lock cmpxchg8b [address]: compares edx:eax, on success stores ecx:ebx, else loads the memory into edx:eax; returns ZF
EXTERNC uint32_t CCALL x86_cmpxchg8b(CPU, uint32_t address)
{
    uint64_t *ptr, expected, desired;

    ptr = (uint64_t *)REG2PTR(address);
    expected = ((uint64_t)edx << 32) | eax;
    desired = ((uint64_t)ecx << 32) | ebx;

    if (((uintptr_t)ptr & 7) != 0)
    {
        uint32_t *words = (uint32_t *)ptr;
        uint64_t current;

        while (__atomic_test_and_set(&x86_unaligned_lock, __ATOMIC_ACQUIRE)) { }
        current = ((uint64_t)words[1] << 32) | words[0];
        if (current == expected)
        {
            words[0] = (uint32_t)desired;
            words[1] = (uint32_t)(desired >> 32);
            __atomic_clear(&x86_unaligned_lock, __ATOMIC_RELEASE);
            return 1;
        }
        __atomic_clear(&x86_unaligned_lock, __ATOMIC_RELEASE);
        eax = (uint32_t)current;
        edx = (uint32_t)(current >> 32);
        return 0;
    }

    if (__atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        return 1;
    }

    eax = (uint32_t)expected;
    edx = (uint32_t)(expected >> 32);
    return 0;
}
