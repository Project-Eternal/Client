/**
 *
 *  Copyright (C) 2016-2025 Roman Pauer
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

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include "SR_defs.h"
#include "SR_vars.h"
#include "udis86_dep.h"

#undef DISPLAY_DISASSEMBLY

typedef struct _Entry_region_ {
    unsigned int Entry;
    uint_fast32_t begin_ofs;
    int isregion;
} Entry_region;

output_data *SR_disassemble_offset_init_output(unsigned int SecNum, uint_fast32_t offset, unsigned int length)
{
    output_data *output;
    unsigned int index;

    if (offset + length == section[SecNum].size)
    {
        output = section_output_list_FindEntryEqual(SecNum, offset + length);
        if (output != NULL)
        {
            if (NULL == section_alias_list_FindEntryEqual(SecNum, offset + length))
            {
                output->has_label = 0;
                output->align = 0;
            }
        }
    }

    for (index = length - 1; index != 0; index--)
    {
        section_output_list_Delete(SecNum, offset + index);
        section_bound_list_Delete(SecNum, offset + index);
    }

    output = section_output_list_FindEntryEqual(SecNum, offset);
    if (output != NULL)
    {
        if (output->str != NULL)
        {
            free(output->str);
            output->str = NULL;
        }
        output->len = length - 1;
        output->type = OT_INSTRUCTION;

        if (section[SecNum].type != ST_CODE)
        {
            output->type = OT_UNKNOWN;
            if (length & 3)
            {
                fprintf(stderr, "Warning: data replacement causes misalignement - %i - %i (0x%x)\n", SecNum, (unsigned int)offset, (unsigned int)(section[SecNum].start + offset));
            }
        }
    }

    return output;
}

/* ************************************************************************** */

#if (OUTPUT_TYPE != OUT_ORIG && OUTPUT_TYPE != OUT_WINDOWS)
static void SR_disassemble_ignore_instructions(Word_t Index, int Value, void *data)
{
#define DATA ((unsigned int)(uintptr_t) data)

    int index;

    if (Value < 0)
    {
        // a data area (data_areas.sci): the bytes stay, marked as data, so that they are written out and can be
        // referred to, but nothing decodes them as instructions
        for (index = -Value - 1; index >= 0; index--)
        {
            output_data *output;

            output = section_output_list_FindEntryEqual(DATA, Index + index);
            if (output != NULL && output->type != OT_OFFSET) // an address inside the data (a fixup) stays as it is
            {
                if (output->str != NULL)
                {
                    free(output->str);
                    output->str = NULL;
                }
                output->len = 0;
                output->type = OT_DATA;
            }
            section_bound_list_Delete(DATA, Index + index);
            section_entry_list_Delete(DATA, Index + index);
            section_export_list_Delete(DATA, Index + index);
        }
        return;
    }

    if (Index + Value == section[DATA].size)
    {
        Value++;
    }

    for (index = Value - 1; index >= 0; index--)
    {
        section_output_list_Delete(DATA, Index + index);
        section_bound_list_Delete(DATA, Index + index);
        section_entry_list_Delete(DATA, Index + index);
        section_export_list_Delete(DATA, Index + index);
    }

    if (Index + Value > section[DATA].size)
    {
        output_data *output;

        output = section_output_list_FindEntryEqualOrLower(DATA, section[DATA].size);

        if (output != NULL)
        {
            output = section_output_list_Insert(DATA, output->ofs + output->len + 1);
            if ( output != NULL )
            {
                output->type = OT_NONE;
                output->has_label = 1;
                output->len = (uint_fast32_t) -1;
            }
        }
    }

#undef DATA
}

static void SR_disassemble_replace_instructions(replace_data *item, void *data)
{
    output_data *output;
    bound_data *bound;

#define DATA ((unsigned int)(uintptr_t) data)

    output = SR_disassemble_offset_init_output(DATA, item->ofs, item->length);

    if (output != NULL)
    {
        output->str = strdup(item->instr);

        bound = section_bound_list_Insert(DATA, item->ofs);
        if (bound != NULL) bound->begin = 1;
    }

    if (section[DATA].type == ST_CODE)
    {
        section_entry_list_Insert(DATA, item->ofs + item->length);
        bound = section_bound_list_Insert(DATA, item->ofs + item->length);
        if (bound != NULL) bound->begin = 1;
    }

#undef DATA
}
#endif

