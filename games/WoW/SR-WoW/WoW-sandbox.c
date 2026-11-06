/* A private server inside the program, for a client that has no network: a port of AIO Sandbox (https://github.com/barncastle/AIO-Sandbox,
   MIT License, Copyright (c) 2020 barncastle), its plugins for the client 3.3.x (WotLK_11685, adapted to build 12340 = 3.3.5a) and what it needs from Common. It
   is started when the realm list of the client is 127.0.0.1 (WoW_SandboxStart) and listens with the in-process sockets of
   winsock.c of the runtime: the realm/login server on port 3724 and the world server on port 8129. The character files (Accounts/NAME.dat, the
   format of AIO Sandbox) are kept in /Accounts, which the page keeps in the browser's storage like WTF. The password of every account
   is "player".

   Built with -DSANDBOX_NATIVE it uses the sockets of the system instead. */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#ifdef SANDBOX_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#ifndef SANDBOX_ACCOUNT_DIR
#define SANDBOX_ACCOUNT_DIR "Accounts"
#endif
/* (the frame time chart of the program, WoW-prof.c; stubs in the test) */
extern volatile int SRW_ProfOn;
void SRW_ProfSet(int on);
void SRW_GilSetMode(int game_first, double budget_ms, double min_slice_ms);
int SRW_GilGetMode(void);
void WoW_ZlibSetMode(int mode);
int WoW_ZlibGetMode(void);
#else
#include "WoW.h"
#include "imports.h"
#define SANDBOX_ACCOUNT_DIR "/Sandbox/Accounts"
#endif

#define SANDBOX_BUILD 12340
#define SANDBOX_REALM_NAME "WotLK (3.3.5a) Sandbox"
#define SANDBOX_PASSWORD "player"
#ifndef SANDBOX_REALM_PORT
#define SANDBOX_REALM_PORT 3724
#endif
#ifndef SANDBOX_WORLD_PORT
#define SANDBOX_WORLD_PORT 8129
#endif

typedef struct {
    float x, y, z, o;
    uint32_t map;
    const char *description;
} SandboxPlace;

typedef struct {
    uint32_t id;
    SandboxPlace place;
} SandboxTrigger;

typedef struct {
    uint32_t text_emote;
    uint32_t animation;
} SandboxEmote;

#include "WoW-sandbox-data.inc"

/* ------------------------------------------------------------------------------------------------------------------ */
/* the system under the server: sockets and the log */

#ifdef SANDBOX_NATIVE

static void sb_log(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static int sb_listen_on(uint16_t port)
{
    struct sockaddr_in address;
    int one = 1, s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *) &address, sizeof(address)) != 0 || listen(s, 8) != 0)
    {
        close(s);
        return -1;
    }
    return s;
}

static int sb_accept_from(int listener)
{
    return accept(listener, NULL, NULL);
}

static int sb_read_some(int s, void *buffer, size_t length)
{
    ssize_t got = recv(s, buffer, length, 0);

    return got > 0 ? (int) got : 0;
}

static int sb_write_all(int s, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;

    while (length > 0)
    {
        ssize_t put = send(s, bytes, length, 0);

        if (put <= 0) return 0;
        bytes += put;
        length -= (size_t) put;
    }
    return 1;
}

static void sb_close(int s)
{
    close(s);
}

#else

typedef struct {
    uint16_t family;
    uint16_t port;   /* network order */
    uint32_t address;
    uint8_t zero[8];
} SbAddress;

static void sb_log(const char *format, ...)
{
    char text[400];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    logprint(text);
}

static int sb_listen_on(uint16_t port)
{
    uint32_t s = socket_c(2, 1, 6);
    SbAddress address = { 2, (uint16_t) ((port << 8) | (port >> 8)), 0x0100007f, { 0 } };

    if (s == 0xffffffffu) return -1;
    if (bind_c(s, &address, sizeof(address)) == 0xffffffffu || listen_c(s, 8) == 0xffffffffu)
    {
        closesocket_c(s);
        return -1;
    }
    return (int) s;
}

static int sb_accept_from(int listener)
{
    SbAddress peer;
    uint32_t length = sizeof(peer), s = accept_c((uint32_t) listener, &peer, &length);

    return s == 0xffffffffu ? -1 : (int) s;
}

static int sb_read_some(int s, void *buffer, size_t length)
{
    uint32_t got = recv_c((uint32_t) s, buffer, (uint32_t) length, 0);

    return (got == 0xffffffffu || got == 0) ? 0 : (int) got;
}

static int sb_write_all(int s, const void *buffer, size_t length)
{
    return send_c((uint32_t) s, (void *) buffer, (uint32_t) length, 0) != 0xffffffffu;
}

static void sb_close(int s)
{
    closesocket_c((uint32_t) s);
}

#endif

static int sb_read_exact(int s, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;

    while (length > 0)
    {
        int got = sb_read_some(s, bytes, length);

        if (got <= 0) return 0;
        bytes += got;
        length -= (size_t) got;
    }
    return 1;
}

/* between minimum and maximum bytes: a packet of the client arrives with one send, so what is there is the whole of it */
static int sb_read_between(int s, uint8_t *buffer, size_t minimum, size_t maximum)
{
    size_t got = 0;

    while (got < minimum)
    {
        int n = sb_read_some(s, buffer + got, maximum - got);

        if (n <= 0) return 0;
        got += (size_t) n;
    }
    return 1;
}

static uint32_t sb_tick_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t) ((uint64_t) now.tv_sec * 1000u + (uint64_t) now.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* SHA-1 */

typedef struct {
    uint32_t h[5];
    uint64_t length;
    uint8_t block[64];
    uint32_t used;
} Sha1;

static uint32_t rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++) w[i] = ((uint32_t) p[4 * i] << 24) | ((uint32_t) p[4 * i + 1] << 16) | ((uint32_t) p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++)
    {
        uint32_t f, k;

        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
    s->length = 0;
    s->used = 0;
}

