/* Files of the MPQ archives read without the client: the sound system (WoW-fmod.c) takes the files of its sounds from here, so that
   it does not call the client's file functions (Storm, recompiled: they need the big lock, and decompressing a wave file of 5 MB took
   them 6-10 s). Format 1 archives: hash and block tables, sectors compressed with zlib, Huffman and ADPCM (the decompressors
   of StormLib, third_party/stormlib), encrypted files. The archives are read through the data streaming (SRW-web/src/remote.c).
   A file that this cannot read (an imploded one, another compression, a file outside the archives) is left to the client. */
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "third_party/stormlib/huff.h"
#include "third_party/stormlib/adpcm.h"

extern "C" {
#include "srw_remote.h"
}

namespace {

uint32_t crypt_table[0x500];

void make_crypt_table()
{
    uint32_t seed = 0x00100001;

    for (int i = 0; i < 0x100; i++)
    {
        for (int k = 0, index = i; k < 5; k++, index += 0x100)
        {
            uint32_t a, b;

            seed = (seed * 125 + 3) % 0x2AAAAB;
            a = (seed & 0xFFFF) << 16;
            seed = (seed * 125 + 3) % 0x2AAAAB;
            b = seed & 0xFFFF;
            crypt_table[index] = a | b;
        }
    }
}

uint32_t hash_string(const char *text, uint32_t type)
{
    uint32_t seed1 = 0x7FED7FED, seed2 = 0xEEEEEEEE;

    for (; *text != 0; text++)
    {
        uint32_t c = (unsigned char) *text;

        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == '/') c = '\\';
        seed1 = crypt_table[type * 0x100 + c] ^ (seed1 + seed2);
        seed2 = c + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

void decrypt(uint32_t *data, size_t words, uint32_t key)
{
    uint32_t seed = 0xEEEEEEEE;

    for (size_t i = 0; i < words; i++)
    {
        uint32_t value;

        seed += crypt_table[0x400 + (key & 0xFF)];
        value = data[i] ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = value + seed + (seed << 5) + 3;
        data[i] = value;
    }
}

struct HashEntry { uint32_t name1, name2; uint16_t locale, platform; uint32_t block; };
struct BlockEntry { uint32_t position, compressed, size, flags; };

enum : uint32_t {
    FILE_IMPLODE = 0x100, FILE_COMPRESS = 0x200, FILE_ENCRYPTED = 0x10000, FILE_FIX_KEY = 0x20000, FILE_SINGLE_UNIT = 0x1000000,
    FILE_SECTOR_CRC = 0x4000000, FILE_EXISTS = 0x80000000u
};

struct Archive {
    const char *name;
    SRW_RemoteFile *file = nullptr;
    bool tried = false;
    uint32_t sector_size = 4096;
    HashEntry *hashes = nullptr;
    uint32_t hash_count = 0;
    BlockEntry *blocks = nullptr;
    uint32_t block_count = 0;
};

/* the patches first: their files replace those of the others */
Archive archives[] = {
    /* {L} is the locale of the game (/Port/locale.txt); archives that are not in the manifest are skipped */
    { "/Data/{L}/patch-{L}-3.MPQ" }, { "/Data/{L}/patch-{L}-2.MPQ" }, { "/Data/{L}/patch-{L}.MPQ" }, { "/Data/patch-3.MPQ" },
    { "/Data/patch-2.MPQ" }, { "/Data/patch.MPQ" },
    { "/Data/lichking.MPQ" }, { "/Data/expansion.MPQ" }, { "/Data/common-2.MPQ" }, { "/Data/common.MPQ" },
    { "/Data/{L}/lichking-speech-{L}.MPQ" }, { "/Data/{L}/lichking-locale-{L}.MPQ" }, { "/Data/{L}/expansion-speech-{L}.MPQ" },
    { "/Data/{L}/expansion-locale-{L}.MPQ" }, { "/Data/{L}/speech-{L}.MPQ" }, { "/Data/{L}/locale-{L}.MPQ" }, { "/Data/{L}/base-{L}.MPQ" },
};

pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;

/* the names of the archives that hold {L}: the locale the page gives to the game (/Port/locale.txt: four letters, e.g. enGB), enGB without it (open_lock is held) */
void set_locale_names()
{
    char locale[16] = "enGB";
    FILE *f = fopen("/Port/locale.txt", "r");

    if (f != nullptr)
    {
        char text[16] = "";

        if (fgets(text, sizeof(text), f) != nullptr && strlen(text) >= 4 && isalpha((unsigned char) text[0]) && isalpha((unsigned char) text[1]) &&
            isalpha((unsigned char) text[2]) && isalpha((unsigned char) text[3]) && (text[4] == 0 || isspace((unsigned char) text[4])))
        {
            memcpy(locale, text, 4);
            locale[4] = 0;
        }
        fclose(f);
    }
    for (Archive &a : archives)
    {
        const char *at;

        if ((at = strstr(a.name, "{L}")) == nullptr) continue;
        char *name = (char *) malloc(strlen(a.name) + 1 + 3 * 4);   /* (at most three marks, each 3 characters become 4) */
        char *out = name;

        for (const char *in = a.name; *in != 0;)
        {
            if (strncmp(in, "{L}", 3) == 0) { memcpy(out, locale, 4); out += 4; in += 3; }
            else *out++ = *in++;
        }
        *out = 0;
        a.name = name;
    }
}

bool read_at(SRW_RemoteFile *file, uint64_t offset, void *buffer, uint32_t size)
{
    return SRW_RemoteRead(file, offset, buffer, size) == (int64_t) size;
}

/* loads the tables of an archive the first time (open_lock is held) */
bool open_archive(Archive &a)
{
    if (a.tried) return a.hashes != nullptr;
    a.tried = true;
    a.file = SRW_RemoteFind(a.name);
    if (a.file == nullptr) return false;

    uint32_t header[8];

    if (!read_at(a.file, 0, header, sizeof(header)) || header[0] != 0x1A51504D) return false;   /* "MPQ\x1A" */
    uint16_t shift = (uint16_t) (header[3] >> 16);
    uint32_t hash_position = header[4], block_position = header[5];

    a.sector_size = 512u << shift;
    a.hash_count = header[6];
    a.block_count = header[7];
    a.hashes = (HashEntry *) malloc(a.hash_count * sizeof(HashEntry));
    a.blocks = (BlockEntry *) malloc(a.block_count * sizeof(BlockEntry));
    if (a.hashes == nullptr || a.blocks == nullptr ||
        !read_at(a.file, hash_position, a.hashes, a.hash_count * sizeof(HashEntry)) ||
        !read_at(a.file, block_position, a.blocks, a.block_count * sizeof(BlockEntry)))
    {
        free(a.hashes);
        free(a.blocks);
        a.hashes = nullptr;
        a.blocks = nullptr;
        return false;
    }
    decrypt((uint32_t *) a.hashes, a.hash_count * 4, hash_string("(hash table)", 3));
    decrypt((uint32_t *) a.blocks, a.block_count * 4, hash_string("(block table)", 3));
    return true;
}

const BlockEntry *find_file(const Archive &a, const char *name)
{
    uint32_t mask = a.hash_count - 1, start = hash_string(name, 0) & mask, name1 = hash_string(name, 1), name2 = hash_string(name, 2);
    const BlockEntry *found = nullptr;

    if (a.hash_count == 0 || (a.hash_count & mask) != 0) return nullptr;
    for (uint32_t n = 0, i = start; n < a.hash_count; n++, i = (i + 1) & mask)
    {
        const HashEntry &h = a.hashes[i];

        if (h.block == 0xFFFFFFFF) break;   /* never used: the end of the chain */
        if (h.name1 == name1 && h.name2 == name2 && h.block < a.block_count && (a.blocks[h.block].flags & FILE_EXISTS))
        {
            found = &a.blocks[h.block];
            if (h.locale == 0) break;   /* the neutral one is preferred */
        }
    }
    return found;
}

/* one sector: the compressions of its first byte undone, in the order of StormLib (Huffman before ADPCM); false when one is unknown */
bool decompress_sector(uint8_t *out, uint32_t out_size, uint8_t *in, uint32_t in_size)
{
    uint8_t mask = in[0];
    uint8_t *temp = (uint8_t *) malloc(out_size), *source = in + 1;
    uint32_t source_size = in_size - 1;
    bool ok = true;

    if (temp == nullptr) return false;
    if (mask & ~(0x02 | 0x01 | 0x80 | 0x40)) ok = false;
    if (ok && mask == 0)
    {
        memcpy(out, source, source_size < out_size ? source_size : out_size);
    }   /* bzip2, implode, sparse: not used by the sound files */
    if (ok && (mask & 0x02))
    {
        uLongf length = out_size;

        ok = uncompress(temp, &length, source, source_size) == Z_OK;
        memcpy(out, temp, length);
        source = out;
        source_size = (uint32_t) length;
    }
    if (ok && (mask & 0x01))
    {
        THuffmannTree tree(false);
        TInputStream stream(source, source_size);
        uint32_t length = tree.Decompress(temp, out_size, &stream);

        ok = length > 0;
        memcpy(out, temp, length);
        source = out;
        source_size = length;
    }
    if (ok && (mask & (0x80 | 0x40)))
    {
        int length = DecompressADPCM(temp, (int) out_size, source, (int) source_size, (mask & 0x80) ? 2 : 1);

        ok = length > 0;
        memcpy(out, temp, (size_t) length);
        source_size = (uint32_t) length;
    }
    ok = ok && source_size == out_size;
    free(temp);
    return ok;
}

uint8_t *read_file(Archive &a, const BlockEntry &b, const char *name)
{
    uint32_t key = 0;
    uint8_t *data = (uint8_t *) malloc(b.size > 0 ? b.size : 1);

    if (data == nullptr) return nullptr;
    if ((b.flags & FILE_IMPLODE) || ((b.flags & FILE_COMPRESS) && (b.flags & FILE_SINGLE_UNIT)))
    {
        free(data);   /* not used by the sound files: the client reads it */
        return nullptr;
    }
    if (b.flags & FILE_ENCRYPTED)
    {
        const char *base = strrchr(name, '\\');

        key = hash_string(base != nullptr ? base + 1 : name, 3);
        if (b.flags & FILE_FIX_KEY) key = (key + b.position) ^ b.size;
    }
    if (!(b.flags & FILE_COMPRESS))
    {
        /* stored: read as it is (decrypted sector by sector) */
        if (!read_at(a.file, b.position, data, b.size))
        {
            free(data);
            return nullptr;
        }
        if (b.flags & FILE_ENCRYPTED)
        {
            for (uint32_t s = 0, o = 0; o < b.size; s++, o += a.sector_size)
            {
                uint32_t length = b.size - o < a.sector_size ? b.size - o : a.sector_size;

                decrypt((uint32_t *) (data + o), length / 4, key + s);
            }
        }
        return data;
    }

    /* compressed in sectors: the table of their offsets, then each one */
    uint32_t sectors = (b.size + a.sector_size - 1) / a.sector_size;
    uint32_t table_words = sectors + 1 + ((b.flags & FILE_SECTOR_CRC) ? 1 : 0);
    uint8_t *packed = (uint8_t *) malloc(b.compressed);
    uint32_t *offsets;
    bool ok = packed != nullptr && read_at(a.file, b.position, packed, b.compressed);

    if (!ok || table_words * 4 > b.compressed)
    {
        free(packed);
        free(data);
        return nullptr;
    }
    offsets = (uint32_t *) packed;
    if (b.flags & FILE_ENCRYPTED) decrypt(offsets, table_words, key - 1);
    for (uint32_t s = 0; s < sectors && ok; s++)
    {
        uint32_t start = offsets[s], end = offsets[s + 1];
        uint32_t want = (s + 1 < sectors) ? a.sector_size : b.size - s * a.sector_size;

        if (end < start || end > b.compressed)
        {
            ok = false;
            break;
        }
        uint8_t *in = packed + start;
        uint32_t in_size = end - start;

        if (b.flags & FILE_ENCRYPTED) decrypt((uint32_t *) in, in_size / 4, key + s);
        if (in_size == want) memcpy(data + s * a.sector_size, in, want);   /* this sector is stored */
        else ok = in_size > 0 && decompress_sector(data + s * a.sector_size, want, in, in_size);
    }
    free(packed);
    if (!ok)
    {
        free(data);
        return nullptr;
    }
    return data;
}

}  // namespace

/* finds the file: its size and whether it can be read here (1), 0 when not */
extern "C" int WoW_MpqFind(const char *name, int64_t *size)
{
    static bool table_ready = false;
    int found = 0;

    pthread_mutex_lock(&open_lock);
    if (!table_ready)
    {
        make_crypt_table();
        set_locale_names();
        table_ready = true;
    }
    for (Archive &a : archives)
    {
        const BlockEntry *b;

        if (!open_archive(a)) continue;
        b = find_file(a, name);
        if (b == nullptr) continue;
        found = !((b->flags & FILE_IMPLODE) || ((b->flags & FILE_COMPRESS) && (b->flags & FILE_SINGLE_UNIT)));
        *size = b->size;
        break;
    }
    pthread_mutex_unlock(&open_lock);
    return found;
}

/* the file with this name (as the client names it, e.g. Sound\Ambience\...\x.wav) from the archives; NULL when it is not there or cannot
   be read here. The data is malloc'ed. */
extern "C" uint8_t *WoW_MpqReadFile(const char *name, int64_t *size)
{
    static bool table_ready = false;

    pthread_mutex_lock(&open_lock);
    if (!table_ready)
    {
        make_crypt_table();
        set_locale_names();
        table_ready = true;
    }
    for (Archive &a : archives)
    {
        const BlockEntry *b;

        if (!open_archive(a)) continue;
        b = find_file(a, name);
        if (b == nullptr) continue;
        BlockEntry copy = *b;
        pthread_mutex_unlock(&open_lock);
        uint8_t *data = read_file(a, copy, name);

        if (data != nullptr) *size = copy.size;
        return data;   /* (the file of the highest archive that has it, or NULL: an older copy below it would be wrong) */
    }
    pthread_mutex_unlock(&open_lock);
    return nullptr;
}