#if ((OUTPUT_TYPE == OUT_X86) || (OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
static void SR_remove_nops(unsigned int Entry, uint_fast32_t begin_ofs, uint_fast32_t end_ofs)
{
    output_data *first;

    if (begin_ofs == 0)
    {
        first = section_output_list_FindEntryFirst(Entry);
        begin_ofs = (first != NULL)?first->ofs:end_ofs;
    }

    for (; begin_ofs < end_ofs; begin_ofs++)
    {
        section_output_list_Delete(Entry, begin_ofs);
    }
}

static void SR_disassemble_create_aligns_code(output_data *item, void *data)
{
#define DATA ((Entry_region *) data)

    switch (DATA->isregion)
    {
    case 0: // data
        if (item->type == OT_INSTRUCTION)
        {
            if (item->align == 0)
            {
                // first instruction after data gets aligned
                item->align = 4;
            }
            DATA->isregion = 1;
        }
        break;
    case 1: // instructions
        if (item->type != OT_INSTRUCTION)
        {
            if (item->type == OT_OFFSET && item->has_label && (item->align == 0 || item->align == 4))
            {
                // jump table after instructions
                DATA->isregion = 3;
            }
            else if (item->type == OT_UNKNOWN && item->len == 0 && item->str == NULL && item->has_label == 0 && item->align == 0 && (section[DATA->Entry].adr[item->ofs] == 0x90 || section[DATA->Entry].adr[item->ofs] == 0xCC))
            {
                DATA->isregion = 2;
                DATA->begin_ofs = item->ofs;
            }
            else
            {
                DATA->isregion = 0;
            }
        }
        break;
    case 2: // nops after instructions
        if (item->type == OT_INSTRUCTION)
        {
            if (item->has_label && item->align == 0 && (item->ofs & 0x03) == 0)
            {
                if ((item->ofs & 0x0f) == 0)
                {
                    item->align = 16;
                }
                else if ((item->ofs & 0x07) == 0)
                {
                    item->align = 8;
                }
                else
                {
                    item->align = 4;
                }

                // remove nops before alignment
                SR_remove_nops(DATA->Entry, DATA->begin_ofs, item->ofs);
            }
            else if (item->align == 0)
            {
                // first instruction after data gets aligned
                item->align = 4;
            }

            DATA->isregion = 1;
        }
        else if (item->type == OT_OFFSET)
        {
            if (item->has_label && (item->align == 0 || item->align == 4))
            {
                // jump table after instructions
                item->align = 4;

                // remove nops before alignment
                SR_remove_nops(DATA->Entry, DATA->begin_ofs, item->ofs);
            }

            DATA->isregion = 3;
        }
        else if (item->type == OT_NONE)
        {
            output_data *last;

            last = section_output_list_FindEntryLast(DATA->Entry);
            if (last != NULL && last->ofs == item->ofs)
            {
                // remove nops before end
                SR_remove_nops(DATA->Entry, DATA->begin_ofs, item->ofs);
            }

            DATA->isregion = 0;
        }
        else if (item->type != OT_UNKNOWN || item->len || item->str || item->has_label || item->align || (section[DATA->Entry].adr[item->ofs] != 0x90 && section[DATA->Entry].adr[item->ofs] != 0xCC))
        {
            DATA->isregion = 0;
        }
        break;
    case 3: // jump table
        if (item->type != OT_OFFSET)
        {
            if (item->type == OT_INSTRUCTION)
            {
                if (item->align == 0)
                {
                    // first instruction after data gets aligned
                    item->align = 4;
                }
                DATA->isregion = 1;
            }
            else if (item->type == OT_UNKNOWN && item->len == 0 && item->str == NULL && item->has_label == 0 && item->align == 0 && (section[DATA->Entry].adr[item->ofs] == 0x90 || section[DATA->Entry].adr[item->ofs] == 0xCC))
            {
                DATA->isregion = 2;
                DATA->begin_ofs = item->ofs;
            }
            else
            {
                DATA->isregion = 0;
            }
        }
        break;
    }

#undef DATA
}