static void sha1_update(Sha1 *s, const void *data, size_t length)
{
    const uint8_t *p = data;

    s->length += length;
    while (length > 0)
    {
        size_t n = 64 - s->used;

        if (n > length) n = length;
        memcpy(s->block + s->used, p, n);
        s->used += (uint32_t) n;
        p += n;
        length -= n;
        if (s->used == 64)
        {
            sha1_block(s, s->block);
            s->used = 0;
        }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->length * 8;
    uint8_t pad = 0x80, zero = 0, size[8];
    int i;

    sha1_update(s, &pad, 1);
    while (s->used != 56) sha1_update(s, &zero, 1);
    for (i = 0; i < 8; i++) size[i] = (uint8_t) (bits >> (56 - 8 * i));
    sha1_update(s, size, 8);
    for (i = 0; i < 5; i++)
    {
        out[4 * i] = (uint8_t) (s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t) (s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t) (s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t) s->h[i];
    }
}

/* SHA-1 of up to three buffers one after the other */
static void sha1_of(uint8_t out[20], const void *a, size_t alen, const void *b, size_t blen, const void *c, size_t clen)
{
    Sha1 s;

    sha1_init(&s);
    if (a != NULL) sha1_update(&s, a, alen);
    if (b != NULL) sha1_update(&s, b, blen);
    if (c != NULL) sha1_update(&s, c, clen);
    sha1_final(&s, out);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* 256-bit numbers for the SRP6 of the login (little-endian limbs) */

typedef struct {
    uint32_t w[8];
} U256;

static void u256_from_be(U256 *r, const uint8_t *bytes, size_t length)
{
    size_t i;

    memset(r, 0, sizeof(*r));
    for (i = 0; i < length && i < 32; i++) r->w[i / 4] |= (uint32_t) bytes[length - 1 - i] << (8 * (i % 4));
}

static void u256_to_be(const U256 *a, uint8_t out[32])
{
    int i;

    for (i = 0; i < 32; i++) out[31 - i] = (uint8_t) (a->w[i / 4] >> (8 * (i % 4)));
}

static int u256_cmp(const U256 *a, const U256 *b)
{
    int i;

    for (i = 7; i >= 0; i--)
    {
        if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}

static uint32_t u256_add(U256 *r, const U256 *a, const U256 *b)
{
    uint64_t carry = 0;
    int i;

    for (i = 0; i < 8; i++)
    {
        carry += (uint64_t) a->w[i] + b->w[i];
        r->w[i] = (uint32_t) carry;
        carry >>= 32;
    }
    return (uint32_t) carry;
}

static void u256_sub(U256 *r, const U256 *a, const U256 *b)
{
    int64_t borrow = 0;
    int i;

    for (i = 0; i < 8; i++)
    {
        int64_t d = (int64_t) a->w[i] - b->w[i] + borrow;

        r->w[i] = (uint32_t) d;
        borrow = d >> 32;
    }
}

static int u256_is_zero(const U256 *a)
{
    int i;

    for (i = 0; i < 8; i++)
    {
        if (a->w[i] != 0) return 0;
    }
    return 1;
}

static void u256_reduce(U256 *a, const U256 *m)
{
    while (u256_cmp(a, m) >= 0) u256_sub(a, a, m);
}

/* (a + b) mod m for a, b < m */
static void u256_addmod(U256 *r, const U256 *a, const U256 *b, const U256 *m)
{
    U256 sum;
    uint32_t carry = u256_add(&sum, a, b);

    if (carry || u256_cmp(&sum, m) >= 0) u256_sub(&sum, &sum, m);
    *r = sum;
}

/* (a * b) mod m for a, b < m: the bits of b from the top, doubling and adding */
static void u256_mulmod(U256 *r, const U256 *a, const U256 *b, const U256 *m)
{
    U256 result;
    int bit;

    memset(&result, 0, sizeof(result));
    for (bit = 255; bit >= 0; bit--)
    {
        u256_addmod(&result, &result, &result, m);
        if ((b->w[bit / 32] >> (bit % 32)) & 1) u256_addmod(&result, &result, a, m);
    }
    *r = result;
}

static void u256_powmod(U256 *r, const U256 *base, const U256 *exponent, const U256 *m)
{
    U256 result, b = *base;
    int bit;

    memset(&result, 0, sizeof(result));
    result.w[0] = 1;
    u256_reduce(&b, m);
    for (bit = 255; bit >= 0; bit--)
    {
        u256_mulmod(&result, &result, &result, m);
        if ((exponent->w[bit / 32] >> (bit % 32)) & 1) u256_mulmod(&result, &result, &b, m);
    }
    *r = result;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the login: SRP6 as AIO Sandbox does it. Its constant N is the wire form of the number that the client is told to use, and the
   arithmetic is done modulo the number that the bytes make up when read the other way round; the client takes what it is told. */

static const uint8_t srp_n[32] = {
    0x89, 0x4B, 0x64, 0x5E, 0x89, 0xE1, 0x53, 0x5B, 0xBD, 0xAD, 0x5B, 0x8B, 0x29, 0x06, 0x50, 0x53,
    0x08, 0x01, 0xB1, 0x8E, 0xBF, 0xBF, 0x5E, 0x8F, 0xAB, 0x3C, 0x82, 0x87, 0x2A, 0x3E, 0x9B, 0xB7
};

static const uint8_t srp_salt[32] = {
    0xAD, 0xD0, 0x3A, 0x31, 0xD2, 0x71, 0x14, 0x46, 0x75, 0xF2, 0x70, 0x7E, 0x50, 0x26, 0xB6, 0xD2,
    0xF1, 0x86, 0x59, 0x99, 0x76, 0x02, 0x50, 0xAA, 0xB9, 0x45, 0xE0, 0x9E, 0xDD, 0x2A, 0xA3, 0x45
};

static void reverse_bytes(uint8_t *bytes, size_t length)
{
    size_t i;

    for (i = 0; i < length / 2; i++)
    {
        uint8_t t = bytes[i];

        bytes[i] = bytes[length - 1 - i];
        bytes[length - 1 - i] = t;
    }
}

/* the modulus: the bytes of N read in the other order */
static void srp_modulus(U256 *m)
{
    uint8_t reversed[32];

    memcpy(reversed, srp_n, 32);
    reverse_bytes(reversed, 32);
    u256_from_be(m, reversed, 32);
}

typedef struct {
    int build;
    char username[64];
    uint8_t username_length;
    U256 v, b_exponent, big_b;
    int have_challenge;
} RealmState;

typedef struct {
    char name[64];
    uint8_t key[40];
    int used;
} SessionKey;

#define MAX_SESSIONS 32
static SessionKey sessions[MAX_SESSIONS];
volatile int WoW_SandboxInWorld;
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;

static void upper_ascii(char *text)
{
    for (; *text != 0; text++)
    {
        if (*text >= 'a' && *text <= 'z') *text = (char) (*text - 'a' + 'A');
    }
}

static void remember_session(const char *name, const uint8_t key[40])
{
    int i, slot = -1;

    pthread_mutex_lock(&session_lock);
    for (i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].used && strcmp(sessions[i].name, name) == 0) { slot = i; break; }
        if (slot < 0 && !sessions[i].used) slot = i;
    }
    if (slot < 0) slot = 0;
    sessions[slot].used = 1;
    snprintf(sessions[slot].name, sizeof(sessions[slot].name), "%s", name);
    memcpy(sessions[slot].key, key, 40);
    pthread_mutex_unlock(&session_lock);
}

static int find_session(const char *name, uint8_t key[40])
{
    int i, found = 0;

    pthread_mutex_lock(&session_lock);
    for (i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].used && strcmp(sessions[i].name, name) == 0)
        {
            memcpy(key, sessions[i].key, 40);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&session_lock);
    return found;
}

static void random_bytes(uint8_t *bytes, size_t length)
{
    static int seeded;
    size_t i;

    if (!seeded)
    {
        srand((unsigned int) (sb_tick_ms() ^ (uint32_t) time(NULL) ^ (uint32_t) (uintptr_t) &bytes));
        seeded = 1;
    }
    for (i = 0; i < length; i++) bytes[i] = (uint8_t) (rand() >> 4);
}

/* the answer to LOGON_CHALLENGE: 0x77 bytes (0x76 before build 5428) */
static size_t srp_challenge(RealmState *state, const uint8_t *packet, size_t length, uint8_t *out)
{
    uint8_t credentials_hash[20], inner[20], salted[20], random_exponent[20];
    Sha1 s;
    char credentials[128];
    size_t size;
    U256 m, g, k, gb, kv, x;
    uint8_t big_b[32];
    int i;

    (void) length;
    state->build = packet[11] | (packet[12] << 8);
    state->username_length = packet[33];
    if (state->username_length >= sizeof(state->username)) state->username_length = sizeof(state->username) - 1;
    memcpy(state->username, packet + 34, state->username_length);
    state->username[state->username_length] = 0;
    size = state->build < 5428 ? 0x76 : 0x77;

    snprintf(credentials, sizeof(credentials), "%s:%s", state->username, SANDBOX_PASSWORD);
    upper_ascii(credentials);
    sha1_of(inner, credentials, strlen(credentials), NULL, 0, NULL, 0);
    sha1_init(&s);
    sha1_update(&s, srp_salt, 32);
    sha1_update(&s, inner, 20);
    sha1_final(&s, salted);
    /* x: the digest read as a little-endian number */
    for (i = 0; i < 20; i++) credentials_hash[i] = salted[19 - i];
    u256_from_be(&x, credentials_hash, 20);

    srp_modulus(&m);
    memset(&g, 0, sizeof(g)); g.w[0] = 7;
    memset(&k, 0, sizeof(k)); k.w[0] = 3;
    u256_powmod(&state->v, &g, &x, &m);

    random_bytes(random_exponent, 20);
    u256_from_be(&state->b_exponent, random_exponent, 20);
    u256_powmod(&gb, &g, &state->b_exponent, &m);
    u256_mulmod(&kv, &k, &state->v, &m);
    u256_addmod(&state->big_b, &kv, &gb, &m);
    state->have_challenge = 1;

    memset(out, 0, size);
    u256_to_be(&state->big_b, big_b);
    reverse_bytes(big_b, 32);
    memcpy(out + 3, big_b, 32);
    out[35] = 1;
    out[36] = 7;
    out[37] = 32;
    memcpy(out + 38, srp_n, 32);
    memcpy(out + 70, srp_salt, 32);
    return size;
}

/* the answer to LOGON_PROOF (empty when the number of the client is refused); returns the size of the answer */
static size_t srp_proof(RealmState *state, const uint8_t *packet, uint8_t *out)
{
    const uint8_t *a_wire = packet + 1;   /* A: 32 bytes, little-endian on the wire */
    uint8_t a_be[32], b_wire[32], ab[64], digest[20], u_be[20], s_wire[32], s1[16], s2[16], h1[20], h2[20], ss_hash[40];
    uint8_t n_hash[20], g_hash[20], xor_hash[20], username_hash[20], m1[20], m2[20];
    U256 m, a, u, vu, s, tmp;
    Sha1 sha;
    int i;
    uint8_t seven = 7;

    if (!state->have_challenge) return 0;
    srp_modulus(&m);
    memcpy(a_be, a_wire, 32);
    reverse_bytes(a_be, 32);
    u256_from_be(&a, a_be, 32);
    tmp = a;
    u256_reduce(&tmp, &m);
    if (u256_is_zero(&tmp)) return 0;

    u256_to_be(&state->big_b, b_wire);
    reverse_bytes(b_wire, 32);
    memcpy(ab, a_wire, 32);
    memcpy(ab + 32, b_wire, 32);
    sha1_of(digest, ab, 64, NULL, 0, NULL, 0);
    for (i = 0; i < 20; i++) u_be[i] = digest[19 - i];
    u256_from_be(&u, u_be, 20);

    /* S = (v^u * A)^b */
    u256_powmod(&vu, &state->v, &u, &m);
    u256_reduce(&a, &m);
    u256_mulmod(&s, &vu, &a, &m);
    u256_powmod(&s, &s, &state->b_exponent, &m);
    u256_to_be(&s, s_wire);
    reverse_bytes(s_wire, 32);
    for (i = 0; i < 16; i++)
    {
        s1[i] = s_wire[2 * i];
        s2[i] = s_wire[2 * i + 1];
    }
    sha1_of(h1, s1, 16, NULL, 0, NULL, 0);
    sha1_of(h2, s2, 16, NULL, 0, NULL, 0);
    for (i = 0; i < 20; i++)
    {
        ss_hash[2 * i] = h1[i];
        ss_hash[2 * i + 1] = h2[i];
    }

    sha1_of(n_hash, srp_n, 32, NULL, 0, NULL, 0);
    sha1_of(g_hash, &seven, 1, NULL, 0, NULL, 0);
    for (i = 0; i < 20; i++) xor_hash[i] = (uint8_t) (n_hash[i] ^ g_hash[i]);
    sha1_of(username_hash, state->username, state->username_length, NULL, 0, NULL, 0);
    sha1_init(&sha);
    sha1_update(&sha, xor_hash, 20);
    sha1_update(&sha, username_hash, 20);
    sha1_update(&sha, srp_salt, 32);
    sha1_update(&sha, a_wire, 32);
    sha1_update(&sha, b_wire, 32);
    sha1_update(&sha, ss_hash, 40);
    sha1_final(&sha, m1);
    sha1_init(&sha);
    sha1_update(&sha, a_wire, 32);
    sha1_update(&sha, m1, 20);
    sha1_update(&sha, ss_hash, 40);
    sha1_final(&sha, m2);

    {
        char name[64];

        snprintf(name, sizeof(name), "%s", state->username);
        upper_ascii(name);
        remember_session(name, ss_hash);
    }
    memset(out, 0, 0x20);
    out[0] = 1;
    memcpy(out + 2, m2, 20);
    return 0x16 + 0xA;   /* + account flags, survey id, flags (build 8089 and later) */
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* packets */

typedef struct {
    uint8_t *data;
    size_t length, capacity;
} Buffer;

static void buffer_bytes(Buffer *b, const void *bytes, size_t length)
{
    if (b->length + length > b->capacity)
    {
        b->capacity = (b->length + length) * 2 + 64;
        b->data = realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->length, bytes, length);
    b->length += length;
}

static void buffer_u8(Buffer *b, uint32_t v) { uint8_t x = (uint8_t) v; buffer_bytes(b, &x, 1); }
static void buffer_u16(Buffer *b, uint32_t v) { uint8_t x[2] = { (uint8_t) v, (uint8_t) (v >> 8) }; buffer_bytes(b, x, 2); }
static void buffer_u32(Buffer *b, uint32_t v) { uint8_t x[4] = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) }; buffer_bytes(b, x, 4); }
static void buffer_u64(Buffer *b, uint64_t v) { buffer_u32(b, (uint32_t) v); buffer_u32(b, (uint32_t) (v >> 32)); }
static void buffer_float(Buffer *b, float f) { uint32_t v; memcpy(&v, &f, 4); buffer_u32(b, v); }
static void buffer_string(Buffer *b, const char *text) { buffer_bytes(b, text, strlen(text) + 1); }

static void buffer_packed_guid(Buffer *b, uint64_t guid)
{
    uint8_t packed[9] = { 0 };
    int i, count = 0;

    for (i = 0; i < 8; i++)
    {
        uint8_t byte = (uint8_t) (guid >> (8 * i));

        if (byte != 0)
        {
            packed[0] |= (uint8_t) (1 << i);
            packed[++count] = byte;
        }
    }
    buffer_bytes(b, packed, (size_t) count + 1);
}

typedef struct {
    const uint8_t *data;
    size_t length, position;
} Reader;

