//part of static recompiler -- do not edit

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

#include <time.h>
#include "llasm_cpu.h"


#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t CCALL X86_InPortProcedure(
    const uint16_t PortNum,
    const uint32_t PortSize
);

extern void CCALL X86_OutPortProcedure(
    const uint16_t PortNum,
    const uint32_t PortSize,
    const uint32_t _eax
);

#ifdef __cplusplus
}
#endif


EXTERNC void CCALL x86_in_al_imm(CPU, uint32_t portno)
{
    eax = (eax & 0xffffff00) | (X86_InPortProcedure(portno & 0xffff, 1) & 0xff);
}

EXTERNC void CCALL x86_in_ax_imm(CPU, uint32_t portno)
{
    eax = (eax & 0xffff0000) | (X86_InPortProcedure(portno & 0xffff, 2) & 0xffff);
}

EXTERNC void CCALL x86_in_eax_imm(CPU, uint32_t portno)
{
    eax = X86_InPortProcedure(portno & 0xffff, 4);
}

EXTERNC void CCALL x86_in_al_dx(CPU)
{
    eax = (eax & 0xffffff00) | (X86_InPortProcedure(edx & 0xffff, 1) & 0xff);
}

EXTERNC void CCALL x86_in_ax_dx(CPU)
{
    eax = (eax & 0xffff0000) | (X86_InPortProcedure(edx & 0xffff, 2) & 0xffff);
}

EXTERNC void CCALL x86_in_eax_dx(CPU)
{
    eax = X86_InPortProcedure(edx & 0xffff, 4);
}

EXTERNC void CCALL x86_out_imm_al(CPU, uint32_t portno)
{
    X86_OutPortProcedure(portno & 0xffff, 1, eax);
}

EXTERNC void CCALL x86_out_imm_ax(CPU, uint32_t portno)
{
    X86_OutPortProcedure(portno & 0xffff, 2, eax);
}

EXTERNC void CCALL x86_out_imm_eax(CPU, uint32_t portno)
{
    X86_OutPortProcedure(portno & 0xffff, 4, eax);
}

EXTERNC void CCALL x86_out_dx_al(CPU)
{
    X86_OutPortProcedure(edx & 0xffff, 1, eax);
}

EXTERNC void CCALL x86_out_dx_ax(CPU)
{
    X86_OutPortProcedure(edx & 0xffff, 2, eax);
}

EXTERNC void CCALL x86_out_dx_eax(CPU)
{
    X86_OutPortProcedure(edx & 0xffff, 4, eax);
}

extern uint64_t SRW_NowNs(void) __attribute__((weak));

/* the time stamp counter runs at a nominal 1 GHz, derived from the monotonic clock */
EXTERNC void CCALL x86_rdtsc(CPU)
{
    uint64_t ticks;

    if (SRW_NowNs != NULL)
    {
        ticks = SRW_NowNs();   /* the clock of the port that is the same for all threads */
    }
    else
    {
        struct timespec now;

        clock_gettime(CLOCK_MONOTONIC, &now);
        ticks = (uint64_t) now.tv_sec * 1000000000ull + (uint64_t) now.tv_nsec;
    }
    eax = (uint32_t) ticks;
    edx = (uint32_t) (ticks >> 32);
}