static void SR_disassemble_create_aligns_data(output_data *item, void *data)
{
#define DATA ((Entry_region *) data)

    if (item->type == OT_INSTRUCTION)
    {
        DATA->isregion = 0;
    }
    else
    {
        if (DATA->isregion == 0)
        {
            if (item->align)
            {
                DATA->isregion = -1;
            }
            else
            {
                if (item->has_label)
                {
                    if (item->type != OT_NONE)
                    {
                        item->align = 4;
                        DATA->isregion = -1;
                    }
                }
                else
                {
                    if (item->type != OT_UNKNOWN)
                    {
                        DATA->isregion = -1;
                    }
                }
            }
        }
    }

#undef DATA
}
#endif

#if ((OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
static void SR_disassemble_create_regions(bound_data *item, void *data)
{
    output_data *output;


#define DATA ((Entry_region *) data)

    if (DATA->isregion && item->begin)
    {
        output = section_output_list_FindEntryLower(DATA->Entry, item->ofs);

        if (output != NULL)
        {
            section_region_list_Insert(DATA->Entry, DATA->begin_ofs, output->ofs);
        }
        DATA->isregion = 0;
    }

    if (item->begin)
    {
        if (item->end)
        {
            section_region_list_Insert(DATA->Entry, item->ofs, item->ofs);
        }
        else
        {
            DATA->isregion = 1;
            DATA->begin_ofs = item->ofs;
        }
    }
    else
    {
        if (DATA->isregion)
        {
            section_region_list_Insert(DATA->Entry, DATA->begin_ofs, item->ofs);

            DATA->isregion = 0;
        }
        else
        {
            section_region_list_Insert(DATA->Entry, item->ofs, item->ofs);
        }
    }


#undef DATA
}
#endif

/* ************************************************************************** */