static uint32_t reader_u8(Reader *r) { return r->position < r->length ? r->data[r->position++] : 0; }
static uint32_t reader_u16(Reader *r) { uint32_t a = reader_u8(r); return a | (reader_u8(r) << 8); }
static uint32_t reader_u32(Reader *r) { uint32_t a = reader_u16(r); return a | (reader_u16(r) << 16); }
static uint64_t reader_u64(Reader *r) { uint64_t a = reader_u32(r); return a | ((uint64_t) reader_u32(r) << 32); }
static float reader_float(Reader *r) { uint32_t v = reader_u32(r); float f; memcpy(&f, &v, 4); return f; }

static void reader_string(Reader *r, char *out, size_t size)
{
    size_t n = 0;

    while (r->position < r->length)
    {
        uint8_t c = r->data[r->position++];

        if (c == 0) break;
        if (n + 1 < size) out[n++] = (char) c;
    }
    out[n] = 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* characters and accounts (the file format of AIO Sandbox: one BinaryWriter record for each character) */

#define MAX_CHARACTERS 64

typedef struct {
    int build;
    uint8_t char_class, gender, face, facial_hair, hair_color, hair_style, skin, race, power_type;
    uint32_t display_id, map, zone, mount_display_id;
    uint64_t guid;
    float x, y, z, o, scale;
    uint8_t is_flying;
    char name[40];
    /* not saved */
    uint32_t level, health, mana, rage, strength, agility, stamina, intellect, spirit, stand_state;
    uint8_t rested_state;
    int teleporting;
} Character;

typedef struct {
    char name[64];
    Character characters[MAX_CHARACTERS];
    int count;
    int active;   /* index of the character in the world, -1 for none */
} Account;

static void character_defaults(Character *c)
{
    c->level = 55;   /* enables the death knight */
    c->health = 100;
    c->mana = 100;
    c->rage = 1000;
    c->strength = c->agility = c->stamina = c->intellect = c->spirit = 10;
    c->power_type = 1;
    c->rested_state = 3;
    c->stand_state = 0;
    c->teleporting = 0;
    c->mount_display_id = 0;
}

static const uint32_t display_ids[12][2] = {
    { 0, 0 }, { 0x31, 0x32 }, { 0x33, 0x34 }, { 0x35, 0x36 }, { 0x37, 0x38 }, { 0x39, 0x3A }, { 0x3B, 0x3C }, { 0x61B, 0x61C }, { 0x5C6, 0x5C7 },
    { 0x1AEE, 0x1AEF }, { 0x3C74, 0x3C73 }, { 0x3EFD, 0x3EFE }
};

static uint32_t character_display_id(const Character *c)
{
    unsigned int race = c->race >= 1 && c->race <= 11 ? c->race : 1;

    return display_ids[race][c->gender & 1];
}

static uint32_t character_faction(const Character *c)
{
    switch (c->race)
    {
    case 8: return 116;
    case 9: return 2204;
    case 10: return 1610;
    case 11: return 1629;
    default: return c->race >= 1 && c->race <= 7 ? c->race : 1;
    }
}

/* the values a new character gets */
static void character_set_defaults(Character *c)
{
    int male = c->gender == 0;

    c->scale = 1.0f;
    if (c->race == 6) c->scale = male ? 1.35f : 1.25f;
    c->display_id = character_display_id(c);
    switch (c->char_class)
    {
    case 1: c->power_type = 1; break;   /* warrior: rage */
    case 4: c->power_type = 3; break;   /* rogue: energy */
    case 6: c->power_type = 6; break;   /* death knight: runic power */
    default: c->power_type = 0; break;  /* mana (hunters have mana in this build) */
    }
}

static void account_path(const char *name, char *path, size_t size)
{
    char clean[64];
    size_t i;

    snprintf(clean, sizeof(clean), "%s", name);
    upper_ascii(clean);
    for (i = 0; clean[i] != 0; i++)
    {
        char c = clean[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) clean[i] = '_';
    }
    snprintf(path, size, "%s/%s.dat", SANDBOX_ACCOUNT_DIR, clean);
}

static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static void write_string(FILE *f, const char *text)
{
    size_t n = strlen(text);
    uint8_t length[5];
    int used = 0;

    /* the length as BinaryWriter does: 7 bits at a time */
    do
    {
        length[used++] = (uint8_t) ((n & 0x7f) | (n > 0x7f ? 0x80 : 0));
        n >>= 7;
    } while (n > 0);
    fwrite(length, 1, (size_t) used, f);
    fwrite(text, 1, strlen(text), f);
}

static void account_save(const Account *account)
{
    char path[160];
    FILE *f;
    int i;

    mkdir(SANDBOX_ACCOUNT_DIR, 0777);
    account_path(account->name, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) return;
    for (i = 0; i < account->count; i++)
    {
        const Character *c = &account->characters[i];
        uint8_t byte;

        write_u32(f, (uint32_t) c->build);
        byte = c->char_class; fwrite(&byte, 1, 1, f);
        write_u32(f, c->display_id);
        byte = c->face; fwrite(&byte, 1, 1, f);
        byte = c->facial_hair; fwrite(&byte, 1, 1, f);
        byte = c->gender; fwrite(&byte, 1, 1, f);
        fwrite(&c->guid, 8, 1, f);
        byte = c->hair_color; fwrite(&byte, 1, 1, f);
        byte = c->hair_style; fwrite(&byte, 1, 1, f);
        fwrite(&c->x, 4, 1, f);
        fwrite(&c->y, 4, 1, f);
        fwrite(&c->z, 4, 1, f);
        fwrite(&c->o, 4, 1, f);
        write_u32(f, c->map);
        write_string(f, c->name);
        byte = c->power_type; fwrite(&byte, 1, 1, f);
        byte = c->race; fwrite(&byte, 1, 1, f);
        byte = c->skin; fwrite(&byte, 1, 1, f);
        write_u32(f, c->zone);
        fwrite(&c->scale, 4, 1, f);
        byte = c->is_flying; fwrite(&byte, 1, 1, f);
    }
    fclose(f);
}

static int read_all(FILE *f, void *out, size_t n)
{
    return fread(out, 1, n, f) == n;
}

static void account_load(Account *account)
{
    char path[160];
    FILE *f;

    account->count = 0;
    account->active = -1;
    account_path(account->name, path, sizeof(path));
    f = fopen(path, "rb");
    if (f == NULL) return;
    while (account->count < MAX_CHARACTERS)
    {
        Character *c = &account->characters[account->count];
        uint8_t byte, length_byte;
        uint32_t length = 0;
        int shift = 0;

        memset(c, 0, sizeof(*c));
        character_defaults(c);
        if (!read_all(f, &c->build, 4)) break;
        if (!read_all(f, &c->char_class, 1) || !read_all(f, &c->display_id, 4) || !read_all(f, &c->face, 1) || !read_all(f, &c->facial_hair, 1) ||
            !read_all(f, &c->gender, 1) || !read_all(f, &c->guid, 8) || !read_all(f, &c->hair_color, 1) || !read_all(f, &c->hair_style, 1) ||
            !read_all(f, &c->x, 4) || !read_all(f, &c->y, 4) || !read_all(f, &c->z, 4) || !read_all(f, &c->o, 4) || !read_all(f, &c->map, 4))
            break;
        do
        {
            if (!read_all(f, &length_byte, 1)) length_byte = 0;
            length |= (uint32_t) (length_byte & 0x7f) << shift;
            shift += 7;
        } while ((length_byte & 0x80) && shift < 35);
        if (length >= sizeof(c->name) || !read_all(f, c->name, length)) break;
        c->name[length] = 0;
        if (!read_all(f, &c->power_type, 1) || !read_all(f, &c->race, 1) || !read_all(f, &c->skin, 1) || !read_all(f, &c->zone, 4) ||
            !read_all(f, &c->scale, 4) || !read_all(f, &byte, 1))
            break;
        c->is_flying = byte;
        account->count++;
    }
    fclose(f);
}

static Character *account_find(Account *account, uint64_t guid)
{
    int i;

    for (i = 0; i < account->count; i++)
    {
        if (account->characters[i].guid == guid && account->characters[i].build == SANDBOX_BUILD) return &account->characters[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the world: connections and the encryption of the headers (build 9614 and later: RC4 with keys from HMAC-SHA1 of the session key,
   the first 1024 bytes of both streams dropped; only the headers are encrypted) */

typedef struct {
    uint8_t s[256];
    uint8_t i, j;
} Rc4;

static void rc4_init(Rc4 *r, const uint8_t *key, size_t length)
{
    int i, j = 0;

    for (i = 0; i < 256; i++) r->s[i] = (uint8_t) i;
    for (i = 0; i < 256; i++)
    {
        uint8_t t = r->s[i];

        j = (j + r->s[i] + key[i % length]) & 255;
        r->s[i] = r->s[j];
        r->s[j] = t;
    }
    r->i = r->j = 0;
}

static void rc4_process(Rc4 *r, uint8_t *data, int count)
{
    int n;

    for (n = 0; n < count; n++)
    {
        uint8_t t;

        r->i = (uint8_t) (r->i + 1);
        r->j = (uint8_t) (r->j + r->s[r->i]);
        t = r->s[r->i]; r->s[r->i] = r->s[r->j]; r->s[r->j] = t;
        data[n] ^= r->s[(uint8_t) (r->s[r->i] + r->s[r->j])];
    }
}

static void hmac_sha1(uint8_t out[20], const uint8_t *key, size_t key_length, const uint8_t *data, size_t length)
{
    uint8_t pad[64], inner[20];
    Sha1 s;
    size_t i;

    memset(pad, 0x36, sizeof(pad));
    for (i = 0; i < key_length; i++) pad[i] ^= key[i];
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, data, length);
    sha1_final(&s, inner);
    memset(pad, 0x5C, sizeof(pad));
    for (i = 0; i < key_length; i++) pad[i] ^= key[i];
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
}

typedef struct {
    int socket;
    int crypt_on;
    uint8_t session_key[40];
    Rc4 encoder, decoder;
    Account account;
    int have_account;
    uint32_t last_save;
} World;

static void crypt_start(World *w)
{
    /* the keys of build 11643 and later (3.3.5a uses them): encoder = server to client */
    static const uint8_t encoder_key[16] = { 0xCC, 0x98, 0xAE, 0x04, 0xE8, 0x97, 0xEA, 0xCA, 0x12, 0xDD, 0xC0, 0x93, 0x42, 0x91, 0x53, 0x57 };
    static const uint8_t decoder_key[16] = { 0xC2, 0xB3, 0x72, 0x3C, 0xC6, 0xAE, 0xD9, 0xB5, 0x34, 0x3C, 0x53, 0xEE, 0x2F, 0x43, 0x67, 0xCE };
    uint8_t key[20], drop[0x400];

    hmac_sha1(key, encoder_key, 16, w->session_key, 40);
    rc4_init(&w->encoder, key, 20);
    hmac_sha1(key, decoder_key, 16, w->session_key, 40);
    rc4_init(&w->decoder, key, 20);
    memset(drop, 0, sizeof(drop));
    rc4_process(&w->encoder, drop, sizeof(drop));
    rc4_process(&w->decoder, drop, sizeof(drop));
    w->crypt_on = 1;
}

static void crypt_encode(World *w, uint8_t *data, int count)
{
    if (w->crypt_on) rc4_process(&w->encoder, data, count);
}

static void crypt_decode(World *w, uint8_t *data, int count)
{
    if (w->crypt_on) rc4_process(&w->decoder, data, count);
}

/* a packet to send: the header is made when it goes out */
static void packet_begin(Buffer *b, uint32_t opcode)
{
    memset(b, 0, sizeof(*b));
    buffer_u16(b, 0);
    buffer_u16(b, opcode);
}

static void world_send(World *w, Buffer *b)
{
    uint32_t size = (uint32_t) b->length - 2;

    b->data[0] = (uint8_t) (size >> 8);
    b->data[1] = (uint8_t) size;
    crypt_encode(w, b->data, 4);
    sb_write_all(w->socket, b->data, b->length);
    free(b->data);
    b->data = NULL;
}

/* the opcodes of the client 3.3.5a that the sandbox knows */
enum {
    CMSG_WORLD_TELEPORT = 0x08, CMSG_CHAR_CREATE = 0x36, CMSG_CHAR_ENUM = 0x37, CMSG_CHAR_DELETE = 0x38, SMSG_CHAR_CREATE = 0x3A,
    SMSG_CHAR_ENUM = 0x3B, SMSG_CHAR_DELETE = 0x3C, CMSG_PLAYER_LOGIN = 0x3D, SMSG_NEW_WORLD = 0x3E, SMSG_TRANSFER_PENDING = 0x3F,
    SMSG_LOGIN_SETTIMESPEED = 0x42, CMSG_LOGOUT_REQUEST = 0x4B, SMSG_LOGOUT_COMPLETE = 0x4D, CMSG_NAME_QUERY = 0x50,
    SMSG_NAME_QUERY_RESPONSE = 0x51, CMSG_MESSAGECHAT = 0x95, SMSG_MESSAGECHAT = 0x96, SMSG_UPDATE_OBJECT = 0xA9, CMSG_AREATRIGGER = 0xB4,
    MSG_MOVE_TELEPORT_ACK = 0xC7, SMSG_FORCE_SPEED_CHANGE = 0xE2, SMSG_FORCE_SWIM_SPEED_CHANGE = 0xE6, SMSG_TUTORIAL_FLAGS = 0xFD,
    CMSG_STANDSTATECHANGE = 0x101, SMSG_EMOTE = 0x103, CMSG_TEXT_EMOTE = 0x104, SMSG_TEXT_EMOTE = 0x105, SMSG_INITIAL_SPELLS = 0x12A,
    CMSG_QUERY_TIME = 0x1CE, CMSG_PING = 0x1DC, SMSG_PONG = 0x1DD, SMSG_AUTH_CHALLENGE = 0x1EC, CMSG_AUTH_SESSION = 0x1ED,
    SMSG_AUTH_RESPONSE = 0x1EE, CMSG_ZONEUPDATE = 0x1F4, SMSG_ACCOUNT_DATA_TIMES = 0x209, SMSG_LOGIN_VERIFY_WORLD = 0x236,
    SMSG_ADDON_INFO = 0x2EF, SMSG_MOVE_SET_CAN_FLY = 0x343, SMSG_MOVE_UNSET_CAN_FLY = 0x344, SMSG_FORCE_FLIGHT_SPEED_CHANGE = 0x381,
    SMSG_TIME_SYNC_REQ = 0x390
};

static int is_movement_opcode(uint32_t opcode)
{
    return (opcode >= 0xB5 && opcode <= 0xC3) || opcode == 0xC9 || opcode == 0xCA || opcode == 0xCB || opcode == 0xDA || opcode == 0xDB ||
           opcode == 0xEC || opcode == 0xED || opcode == 0xEE || opcode == 0x359 || opcode == 0x35A;
}

/* SMSG_UPDATE_OBJECT that creates the player in the world (the update fields of 3.3.5a) */
#define OBJECT_END 6
#define UNIT_END (OBJECT_END + 0x8E)
#define FIELD_MAX (UNIT_END + 0x49A)
#define FIELD_MASK_BLOCKS ((FIELD_MAX + 31) / 32)

typedef struct {
    uint32_t value[FIELD_MAX + 2];
    uint8_t mask[FIELD_MASK_BLOCKS * 4];
    uint8_t present[FIELD_MAX + 2];
} Fields;

static void field_set(Fields *f, int index, uint32_t value)
{
    f->value[index] = value;
    f->present[index] = 1;
    f->mask[index / 8] |= (uint8_t) (1 << (index % 8));
}

static void field_set64(Fields *f, int index, uint64_t value)
{
    field_set(f, index, (uint32_t) value);
    field_set(f, index + 1, (uint32_t) (value >> 32));
}

static uint32_t bytes4(uint32_t b1, uint32_t b2, uint32_t b3, uint32_t b4)
{
    return (b1 & 0xff) | ((b2 & 0xff) << 8) | ((b3 & 0xff) << 16) | ((b4 & 0xff) << 24);
}

static uint32_t float_bits(float f)
{
    uint32_t v;

    memcpy(&v, &f, 4);
    return v;
}

static void send_update(World *w, const Character *c)
{
    Buffer p;
    Fields *f = calloc(1, sizeof(*f));
    int i;

    packet_begin(&p, SMSG_UPDATE_OBJECT);
    buffer_u32(&p, 1);   /* number of transactions */
    buffer_u8(&p, 3);    /* CREATE_OBJECT2 */
    buffer_packed_guid(&p, c->guid);
    buffer_u8(&p, 4);    /* the object is a player */
    buffer_u16(&p, 0x71);   /* update flags: self, all, living, has position */
    buffer_u32(&p, 0);   /* movement flags */
    buffer_u16(&p, 0);
    buffer_u32(&p, sb_tick_ms());
    buffer_float(&p, c->x);
    buffer_float(&p, c->y);
    buffer_float(&p, c->z);
    buffer_float(&p, c->o);
    buffer_u32(&p, 0);   /* fall time */
    buffer_float(&p, 2.5f);      /* walk */
    buffer_float(&p, 7.0f);      /* run */
    buffer_float(&p, 4.5f);      /* run backwards */
    buffer_float(&p, 4.7222f);   /* swim */
    buffer_float(&p, 2.5f);      /* swim backwards */
    buffer_float(&p, 7.0f);      /* fly */
    buffer_float(&p, 4.5f);      /* fly backwards */
    buffer_float(&p, 3.14f);     /* turn */
    buffer_float(&p, 7.0f);      /* pitch rate */
    buffer_u32(&p, 0);

    field_set64(f, 0, c->guid);                                   /* OBJECT_FIELD_GUID */
    field_set(f, 2, 0x19);                                        /* OBJECT_FIELD_TYPE */
    field_set(f, 3, 0);                                           /* OBJECT_FIELD_ENTRY */
    field_set(f, 4, float_bits(c->scale));                        /* OBJECT_FIELD_SCALE_X */
    field_set(f, 5, 0);                                           /* OBJECT_FIELD_PADDING */
    field_set64(f, OBJECT_END + 0x0C, 0);                         /* UNIT_FIELD_TARGET */
    field_set(f, OBJECT_END + 0x11, bytes4(c->race, c->char_class, c->gender, c->power_type));   /* UNIT_FIELD_BYTES_0 */
    field_set(f, OBJECT_END + 0x12, c->health);                   /* UNIT_FIELD_HEALTH */
    field_set(f, OBJECT_END + 0x14, 0);                           /* UNIT_FIELD_POWER2 */
    field_set(f, OBJECT_END + 0x1A, c->health);                   /* UNIT_FIELD_MAXHEALTH */
    field_set(f, OBJECT_END + 0x1C, c->rage);                     /* UNIT_FIELD_MAXPOWER2 */
    field_set(f, OBJECT_END + 0x30, c->level);                    /* UNIT_FIELD_LEVEL */
    field_set(f, OBJECT_END + 0x31, character_faction(c));        /* UNIT_FIELD_FACTIONTEMPLATE */
    field_set(f, OBJECT_END + 0x35, 8);                           /* UNIT_FIELD_FLAGS */
    field_set(f, OBJECT_END + 0x36, 0x800);                       /* UNIT_FIELD_FLAGS_2 */
    field_set(f, OBJECT_END + 0x3D, c->display_id);               /* UNIT_FIELD_DISPLAYID */
    field_set(f, OBJECT_END + 0x3E, character_display_id(c));     /* UNIT_FIELD_NATIVEDISPLAYID */
    field_set(f, OBJECT_END + 0x3F, c->mount_display_id);         /* UNIT_FIELD_MOUNTDISPLAYID */
    field_set(f, OBJECT_END + 0x44, bytes4(c->stand_state, 0, 0, 0));   /* UNIT_FIELD_BYTES_1 */
    field_set(f, OBJECT_END + 0x4E, c->strength);                 /* UNIT_FIELD_STAT0..4 */
    field_set(f, OBJECT_END + 0x4F, c->agility);
    field_set(f, OBJECT_END + 0x50, c->stamina);
    field_set(f, OBJECT_END + 0x51, c->intellect);
    field_set(f, OBJECT_END + 0x52, c->spirit);
    field_set(f, OBJECT_END + 0x72, c->mana);                     /* UNIT_FIELD_BASE_MANA */
    field_set(f, OBJECT_END + 0x74, 10240);                       /* UNIT_FIELD_BYTES_2 */
    field_set(f, OBJECT_END + 0x75, 1);                           /* UNIT_FIELD_ATTACK_POWER */
    field_set(f, OBJECT_END + 0x76, 0);
    field_set(f, OBJECT_END + 0x78, 1);                           /* UNIT_FIELD_RANGED_ATTACK_POWER */
    field_set(f, OBJECT_END + 0x79, 0);
    field_set(f, UNIT_END + 0x02, 0);                             /* PLAYER_FLAGS */
    field_set(f, UNIT_END + 0x05, bytes4(c->skin, c->face, c->hair_style, c->hair_color));   /* PLAYER_BYTES */
    field_set(f, UNIT_END + 0x06, bytes4(c->facial_hair, 0, 0, c->rested_state));            /* PLAYER_BYTES_2 */
    field_set(f, UNIT_END + 0x07, bytes4(c->gender, 0, 0, 0));    /* PLAYER_BYTES_3 */
    field_set(f, UNIT_END + 0x1E6, 47);                           /* PLAYER_XP */
    field_set(f, UNIT_END + 0x1E7, 200);                          /* PLAYER_NEXT_LEVEL_XP */
    /* the languages, so that the chat works: PLAYER_SKILL_INFO_1_1 holds id, value (current, maximum) and bonus of each skill */
    field_set(f, UNIT_END + 0x1E8, 98);                           /* Common */
    field_set(f, UNIT_END + 0x1E9, (300u | (300u << 16)));
    field_set(f, UNIT_END + 0x1EA, 0);
    field_set(f, UNIT_END + 0x1EB, 109);                          /* Orcish */
    field_set(f, UNIT_END + 0x1EC, (300u | (300u << 16)));
    field_set(f, UNIT_END + 0x1ED, 0);
    for (i = 0; i < 0x80; i++) field_set(f, UNIT_END + 0x37D + i, 0xFFFFFFFFu);   /* PLAYER_EXPLORED_ZONES_1 */

    buffer_u8(&p, FIELD_MASK_BLOCKS);
    buffer_bytes(&p, f->mask, sizeof(f->mask));
    for (i = 0; i < FIELD_MAX + 2; i++)
    {
        if (f->present[i]) buffer_u32(&p, f->value[i]);
    }
    free(f);
    world_send(w, &p);
}

/* SMSG_MESSAGECHAT as the server sends it (3.3.5a): type, language, sender, flags, receiver, text, tag */
static void send_chat(World *w, uint32_t type, uint64_t sender, const char *text)
{
    Buffer p;

    packet_begin(&p, SMSG_MESSAGECHAT);
    buffer_u8(&p, type);
    buffer_u32(&p, 0);
    buffer_u64(&p, sender);
    buffer_u32(&p, 0);
    buffer_u64(&p, 0);
    buffer_u32(&p, (uint32_t) strlen(text) + 1);
    buffer_string(&p, text);
    buffer_u8(&p, 0);
    world_send(w, &p);
}

static void send_fly(World *w, Character *c, int on)
{
    Buffer p;

    c->is_flying = (uint8_t) on;
    packet_begin(&p, on ? SMSG_MOVE_SET_CAN_FLY : SMSG_MOVE_UNSET_CAN_FLY);
    buffer_packed_guid(&p, c->guid);
    buffer_u32(&p, 2);
    world_send(w, &p);
}

/* SMSG_MESSAGECHAT of the system (type 0) */
static void send_system_message(World *w, const char *text)
{
    send_chat(w, 0, 0, text);
}

static void character_teleport(World *w, Character *c, float x, float y, float z, float o, uint32_t map)
{
    uint32_t old_map = c->map;

    c->teleporting = 1;
    if (c->map == map)
    {
        Buffer p;

        packet_begin(&p, MSG_MOVE_TELEPORT_ACK);
        buffer_packed_guid(&p, c->guid);
        buffer_u64(&p, 0);
        buffer_u16(&p, 0);
        buffer_u32(&p, 0);
        buffer_float(&p, x);
        buffer_float(&p, y);
        buffer_float(&p, z);
        buffer_float(&p, o);
        buffer_float(&p, 0);
        world_send(w, &p);
    }
    else
    {
        Buffer p;

        packet_begin(&p, SMSG_TRANSFER_PENDING);
        buffer_u32(&p, map);
        world_send(w, &p);
        packet_begin(&p, SMSG_NEW_WORLD);
        buffer_u32(&p, map);
        buffer_float(&p, x);
        buffer_float(&p, y);
        buffer_float(&p, z);
        buffer_float(&p, o);
        world_send(w, &p);
        c->is_flying = 0;
    }
    usleep(150000);   /* time for the packets that are on their way */
    c->x = x; c->y = y; c->z = z; c->o = o; c->map = map;
    send_update(w, c);
    send_fly(w, c, c->is_flying);
    if (c->map != old_map)
    {
        Buffer p;

        packet_begin(&p, SMSG_TIME_SYNC_REQ);
        buffer_u32(&p, 0);
        world_send(w, &p);
    }
    c->teleporting = 0;
}

/* kind: 0 run, 1 swim, 2 fly */
static void send_force_speed(World *w, const Character *c, float modifier, int kind)
{
    Buffer p;

    packet_begin(&p, kind == 1 ? SMSG_FORCE_SWIM_SPEED_CHANGE : (kind == 2 ? SMSG_FORCE_FLIGHT_SPEED_CHANGE : SMSG_FORCE_SPEED_CHANGE));
    buffer_packed_guid(&p, c->guid);
    buffer_u32(&p, 0);
    if (kind == 0) buffer_u8(&p, 0);
    buffer_float(&p, modifier * 7.0f);
    world_send(w, &p);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the port's own commands (.zlib .sched .prof .gfx .log): WoW-portcmd.c */
int WoW_PortCommand(const char *command, int count, char **arguments, void (*reply)(void *context, const char *text), void *context);

/* the chat commands (.help .gps .go .nudge .speed .morph .demorph .gfx) */

/* the text without the characters that AIO Sandbox ignores when it looks for a place */
static void sanitize(const char *in, char *out, size_t size)
{
    size_t n = 0;

    for (; *in != 0 && n + 1 < size; in++)
    {
        if (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n' || *in == '\'' || *in == ',') continue;
        out[n++] = *in;
    }
    out[n] = 0;
}

static int contains_nocase(const char *text, const char *needle)
{
    size_t n = strlen(needle), i, length = strlen(text);

    if (n == 0) return 1;
    for (i = 0; i + n <= length; i++)
    {
        if (strncasecmp(text + i, needle, n) == 0) return 1;
    }
    return 0;
}

static int parse_float(const char *text, float *value)
{
    char *end;
    double d = strtod(text, &end);

    if (end == text || *end != 0) return 0;
    *value = (float) d;
    return 1;
}

static int parse_uint(const char *text, uint32_t *value)
{
    char *end;
    unsigned long v = strtoul(text, &end, 10);

    if (end == text || *end != 0 || text[0] == '-') return 0;
    *value = (uint32_t) v;
    return 1;
}

static void go_named(World *w, Character *c, int triggers, const char *name)
{
    const SandboxPlace *matches[SANDBOX_WORLDPORT_COUNT];
    int count = 0, i, exact = -1;
    char needle[128], formatted[160];

    sanitize(name, needle, sizeof(needle));
    if (!triggers)
    {
        for (i = 0; i < SANDBOX_WORLDPORT_COUNT; i++)
        {
            const SandboxPlace *place = &sandbox_worldports[i];

            sanitize(place->description, formatted, sizeof(formatted));
            if (strlen(needle) > 0 && !contains_nocase(formatted, needle)) continue;
            if (strcasecmp(formatted, needle) == 0 && exact < 0) exact = count;
            matches[count++] = place;
        }
    }
    else
    {
        for (i = 0; i < SANDBOX_TRIGGER_COUNT; i++)
        {
            const SandboxPlace *place = &sandbox_triggers[i].place;

            if (place->map == 0 || place->map == 1 || place->map == 530 || place->map == 571) continue;   /* continents are not instances */
            sanitize(place->description, formatted, sizeof(formatted));
            if (strlen(needle) > 0 && !contains_nocase(formatted, needle)) continue;
            if (strcasecmp(formatted, needle) == 0 && exact < 0) exact = count;
            matches[count++] = place;
        }
    }
    if (exact >= 0)
    {
        matches[0] = matches[exact];
        count = 1;
    }
    if (count == 0)
    {
        send_system_message(w, "No matching locations found");
    }
    else if (count == 1)
    {
        character_teleport(w, c, matches[0]->x, matches[0]->y, matches[0]->z, matches[0]->o, matches[0]->map);
    }
    else
    {
        char line[200];

        send_system_message(w, "Multiple matches:");
        for (i = 0; i < count; i++)
        {
            snprintf(line, sizeof(line), " %s", matches[i]->description);
            send_system_message(w, line);
        }
    }
}

static void command_help(World *w)
{
    static const char *lines[] = {
        "Commands: ", "    .demorph", "    .fly {on | off}", "    .go instance {name | id}", "    .go {name}",
        "    .go {x} {y} {z} Optional: {mapid}", "    .gps", "    .morph {id}", "    .nudge Optional: [0 - 100] {z offset}",
        "    .prof Optional: {on | off}",
        "    .zlib Optional: {client | native | check}",
        "    .sched Optional: {game | fair} Optional: {budget in ms} Optional: {minimum slice in ms}",
        "    .speed [0.1 - 1000] Optional: {run | swim | fly | all} ",
#ifdef SRW_DIAGNOSTICS
        "    .gfx {off | census | vertexalpha | tex0alpha | tex1alpha | alpha | redalpha | halfred | hide {state} | hidefvf {hex} | halfpixel {0|1} | <mode>}",
        "    .log Optional: {category | all | off} Optional: {on | off}"
#endif
    };
    size_t i;

    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) send_system_message(w, lines[i]);
}

/* returns 1 when the text was a command */
static void sandbox_reply(void *context, const char *text)
{
    send_system_message((World *) context, text);
}

static int run_command(World *w, Character *c, const char *message)
{
    char copy[512], *args[16], *token, *save;
    int count = 0, i;
    char command[64], reply[200], **arguments;

    if (message[0] != '.') return 0;
#ifdef SRW_DIAGNOSTICS
    sb_log("WoW sandbox: chat command \"%s\"", message);
#endif
    snprintf(copy, sizeof(copy), "%s", message + 1);
    for (token = strtok_r(copy, " ", &save); token != NULL && count < 16; token = strtok_r(NULL, " ", &save)) args[count++] = token;
    if (count == 0) return 0;
    snprintf(command, sizeof(command), "%s", args[0]);
    for (i = 1; i < count; i++)
    {
        char *p;

        for (p = args[i]; *p != 0; p++) *p = (char) tolower((unsigned char) *p);
    }
    arguments = args + 1;   /* the arguments follow the name of the command */
    count--;

    if (strcasecmp(command, "gps") == 0)
    {
        snprintf(reply, sizeof(reply), "X: %g, Y: %g, Z: %g, O: %g, Map: %u", (double) c->x, (double) c->y, (double) c->z, (double) c->o, (unsigned int) c->map);
        send_system_message(w, reply);
    }
    else if (strcasecmp(command, "help") == 0)
    {
        command_help(w);
    }
    else if (strcasecmp(command, "speed") == 0)
    {
        float speed;
        const char *type = count > 1 ? arguments[1] : "all";
        char shown[8];

        if (count < 1 || !parse_float(arguments[0], &speed)) return 1;
        if (speed < 0.1f) speed = 0.1f;
        if (speed > 1000.0f) speed = 1000.0f;
        if (strcmp(type, "swim") == 0) send_force_speed(w, c, speed, 1);
        else if (strcmp(type, "run") == 0) send_force_speed(w, c, speed, 0);
        else if (strcmp(type, "fly") == 0) send_force_speed(w, c, speed, 2);
        else if (strcmp(type, "all") == 0) { send_force_speed(w, c, speed, 0); send_force_speed(w, c, speed, 1); send_force_speed(w, c, speed, 2); }
        else return 1;
        snprintf(shown, sizeof(shown), "%s", type);
        shown[0] = (char) toupper((unsigned char) shown[0]);
        snprintf(reply, sizeof(reply), "%s speed changed to %gx normal", shown, (double) speed);
        send_system_message(w, reply);
    }
    else if (strcasecmp(command, "go") == 0)
    {
        float x, y, z;

        if (count == 0) return 1;
        if (strcmp(arguments[0], "instance") == 0)
        {
            uint32_t id;

            if (count < 2) return 1;
            if (parse_uint(arguments[1], &id))
            {
                int found = -1;

                for (i = 0; i < SANDBOX_TRIGGER_COUNT; i++)
                {
                    if (sandbox_triggers[i].id == id) found = i;
                }
                if (found >= 0)
                {
                    const SandboxPlace *place = &sandbox_triggers[found].place;

                    character_teleport(w, c, place->x, place->y, place->z, place->o, place->map);
                }
                else
                {
                    snprintf(reply, sizeof(reply), "Area Id %u does not exist", (unsigned int) id);
                    send_system_message(w, reply);
                }
            }
            else
            {
                char name[200] = "";

                for (i = 1; i < count; i++)
                {
                    if (i > 1) strncat(name, " ", sizeof(name) - strlen(name) - 1);
                    strncat(name, arguments[i], sizeof(name) - strlen(name) - 1);
                }
                go_named(w, c, 1, name);
            }
        }
        else if (parse_float(arguments[0], &x))
        {
            uint32_t map = c->map;

            if (count < 3 || count > 4) return 1;
            if (!parse_float(arguments[1], &y) || !parse_float(arguments[2], &z)) return 1;
            if (count > 3 && !parse_uint(arguments[3], &map)) return 1;
            character_teleport(w, c, x, y, z, c->o, map);
        }
        else
        {
            char name[200] = "";

            for (i = 0; i < count; i++)
            {
                if (i > 0) strncat(name, " ", sizeof(name) - strlen(name) - 1);
                strncat(name, arguments[i], sizeof(name) - strlen(name) - 1);
            }
            go_named(w, c, 0, name);
        }
    }
    else if (strcasecmp(command, "nudge") == 0)
    {
        float force = 1, z = 0;

        if (count > 0)
        {
            if (!parse_float(arguments[0], &force)) return 1;
            force = (float) (int) force;
            if (force < 0) force = 0;
            if (force > 100) force = 100;
        }
        if (count > 1) parse_float(arguments[1], &z);
        character_teleport(w, c, c->x + cosf(c->o) * force, c->y + sinf(c->o) * force, c->z + z, c->o, c->map);
    }
    else if (strcasecmp(command, "morph") == 0)
    {
        uint32_t id;

        if (count < 1 || !parse_uint(arguments[0], &id)) return 1;
        c->display_id = id;
        send_update(w, c);
    }
    else if (strcasecmp(command, "demorph") == 0)
    {
        c->display_id = character_display_id(c);
        send_update(w, c);
    }
    else if (WoW_PortCommand(command, count, arguments, sandbox_reply, w))
    {
        /* the port's own commands (.zlib .sched .prof, .gfx .log in the diagnostics build): WoW-portcmd.c */
    }
    else if (strcasecmp(command, "fly") == 0)
    {
        if (count > 0 && (strcmp(arguments[0], "on") == 0 || strcmp(arguments[0], "off") == 0))
        {
            send_fly(w, c, strcmp(arguments[0], "on") == 0);
            send_system_message(w, c->is_flying ? "Flying enabled" : "Flying disabled");
        }
    }
    else
    {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the handlers of the world server */

static void upper_first(char *text)
{
    size_t i;

    for (i = 0; text[i] != 0; i++) text[i] = (char) (i == 0 ? toupper((unsigned char) text[i]) : tolower((unsigned char) text[i]));
}

static int game_time(void)
{
    time_t now = time(NULL);
    struct tm t;

    localtime_r(&now, &t);
    return t.tm_min + (t.tm_hour << 6) + (t.tm_wday << 11) + ((t.tm_mday - 1) << 14) + (t.tm_mon << 20) + ((t.tm_year - 100) << 24);
}

static void send_time(World *w)
{
    Buffer p;

    packet_begin(&p, SMSG_LOGIN_SETTIMESPEED);
    buffer_u32(&p, (uint32_t) game_time());
    buffer_float(&p, 0.01666667f);
    world_send(w, &p);
}

static Character *active_character(World *w)
{
    return (w->have_account && w->account.active >= 0) ? &w->account.characters[w->account.active] : NULL;
}

static void save_account(World *w)
{
    if (w->have_account) account_save(&w->account);
    w->last_save = sb_tick_ms();
}

static const uint8_t addon_public_key[256] = {
    0xC3, 0x5B, 0x50, 0x84, 0xB9, 0x3E, 0x32, 0x42, 0x8C, 0xD0, 0xC7, 0x48, 0xFA, 0x0E, 0x5D, 0x54,
    0x5A, 0xA3, 0x0E, 0x14, 0xBA, 0x9E, 0x0D, 0xB9, 0x5D, 0x8B, 0xEE, 0xB6, 0x84, 0x93, 0x45, 0x75,
    0xFF, 0x31, 0xFE, 0x2F, 0x64, 0x3F, 0x3D, 0x6D, 0x07, 0xD9, 0x44, 0x9B, 0x40, 0x85, 0x59, 0x34,
    0x4E, 0x10, 0xE1, 0xE7, 0x43, 0x69, 0xEF, 0x7C, 0x16, 0xFC, 0xB4, 0xED, 0x1B, 0x95, 0x28, 0xA8,
    0x23, 0x76, 0x51, 0x31, 0x57, 0x30, 0x2B, 0x79, 0x08, 0x50, 0x10, 0x1C, 0x4A, 0x1A, 0x2C, 0xC8,
    0x8B, 0x8F, 0x05, 0x2D, 0x22, 0x3D, 0xDB, 0x5A, 0x24, 0x7A, 0x0F, 0x13, 0x50, 0x37, 0x8F, 0x5A,
    0xCC, 0x9E, 0x04, 0x44, 0x0E, 0x87, 0x01, 0xD4, 0xA3, 0x15, 0x94, 0x16, 0x34, 0xC6, 0xC2, 0xC3,
    0xFB, 0x49, 0xFE, 0xE1, 0xF9, 0xDA, 0x8C, 0x50, 0x3C, 0xBE, 0x2C, 0xBB, 0x57, 0xED, 0x46, 0xB9,
    0xAD, 0x8B, 0xC6, 0xDF, 0x0E, 0xD6, 0x0F, 0xBE, 0x80, 0xB3, 0x8B, 0x1E, 0x77, 0xCF, 0xAD, 0x22,
    0xCF, 0xB7, 0x4B, 0xCF, 0xFB, 0xF0, 0x6B, 0x11, 0x45, 0x2D, 0x7A, 0x81, 0x18, 0xF2, 0x92, 0x7E,
    0x98, 0x56, 0x5D, 0x5E, 0x69, 0x72, 0x0A, 0x0D, 0x03, 0x0A, 0x85, 0xA2, 0x85, 0x9C, 0xCB, 0xFB,
    0x56, 0x6E, 0x8F, 0x44, 0xBB, 0x8F, 0x02, 0x22, 0x68, 0x63, 0x97, 0xBC, 0x85, 0xBA, 0xA8, 0xF7,
    0xB5, 0x40, 0x68, 0x3C, 0x77, 0x86, 0x6F, 0x4B, 0xD7, 0x88, 0xCA, 0x8A, 0xD7, 0xCE, 0x36, 0xF0,
    0x45, 0x6E, 0xD5, 0x64, 0x79, 0x0F, 0x17, 0xFC, 0x64, 0xDD, 0x10, 0x6F, 0xF3, 0xF5, 0xE0, 0xA6,
    0xC3, 0xFB, 0x1B, 0x8C, 0x29, 0xEF, 0x8E, 0xE5, 0x34, 0xCB, 0xD1, 0x2A, 0xCE, 0x79, 0xC3, 0x9A,
    0x0D, 0x36, 0xEA, 0x01, 0xE0, 0xAA, 0x91, 0x20, 0x54, 0xF0, 0x72, 0xD8, 0x1E, 0xC7, 0x89, 0xD2
};

/* SMSG_ADDON_INFO: an entry for every addon in the zlib compressed list of the client, then the banned addons (none) */
static void send_addon_info(World *w, Reader *r, uint32_t uncompressed_size)
{
    Buffer p;
    uLongf size = uncompressed_size;
    uint8_t *list;
    Reader a;
    uint32_t count, i;

    packet_begin(&p, SMSG_ADDON_INFO);
    if (uncompressed_size > 0 && uncompressed_size < 0x100000 && (list = malloc(uncompressed_size)) != NULL)
    {
        int z = uncompress(list, &size, r->data + r->position, (uLong) (r->length - r->position));

        if (z == Z_OK)
        {
            a.data = list; a.length = size; a.position = 0;
            count = reader_u32(&a);
            for (i = 0; i < count && a.position < a.length; i++)
            {
                char name[80];
                uint32_t crc;
                int requires_key;

                uint32_t enabled;

                reader_string(&a, name, sizeof(name));
                enabled = reader_u8(&a);
                crc = reader_u32(&a);
                reader_u32(&a);
                requires_key = crc != 0x1C776D01u && crc != 0x4C1C776Du;   /* the checksums of the official addons */
                buffer_u8(&p, 2);   /* from Blizzard */
                buffer_u8(&p, 1);   /* enabled */
                buffer_u8(&p, (uint32_t) requires_key);
                if (requires_key) buffer_bytes(&p, addon_public_key, sizeof(addon_public_key));
                buffer_u32(&p, 0);  /* no addon url file */
                buffer_u8(&p, 0);
            }
        }
        free(list);
    }
    buffer_u32(&p, 0);   /* banned addons */
    world_send(w, &p);
}

static void handle_auth_session(World *w, Reader *r)
{
    char name[64];
    Buffer p;
    uint32_t addon_size;

    reader_u64(r);   /* build, server id */
    reader_string(r, name, sizeof(name));
    upper_ascii(name);
    r->position += 4 + 4 + 12 + 8 + 20;   /* login type, client seed, region, battlegroup, realm, dos response, digest */
    addon_size = reader_u32(r);
    if (!find_session(name, w->session_key)) memset(w->session_key, 0, sizeof(w->session_key));
    crypt_start(w);

    snprintf(w->account.name, sizeof(w->account.name), "%s", name);
    account_load(&w->account);
    w->have_account = 1;
    sb_log("WoW sandbox: account %s logs in (%d characters)", name, w->account.count);

    send_addon_info(w, r, addon_size);

    packet_begin(&p, SMSG_AUTH_RESPONSE);
    buffer_u8(&p, 0x0C);   /* AUTH_OK */
    buffer_u32(&p, 0);
    buffer_u8(&p, 0);
    buffer_u32(&p, 0);
    buffer_u8(&p, 2);      /* expansion: Wrath of the Lich King */
    world_send(w, &p);
}

static void handle_char_enum(World *w)
{
    Buffer p;
    int i, count = 0, j;

    for (i = 0; i < w->account.count; i++) count += w->account.characters[i].build == SANDBOX_BUILD;
    packet_begin(&p, SMSG_CHAR_ENUM);
    buffer_u8(&p, (uint32_t) count);
    for (i = 0; i < w->account.count; i++)
    {
        const Character *c = &w->account.characters[i];

        if (c->build != SANDBOX_BUILD) continue;
        buffer_u64(&p, c->guid);
        buffer_string(&p, c->name);
        buffer_u8(&p, c->race);
        buffer_u8(&p, c->char_class);
        buffer_u8(&p, c->gender);
        buffer_u8(&p, c->skin);
        buffer_u8(&p, c->face);
        buffer_u8(&p, c->hair_style);
        buffer_u8(&p, c->hair_color);
        buffer_u8(&p, c->facial_hair);
        buffer_u8(&p, c->level);
        buffer_u32(&p, c->zone);
        buffer_u32(&p, c->map);
        buffer_float(&p, c->x);
        buffer_float(&p, c->y);
        buffer_float(&p, c->z);
        buffer_u32(&p, 0);   /* guild */
        buffer_u32(&p, 0);   /* flags */
        buffer_u32(&p, 0);   /* customization */
        buffer_u8(&p, 0);    /* first login */
        buffer_u32(&p, 0);   /* pet display id, level, family */
        buffer_u32(&p, 0);
        buffer_u32(&p, 0);
        for (j = 0; j < 0x17; j++)
        {
            buffer_u32(&p, 0);   /* display id */
            buffer_u8(&p, 0);    /* inventory type */
            buffer_u32(&p, 0);   /* enchantment */
        }
    }
    world_send(w, &p);
}

static void handle_char_create(World *w, Reader *r)
{
    Character c;
    Buffer p;
    int i;
    uint64_t highest = 0;

    memset(&c, 0, sizeof(c));
    character_defaults(&c);
    reader_string(r, c.name, sizeof(c.name));
    upper_first(c.name);
    c.build = SANDBOX_BUILD;
    c.race = (uint8_t) reader_u8(r);
    c.char_class = (uint8_t) reader_u8(r);
    c.gender = (uint8_t) reader_u8(r);
    c.skin = (uint8_t) reader_u8(r);
    c.face = (uint8_t) reader_u8(r);
    c.hair_style = (uint8_t) reader_u8(r);
    c.hair_color = (uint8_t) reader_u8(r);
    c.facial_hair = (uint8_t) reader_u8(r);

    packet_begin(&p, SMSG_CHAR_CREATE);
    for (i = 0; i < w->account.count; i++)
    {
        if (w->account.characters[i].guid > highest) highest = w->account.characters[i].guid;
        if (w->account.characters[i].build == SANDBOX_BUILD && strcasecmp(w->account.characters[i].name, c.name) == 0)
        {
            buffer_u8(&p, 0x32);   /* duplicate name */
            world_send(w, &p);
            return;
        }
    }
    if (w->account.count >= MAX_CHARACTERS)
    {
        buffer_u8(&p, 0x35);   /* the account is full */
        world_send(w, &p);
        return;
    }
    c.guid = highest + 1;
    c.x = -8949.95f; c.y = -132.493f; c.z = 83.5312f; c.o = 0; c.map = 0;
    c.rested_state = (uint8_t) (1 + rand() % 2);
    character_set_defaults(&c);
    w->account.characters[w->account.count++] = c;
    save_account(w);
    sb_log("WoW sandbox: character %s created", c.name);
    buffer_u8(&p, 0x2F);   /* success */
    world_send(w, &p);
}

static void handle_char_delete(World *w, Reader *r)
{
    uint64_t guid = reader_u64(r);
    Character *c = account_find(&w->account, guid);
    Buffer p;

    packet_begin(&p, SMSG_CHAR_DELETE);
    buffer_u8(&p, 0x47);
    world_send(w, &p);
    if (c != NULL)
    {
        int index = (int) (c - w->account.characters);

        memmove(&w->account.characters[index], &w->account.characters[index + 1], (size_t) (w->account.count - index - 1) * sizeof(Character));
        w->account.count--;
        w->account.active = -1;
        save_account(w);
    }
}

static void handle_player_login(World *w, Reader *r)
{
    uint64_t guid = reader_u64(r);
    Character *c = account_find(&w->account, guid);
    Buffer p;
    int i;

    if (c == NULL) return;
    w->account.active = (int) (c - w->account.characters);
    c->display_id = character_display_id(c);

    packet_begin(&p, SMSG_LOGIN_VERIFY_WORLD);
    buffer_u32(&p, c->map);
    buffer_float(&p, c->x);
    buffer_float(&p, c->y);
    buffer_float(&p, c->z);
    buffer_float(&p, c->o);
    world_send(w, &p);

    packet_begin(&p, SMSG_ACCOUNT_DATA_TIMES);
    buffer_u32(&p, (uint32_t) time(NULL));
    buffer_u8(&p, 1);
    buffer_u32(&p, 0xFF);   /* all eight account data types */
    for (i = 0; i < 8; i++) buffer_u32(&p, 0);
    world_send(w, &p);

    packet_begin(&p, SMSG_TUTORIAL_FLAGS);
    for (i = 0; i < 8; i++) buffer_u32(&p, 0xFFFFFFFFu);
    world_send(w, &p);

    /* the language spells, so that the chat works */
    packet_begin(&p, SMSG_INITIAL_SPELLS);
    buffer_u8(&p, 0);
    buffer_u16(&p, 2);
    buffer_u32(&p, 668);
    buffer_u16(&p, 0);
    buffer_u32(&p, 669);
    buffer_u16(&p, 0);
    buffer_u16(&p, 0);   /* no cooldowns */
    world_send(w, &p);

    send_time(w);
    send_update(w, c);
    send_fly(w, c, c->is_flying);

    packet_begin(&p, SMSG_TIME_SYNC_REQ);
    buffer_u32(&p, 0);
    world_send(w, &p);
    sb_log("WoW sandbox: %s enters the world", c->name);
    WoW_SandboxInWorld = 1;
}

static void handle_message_chat(World *w, Reader *r)
{
    Character *c = active_character(w);
    char message[400];
    uint32_t type = reader_u32(r);

    if (c == NULL) return;
    reader_u32(r);   /* the language */
    reader_string(r, message, sizeof(message));
    if (run_command(w, c, message)) return;
    send_chat(w, type, c->guid, message);
}

static void handle_movement(World *w, uint32_t opcode, const uint8_t *payload, size_t length)
{
    Character *c = active_character(w);
    Reader r = { payload, length, 0 };
    Buffer p;

    (void) opcode;
    (void) p;
    if (c == NULL || c->teleporting) return;
    {
        uint8_t mask = (uint8_t) reader_u8(&r);
        int bit;

        for (bit = 0; bit < 8; bit++) if (mask & (1 << bit)) reader_u8(&r);   /* the packed guid */
    }
    r.position += 4 + 2 + 4;   /* flags, flags 2, time */
    c->x = reader_float(&r);
    c->y = reader_float(&r);
    c->z = reader_float(&r);
    c->o = reader_float(&r);
}

static void handle_name_query(World *w, Reader *r)
{
    uint64_t guid = reader_u64(r);
    Character *c = account_find(&w->account, guid);
    Buffer p;

    if (c == NULL) return;
    packet_begin(&p, SMSG_NAME_QUERY_RESPONSE);
    buffer_packed_guid(&p, guid);
    buffer_u8(&p, 0);   /* found */
    buffer_string(&p, c->name);
    buffer_u8(&p, 0);   /* realm name */
    buffer_u8(&p, c->race);
    buffer_u8(&p, c->gender);
    buffer_u8(&p, c->char_class);
    buffer_u8(&p, 0);   /* not declined */
    world_send(w, &p);
}

static void handle_text_emote(World *w, Reader *r)
{
    Character *c = active_character(w);
    uint32_t emote = reader_u32(r), number = reader_u32(r), animation = 0;
    Buffer p;
    size_t i;

    reader_u64(r);
    if (c == NULL) return;
    for (i = 0; i < SANDBOX_EMOTE_COUNT; i++)
    {
        if (sandbox_emotes[i].text_emote == emote) animation = sandbox_emotes[i].animation;
    }
    packet_begin(&p, SMSG_TEXT_EMOTE);
    buffer_u64(&p, c->guid);
    buffer_u32(&p, emote);
    buffer_u32(&p, number);
    buffer_u32(&p, 0);
    buffer_u8(&p, 0);
    world_send(w, &p);

    if (emote == SANDBOX_EMOTE_SIT || emote == SANDBOX_EMOTE_STAND || emote == SANDBOX_EMOTE_SLEEP || emote == SANDBOX_EMOTE_KNEEL)
    {
        c->stand_state = emote == SANDBOX_EMOTE_SIT ? 1 : (emote == SANDBOX_EMOTE_STAND ? 0 : (emote == SANDBOX_EMOTE_SLEEP ? 3 : 8));
        send_update(w, c);
        return;
    }
    if (animation > 0)
    {
        packet_begin(&p, SMSG_EMOTE);
        buffer_u32(&p, animation);
        buffer_u64(&p, c->guid);
        world_send(w, &p);
    }
}

static void world_packet(World *w, uint32_t opcode, const uint8_t *payload, size_t length)
{
    Reader r = { payload, length, 0 };
    Character *c = active_character(w);
    Buffer p;

    if (is_movement_opcode(opcode))
    {
        handle_movement(w, opcode, payload, length);
        return;
    }
    switch (opcode)
    {
    case CMSG_AUTH_SESSION:
        handle_auth_session(w, &r);
        break;
    case CMSG_LOGOUT_REQUEST:
        if (c != NULL)
        {
            packet_begin(&p, SMSG_LOGOUT_COMPLETE);
            world_send(w, &p);
            w->account.active = -1;
            save_account(w);
        }
        break;
    case CMSG_CHAR_ENUM:
        handle_char_enum(w);
        break;
    case CMSG_CHAR_CREATE:
        handle_char_create(w, &r);
        break;
    case CMSG_CHAR_DELETE:
        handle_char_delete(w, &r);
        break;
    case CMSG_PING:
        packet_begin(&p, SMSG_PONG);
        buffer_u32(&p, reader_u32(&r));
        world_send(w, &p);
        if (sb_tick_ms() - w->last_save > 45000) save_account(w);
        break;
    case CMSG_PLAYER_LOGIN:
        handle_player_login(w, &r);
        break;
    case CMSG_QUERY_TIME:
        send_time(w);
        break;
    case CMSG_NAME_QUERY:
        handle_name_query(w, &r);
        break;
    case CMSG_AREATRIGGER:
        if (c != NULL)
        {
            uint32_t id = reader_u32(&r);
            size_t i;

            for (i = 0; i < SANDBOX_TRIGGER_COUNT; i++)
            {
                if (sandbox_triggers[i].id == id)
                {
                    const SandboxPlace *place = &sandbox_triggers[i].place;

                    character_teleport(w, c, place->x, place->y, place->z, place->o, place->map);
                    break;
                }
            }
        }
        break;
    case CMSG_ZONEUPDATE:
        if (c != NULL) c->zone = reader_u32(&r);
        break;
    case CMSG_MESSAGECHAT:
        handle_message_chat(w, &r);
        break;
    case CMSG_TEXT_EMOTE:
        handle_text_emote(w, &r);
        break;
    case CMSG_STANDSTATECHANGE:
        if (c != NULL)
        {
            c->stand_state = reader_u32(&r);
            send_update(w, c);
        }
        break;
    default:
        {
            static int ignored;

            if (ignored++ < 60) sb_log("WoW sandbox: ignored opcode %x (%u bytes)", (unsigned int) opcode, (unsigned int) length);
        }
        break;   /* everything else the client sends is ignored */
    }
}

static void *world_connection_main(void *argument)
{
    World *w = calloc(1, sizeof(*w));
    Buffer p;

    w->socket = (int) (intptr_t) argument;
    w->account.active = -1;
    w->last_save = sb_tick_ms();

    packet_begin(&p, SMSG_AUTH_CHALLENGE);
    buffer_u32(&p, 1);
    buffer_u32(&p, 0);   /* seed */
    for (int i = 0; i < 32; i++) buffer_u8(&p, 0);
    world_send(w, &p);

    for (;;)
    {
        uint8_t header[6], *payload;
        uint32_t size, opcode;

        if (!sb_read_exact(w->socket, header, 6)) break;
        crypt_decode(w, header, 6);
        size = ((uint32_t) header[0] << 8) | header[1];
        opcode = header[2] | ((uint32_t) header[3] << 8) | ((uint32_t) header[4] << 16) | ((uint32_t) header[5] << 24);
        if (size < 4 || size > 0x4000) break;
        payload = malloc(size);
        if (size > 4 && !sb_read_exact(w->socket, payload, size - 4))
        {
            free(payload);
            break;
        }
        world_packet(w, opcode, payload, size - 4);
        free(payload);
    }
    save_account(w);
    if (w->have_account) sb_log("WoW sandbox: account %s left", w->account.name);
    sb_close(w->socket);
    free(w);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the realm server: logon, realm list */

static void *realm_connection_main(void *argument)
{
    int s = (int) (intptr_t) argument;
    RealmState *state = calloc(1, sizeof(*state));

    for (;;)
    {
        uint8_t command, packet[300], out[300];
        size_t length = 0;

        if (!sb_read_exact(s, &command, 1)) break;
        packet[0] = command;
        if (command == 0 || command == 2)
        {
            /* LOGON_CHALLENGE / RECONNECT_CHALLENGE: cmd, error, size (16 bits), then the size bytes */
            uint32_t size;

            if (!sb_read_exact(s, packet + 1, 3)) break;
            size = packet[2] | ((uint32_t) packet[3] << 8);
            if (size < 30 || size > 200 || !sb_read_exact(s, packet + 4, size)) break;
            if (command == 0)
            {
                length = srp_challenge(state, packet, size + 4, out);
            }
            else
            {
                static const uint8_t reconnect[34] = { 0x02, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                                                       0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10 };

                memcpy(out, reconnect, sizeof(reconnect));
                length = sizeof(reconnect);
            }
        }
        else if (command == 1)
        {
            /* LOGON_PROOF: A (32), M1 (20), crc (20), number of keys, security flags (the last two are not sent by every build) */
            if (!sb_read_between(s, packet + 1, 72, 74)) break;
            length = srp_proof(state, packet, out);
            if (length == 0)
            {
                out[0] = 0;
                length = 1;
            }
            else
            {
                sb_log("WoW sandbox: login of %s", state->username);
            }
        }
        else if (command == 3)
        {
            /* RECONNECT_PROOF: the client sends R1 (16), R2 (20), R3 (20), number of keys */
            if (!sb_read_between(s, packet + 1, 56, 57)) break;
            out[0] = 3;
            out[1] = 0;
            length = 2;
        }
        else if (command == 0x10)
        {
            /* REALMLIST_REQUEST: 4 bytes follow. The realm list has the address of the world server. */
            Buffer b;
            const char *name = SANDBOX_REALM_NAME;
            char address[32];

            if (!sb_read_exact(s, packet + 1, 4)) break;
            snprintf(address, sizeof(address), "127.0.0.1:%d", SANDBOX_WORLD_PORT);
            memset(&b, 0, sizeof(b));
            buffer_u8(&b, 0x10);
            buffer_u16(&b, 0);   /* size, set below */
            buffer_u32(&b, 0);
            buffer_u16(&b, 1);   /* one realm */
            buffer_u8(&b, 0);    /* type */
            buffer_u8(&b, 0);    /* locked */
            buffer_u8(&b, 0);    /* flags */
            buffer_string(&b, name);
            buffer_string(&b, address);
            buffer_float(&b, 0); /* population */
            buffer_u8(&b, 0);    /* characters */
            buffer_u8(&b, 1);    /* time zone */
            buffer_u8(&b, 1);    /* id */
            buffer_u16(&b, 0x10);
            b.data[1] = (uint8_t) (b.length - 3);
            b.data[2] = (uint8_t) ((b.length - 3) >> 8);
            sb_write_all(s, b.data, b.length);
            free(b.data);
            continue;
        }
        else
        {
            break;   /* not a command of the logon protocol */
        }
        if (length > 0 && !sb_write_all(s, out, length)) break;
    }
    sb_close(s);
    free(state);
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the listeners */

typedef struct {
    int listener;
    void *(*connection)(void *);
} Acceptor;

static void *accept_main(void *argument)
{
    Acceptor *acceptor = argument;

    for (;;)
    {
        int s = sb_accept_from(acceptor->listener);
        pthread_t thread;
        pthread_attr_t attributes;

        if (s < 0) break;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attributes, acceptor->connection, (void *) (intptr_t) s) != 0) sb_close(s);
        pthread_attr_destroy(&attributes);
    }
    return NULL;
}

static int start_acceptor(uint16_t port, void *(*connection)(void *))
{
    static Acceptor acceptors[2];
    static int used;
    Acceptor *acceptor = &acceptors[used];
    pthread_t thread;
    pthread_attr_t attributes;
    int listener = sb_listen_on(port);

    if (listener < 0)
    {
        sb_log("WoW sandbox: port %u could not be opened", (unsigned int) port);
        return 0;
    }
    acceptor->listener = listener;
    acceptor->connection = connection;
    used++;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attributes, accept_main, acceptor);
    pthread_attr_destroy(&attributes);
    return 1;
}

int WoW_SandboxRun(void)
{
    if (!start_acceptor(SANDBOX_REALM_PORT, realm_connection_main)) return 0;
    if (!start_acceptor(SANDBOX_WORLD_PORT, world_connection_main)) return 0;
    sb_log("WoW sandbox: %s listens on ports %d (login) and %d (world), password \"%s\"", SANDBOX_REALM_NAME, SANDBOX_REALM_PORT, SANDBOX_WORLD_PORT, SANDBOX_PASSWORD);
    return 1;
}

#if !defined(SANDBOX_NATIVE) && defined(WOW_SANDBOX_SELFTEST)
/* scons sandboxtest=1: a client inside the program talks to the server through the in-process sockets */
static int selftest_connect(uint16_t port)
{
    uint32_t s = socket_c(2, 1, 6);
    SbAddress address = { 2, (uint16_t) ((port << 8) | (port >> 8)), 0x0100007f, { 0 } };

    if (s == 0xffffffffu) return -1;
    if (connect_c(s, &address, sizeof(address)) == 0xffffffffu)
    {
        closesocket_c(s);
        return -1;
    }
    return (int) s;
}

static void *selftest_main(void *argument)
{
    uint8_t challenge[34 + 6] = { 0, 8, 36, 0, 'W', 'o', 'W', 0, 1, 12, 1, 0xF3, 0x16, '6', '8', 'x', 0, 'n', 'i', 'W', 0, 'S', 'U', 'n', 'e',
                                  0, 0, 0, 0, 0x7f, 0, 0, 1, 6, 'T', 'E', 'S', 'T', 'E', 'R' };
    uint8_t answer[0x77], world[8];
    int s;

    (void) argument;
    sleep(2);
    s = selftest_connect(SANDBOX_REALM_PORT);
    if (s < 0 || !sb_write_all(s, challenge, sizeof(challenge)) || !sb_read_exact(s, answer, sizeof(answer)) || answer[0] != 0 || answer[2] != 0 || answer[36] != 7)
    {
        sb_log("WoW sandbox self test: the login server does not answer");
        return NULL;
    }
    sb_close(s);
    s = selftest_connect(SANDBOX_WORLD_PORT);
    if (s < 0 || !sb_read_exact(s, world, sizeof(world)) || world[0] != 0 || world[1] != 6 || world[2] != 0xEC || world[3] != 0x01)
    {
        sb_log("WoW sandbox self test: the world server does not answer");
        return NULL;
    }
    sb_close(s);
    sb_log("WoW sandbox self test: OK (the login server answered with %d bytes, the world server greeted)", (int) sizeof(answer));
    return NULL;
}
#endif

#ifndef SANDBOX_NATIVE
/* started once before the program runs: the sandbox is the server when the realm list of the client is 127.0.0.1 */
void WoW_SandboxStart(void)
{
    char line[256], word[128] = "";
    FILE *f;

#ifdef WOW_SANDBOX_SELFTEST
    if (WoW_SandboxRun())
    {
        pthread_t thread;

        pthread_create(&thread, NULL, selftest_main, NULL);
        pthread_detach(thread);
    }
    return;
#endif
    f = fopen("/Port/realmlist.wtf", "r");
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char first[32], second[32], third[128];

        if (sscanf(line, " %31s %31s %127s", first, second, third) == 3 && strcasecmp(first, "set") == 0 && strcasecmp(second, "realmlist") == 0)
        {
            snprintf(word, sizeof(word), "%s", third);
            break;
        }
    }
    fclose(f);
    if (strcmp(word, "127.0.0.1") == 0 || strcmp(word, "127.0.0.1:3724") == 0) WoW_SandboxRun();
}
#endif