int SR_full_disassembly(void)
{
    unsigned int index;
    int finished, ret;
#if (OUTPUT_TYPE != OUT_ORIG)
    uint_fast32_t offset;
#endif
#if ((OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
    output_data *output;
    region_data *region;
    replace_data *replace;
#endif
#if ((OUTPUT_TYPE == OUT_X86) || (OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
    Entry_region ER;
#endif
    Word_t entry_index;


#if (OUTPUT_TYPE == OUT_ORIG)
    ret = 0;
#endif


#if (OUTPUT_TYPE != OUT_ORIG && OUTPUT_TYPE != OUT_WINDOWS)
    for (index = 0; index < num_sections; index++)
    {
        section_ignored_list_ForEach(index, &SR_disassemble_ignore_instructions, (void *)(uintptr_t) index);
        section_replace_list_ForEach(index, &SR_disassemble_replace_instructions, (void *)(uintptr_t) index);
    }
#endif

#if (OUTPUT_TYPE == OUT_LLASM)
    ret = init_udis86_dep();

    if (ret) return ret;
#endif

    finished = 0;

    while (!finished)
    {
        finished = 1;

        for (index = 0; index < num_sections; index++)
        {
            // process only code segments
            if (section[index].type != ST_CODE) continue;

            while (section_entry_list_FindFirstIndex(index, &entry_index))
            {
                finished = 0;

#if (OUTPUT_TYPE != OUT_ORIG)
                offset = (uint_fast32_t)entry_index;
#endif
                section_entry_list_Delete(index, entry_index);

#if (OUTPUT_TYPE == OUT_WINDOWS)
                ret = SR_disassemble_offset_windows(index, offset);
#endif
#if ((OUTPUT_TYPE == OUT_X86) || (OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
                ret = SR_disassemble_offset_win32(index, offset);
#endif

                if (ret) return ret;
            }
        }
    }

#if ((OUTPUT_TYPE == OUT_X86) || (OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
    // align code and data in code segments
    for (index = 0; index < num_sections; index++)
    {
        // process only code segments
        if (section[index].type != ST_CODE) continue;

        ER.Entry = index;
        ER.begin_ofs = 0;
        ER.isregion = 2;

        section_output_list_ForEach(index, &SR_disassemble_create_aligns_code, (void *) &ER);

        ER.Entry = index;
        ER.isregion = -1;

        section_output_list_ForEach(index, &SR_disassemble_create_aligns_data, (void *) &ER);
    }
#endif

#if ((OUTPUT_TYPE == OUT_LLASM) || (OUTPUT_TYPE == OUT_X64))
    // create regions from bounds
    for (index = 0; index < num_sections; index++)
    {
        // process only code segments
        if (section[index].type != ST_CODE) continue;

        ER.Entry = index;
        ER.isregion = 0;

        section_bound_list_ForEach(index, &SR_disassemble_create_regions, (void *) &ER);
        if (ER.isregion)
        {
            output = section_output_list_FindEntryLast(index);

            if (output != NULL)
            {
                section_region_list_Insert(index, ER.begin_ofs, output->ofs);
            }
        }
    }


    // process regions
    for (index = 0; index < num_sections; index++)
    {
        // process only code segments
        if (section[index].type != ST_CODE) continue;

#ifdef USE_JUDY
        region = section_region_list_FindEntryFirst(index);
#else
        // flags needed at the start of a region are handed to the preceding region, so go from the end
        region = section_region_list_FindEntryLast(index);
#endif

        while (region != NULL)
        {
            replace = section_replace_list_FindEntryEqual(index, region->begin_ofs);

            if (replace == NULL)
            {
#if (OUTPUT_TYPE == OUT_LLASM)
                ret = SR_disassemble_region_llasm(index, region);
#elif (OUTPUT_TYPE == OUT_X64)
                ret = SR_disassemble_region_x64(index, region);
#endif

                if (ret) return ret;
            }

#ifdef USE_JUDY
            region = section_region_list_FindEntryHigher(index, region->begin_ofs);
#else
            if (region->begin_ofs != 0)
            {
                region = section_region_list_FindEntryLower(index, region->begin_ofs);
            }
            else
            {
                region = NULL;
            }
#endif
        }
    }
#endif

#if (OUTPUT_TYPE == OUT_LLASM) && !defined(USE_JUDY)
    // a region reached by a jump may need flags that the jumping region has to produce: iterate until nothing changes
    for (;;)
    {
        int changed = 0;

        for (index = 0; index < num_sections; index++)
        {
            if (section[index].type != ST_CODE) continue;

            region = section_region_list_FindEntryLast(index);
            while (region != NULL)
            {
                if (section_replace_list_FindEntryEqual(index, region->begin_ofs) == NULL)
                {
                    uint_fast32_t need = region->flags_to_write;
                    const char *p;

                    output = section_output_list_FindEntryEqual(index, region->end_ofs);
                    p = (output != NULL && output->str != NULL) ? output->str : "";
                    for (; *p != 0; p++)
                    {
                        // labels of jumps: "tcall loc_X", "ctcallz cond, loc_X", "ctcallnz cond, loc_X" at the start of a line
                        if (p == output->str || p[-1] == '\n')
                        {
                            const char *label = NULL;

                            if (strncmp(p, "tcall loc_", 10) == 0) label = p + 6;
                            else if (strncmp(p, "ctcallz ", 8) == 0 || strncmp(p, "ctcallnz ", 9) == 0)
                            {
                                label = strstr(p, ", loc_");
                                if (label != NULL && memchr(p, '\n', (size_t) (label - p)) == NULL) label += 2;
                                else label = NULL;
                            }
                            if (label != NULL)
                            {
                                unsigned long address = strtoul(label + 4, NULL, 16);
                                region_data *target;

                                if (address >= section[index].start && address < section[index].start + section[index].size)
                                {
                                    target = section_region_list_FindEntryEqual(index, (uint_fast32_t) (address - section[index].start));
                                    if (target != NULL) need |= (target->flags_start_jump & FL_COND_ALL);
                                }
                            }
                        }
                    }

                    if (need != region->flags_translated)
                    {
                        region->flags_to_write = need;
#if (OUTPUT_TYPE == OUT_LLASM)
                        ret = SR_disassemble_region_llasm(index, region);
#endif
                        if (ret) return ret;
                        changed = 1;
                    }
                }

                if (region->begin_ofs != 0) region = section_region_list_FindEntryLower(index, region->begin_ofs);
                else region = NULL;
            }
        }

        if (!changed) break;
    }
#endif

#if (OUTPUT_TYPE == OUT_LLASM)
    if (SR_collect_report() && !SR_collect_enabled()) return 8; /* collect mode: report, then carry on with the gaps commented out */
#endif

    return 0;
}
