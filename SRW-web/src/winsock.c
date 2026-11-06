/* wsock32: TCP sockets are implemented here, in the process: connections between two sockets of the program (127.x.x.x,
   the program makes such pairs to wake up its network thread) work completely, connections to any other address fail as if
   there were no network. A browser cannot listen, and reaching a server needs a WebSocket proxy: the POSIX sockets of
   Emscripten (connecting through WebSockets, websockify style) stay available for the day a proxy is configured
   (real_sockets_enabled) and are used for other socket types. A Winsock socket handle is a table index plus 0x400. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <emscripten.h>

#include "srw.h"
#include "srw_debuglog.h"
#include "imports.h"

#define INVALID_SOCKET_ 0xffffffffu
#define HANDLE_OFFSET 0x400u
#define WIN_SOCKET_ERROR 0xffffffffu

static _Thread_local uint32_t socket_error;

static uint32_t winsock_error(int error)
{
    switch (error)
    {
    case EWOULDBLOCK: return 10035;
    case EINPROGRESS: return 10035; /* Winsock reports a connect in progress as WSAEWOULDBLOCK */
    case EALREADY: return 10037;
    case ENOTSOCK: return 10038;
    case EBADF: return 10038;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case EPROTOTYPE: return 10041;
    case ENOPROTOOPT: return 10042;
    case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045;
    case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EINVAL: return 10022;
    case EACCES: return 10013;
    case EFAULT: return 10014;
    default: return 10035;
    }
}

static uint32_t fail(void)
{
    socket_error = winsock_error(errno);
    return WIN_SOCKET_ERROR;
}

uint32_t CCALL WSAStartup_c(uint32_t wVersionRequested, void *lpWSAData)
{
    SRW_LOG(SRW_LOG_NET, "WSAStartup(%x)", (unsigned int) wVersionRequested);
    (void) wVersionRequested;
    memset(lpWSAData, 0, 400);
    *(uint16_t *) lpWSAData = 0x0202;
    *(uint16_t *) ((uint8_t *) lpWSAData + 2) = 0x0202;
    return 0;
}

uint32_t CCALL WSACleanup_c(void)
{
    return 0;
}

uint32_t CCALL WSAGetLastError_c(void)
{
    SRW_LOG(SRW_LOG_NET, "WSAGetLastError() = %u", (unsigned int) socket_error);
    return socket_error;
}

uint32_t CCALL htons_c(uint32_t hostshort)
{
    return ((hostshort & 0xff) << 8) | ((hostshort >> 8) & 0xff);
}

uint32_t CCALL ntohs_c(uint32_t netshort)
{
    return htons_c(netshort);
}

uint32_t CCALL ntohl_c(uint32_t netlong)
{
    return (netlong << 24) | ((netlong & 0xff00) << 8) | ((netlong >> 8) & 0xff00) | (netlong >> 24);
}

uint32_t CCALL inet_addr_c(const char *cp)
{
    unsigned int a, b, c, d;
    char extra;

    SRW_LOG(SRW_LOG_NET, "Inet_addr(\"%s\")", cp);
    if (sscanf(cp, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0xffffffffu;
    return a | (b << 8) | (c << 16) | (d << 24);
}

uint32_t CCALL inet_ntoa_c(void *address)
{
    static _Thread_local char text[16];   /* (per thread, as Windows keeps it) */
    uint32_t in = (uint32_t) (uintptr_t) address; /* the address itself is the argument (a struct in_addr by value) */

    snprintf(text, sizeof(text), "%u.%u.%u.%u", in & 0xff, (in >> 8) & 0xff, (in >> 16) & 0xff, in >> 24);
    return (uint32_t) (uintptr_t) text;
}


/* ------------------------------------------------------------------------------------------------------------------ */
/* the table of sockets */

#define MAX_SOCKETS 256
#define BACKLOG_MAX 16
#define EPHEMERAL_FIRST 49152

typedef struct VSock {
    int used;
    int type;                 /* SOCK_STREAM for the sockets of the process */
    int fd;                   /* descriptor of Emscripten for the real ones, otherwise -1 */
    int nonblocking;
    int bound, listening, connected, eof;
    int bridge_id;            /* a connection through the page's WebSocket to the proxy (0: none), the data arrives in data[] */
    int connecting, connect_failed;
    uint32_t received_total;
    uint32_t error;           /* what SO_ERROR reports */
    uint32_t address;         /* local address, as it is in memory (network order) */
    uint16_t port;            /* local port, host order */
    uint32_t peer_address;
    uint16_t peer_port;
    struct VSock *peer;       /* the other end of a connection inside the process */
    uint8_t *data;            /* received bytes not read yet: data[start .. start + length) */
    uint32_t data_start, data_length, data_capacity;
    struct VSock *backlog[BACKLOG_MAX];
    int backlog_count;
    /* WSAEventSelect: the event, the events asked for, those that happened and are not fetched yet, their errors */
    uint32_t net_event, net_mask, net_pending, net_errors[10];
    int read_armed, write_armed, connect_pending, connected_seen, close_reported;
} VSock;

static VSock sockets[MAX_SOCKETS];
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vchanged = PTHREAD_COND_INITIALIZER;
static uint16_t next_ephemeral = EPHEMERAL_FIRST;
static int real_sockets_enabled;   /* connections to other hosts through the POSIX sockets of Emscripten (WebSockets) */

typedef struct {
    uint16_t family;
    uint16_t port;            /* network order */
    uint32_t address;
    uint8_t zero[8];
} SockAddrIn;

static uint16_t swap16(uint16_t v)
{
    return (uint16_t) ((v << 8) | (v >> 8));
}

static uint32_t werr(uint32_t code)
{
    socket_error = code;
    return WIN_SOCKET_ERROR;
}

static VSock *sock_of(uint32_t s)
{
    VSock *v;

    if (s < HANDLE_OFFSET || s - HANDLE_OFFSET >= MAX_SOCKETS) return NULL;
    v = &sockets[s - HANDLE_OFFSET];
    return v->used ? v : NULL;
}

static uint32_t handle_of(const VSock *v)
{
    return (uint32_t) (v - sockets) + HANDLE_OFFSET;
}

static VSock *allocate_socket(int type)
{
    int i;

    for (i = 0; i < MAX_SOCKETS; i++)
    {
        if (!sockets[i].used)
        {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].used = 1;
            sockets[i].type = type;
            sockets[i].fd = -1;
            return &sockets[i];
        }
    }
    return NULL;
}

/* the sockets wait for each other with the big lock released; vlock is never held while it is taken or given back */
static void wait_begin(int *held)
{
    *held = SRW_GilHeld();
    if (*held) SRW_GilRelease();
}

static void wait_end(int held)
{
    if (held) SRW_GilAcquire();
}

static void deadline_after(uint32_t milliseconds, struct timespec *deadline)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += milliseconds / 1000;
    deadline->tv_nsec += (long) (milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L)
    {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int is_loopback(uint32_t address)
{
    return (address & 0xff) == 127;
}

static int port_in_use(uint16_t port, const VSock *except)
{
    int i;

    for (i = 0; i < MAX_SOCKETS; i++)
    {
        if (sockets[i].used && &sockets[i] != except && sockets[i].bound && sockets[i].fd < 0 && sockets[i].port == port) return 1;
    }
    return 0;
}

static void fill_address(void *name, void *namelen, uint32_t address, uint16_t port)
{
    SockAddrIn sa;
    uint32_t length = namelen != NULL ? *(uint32_t *) namelen : 0;

    if (name == NULL || namelen == NULL) return;
    memset(&sa, 0, sizeof(sa));
    sa.family = 2;
    sa.port = swap16(port);
    sa.address = address;
    if (length > sizeof(sa)) length = sizeof(sa);
    memcpy(name, &sa, length);
    *(uint32_t *) namelen = sizeof(sa);
}

/* the connection of a socket of the process ends: the other side sees the end of the data */
static void detach(VSock *v)
{
    if (v->peer != NULL)
    {
        v->peer->eof = 1;
        v->peer->peer = NULL;
        v->peer = NULL;
    }
}


/* ------------------------------------------------------------------------------------------------------------------ */
/* the network: a TCP connection of the program to another host is a WebSocket of the page to the proxy (a proxy that relays the connection).
   The page's address is in /netproxy.txt (written by the page; empty or missing: there is no network). A receiving thread moves the
   bytes that came in to data[] of the socket, so receiving, select and ioctlsocket work as they do for the sockets of the process.
   Names are not resolved here: gethostbyname gives a made up address (10.255.0.N) and the proxy resolves the name. */

#define FAKE_HOST_MAX 64
#define BRIDGE_CHUNK 16384

static char proxy_url[512];
static int proxy_loaded;
static int next_bridge_id = 1;
static char fake_hosts[FAKE_HOST_MAX][128];
static int fake_host_count;
static pthread_t receive_thread;
static int receive_started;

static const char *net_proxy(void)
{
    if (!proxy_loaded)
    {
        FILE *f = fopen("/Port/netproxy.txt", "r");

        proxy_loaded = 1;
        if (f != NULL)
        {
            if (fgets(proxy_url, sizeof(proxy_url), f) != NULL)
            {
                size_t n = strlen(proxy_url);

                while (n > 0 && (proxy_url[n - 1] == '\n' || proxy_url[n - 1] == '\r' || proxy_url[n - 1] == ' ')) proxy_url[--n] = 0;
            }
            fclose(f);
        }
        if (proxy_url[0] != 0) logprintf("Network through the proxy %s", proxy_url);
    }
    return proxy_url[0] != 0 ? proxy_url : NULL;
}

static int net_js_open(int id, const char *url)
{
    return MAIN_THREAD_EM_ASM_INT({ return Module['srwNet'] ? Module['srwNet'].open($0, $1, $2) : -1; }, id, url, (int) strlen(url));
}

static int net_js_poll(int id, void *buffer, int maximum)
{
    return MAIN_THREAD_EM_ASM_INT({ return Module['srwNet'] ? Module['srwNet'].poll($0, $1, $2) : (3 << 28); }, id, buffer, maximum);
}

static int net_js_send(int id, const void *buffer, int length)
{
    return MAIN_THREAD_EM_ASM_INT({ return Module['srwNet'] ? Module['srwNet'].send($0, $1, $2) : -1; }, id, buffer, length);
}

static void net_js_close(int id)
{
    MAIN_THREAD_EM_ASM({ if (Module['srwNet']) Module['srwNet'].close($0); }, id);
}

static void push_data(VSock *to, const void *buf, uint32_t len);

static void *receive_main(void *argument)
{
    static uint8_t buffer[BRIDGE_CHUNK];
    int i;

    (void) argument;
    for (;;)
    {
        usleep(4000);
        for (i = 0; i < MAX_SOCKETS; i++)
        {
            int id, result, state;
            uint32_t count;

            pthread_mutex_lock(&vlock);
            id = (sockets[i].used && sockets[i].bridge_id != 0 && !sockets[i].connect_failed && !(sockets[i].eof && !sockets[i].connecting)) ? sockets[i].bridge_id : 0;
            pthread_mutex_unlock(&vlock);
            if (id == 0) continue;
            result = net_js_poll(id, buffer, BRIDGE_CHUNK);
            state = (result >> 28) & 7;
            count = (uint32_t) result & 0x0fffffff;
            pthread_mutex_lock(&vlock);
            if (sockets[i].used && sockets[i].bridge_id == id)
            {
                VSock *v = &sockets[i];

                if (count > 0) push_data(v, buffer, count);
                if (state == 1 && v->connecting)
                {
                    v->connecting = 0;
                    v->connected = 1;
                    SRW_LOG(SRW_LOG_NET, "Connection %d through the proxy is open", id);
                }
                else if (state == 3 && v->connecting)
                {
                    v->connecting = 0;
                    v->connect_failed = 1;
                    v->error = 10061; /* WSAECONNREFUSED */
                    logprintf("Connection %d through the proxy failed (see the console of the browser and the log of the proxy)", id);
                }
                else if ((state == 2 || state == 3) && !v->connecting && !v->eof)
                {
                    v->eof = 1;
                    SRW_LOG(SRW_LOG_NET, "Connection %d ended (%s), %u bytes received in all", id, state == 2 ? "closed" : "error", (unsigned int) v->received_total);
                }
                v->received_total += count;
                pthread_cond_broadcast(&vchanged);
            }
            pthread_mutex_unlock(&vlock);
        }
    }
    return NULL;
}

static uint32_t bridge_connect(VSock *v, const SockAddrIn *sa, uint16_t port)
{
    char url[768], host[160];
    const char *proxy = net_proxy();
    uint32_t a = sa->address;
    int id, held = 0, waiting = 0;

    if ((a & 0xffff) == (10u | (255u << 8)) && (a >> 24) >= 1 && (int) (a >> 24) <= fake_host_count)
        snprintf(host, sizeof(host), "%s", fake_hosts[(a >> 24) - 1]);
    else
        snprintf(host, sizeof(host), "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
    snprintf(url, sizeof(url), "%s%chost=%s&port=%u", proxy, strchr(proxy, '?') != NULL ? '&' : '?', host, (unsigned int) port);

    pthread_mutex_lock(&vlock);
    if (v->connected || v->connecting || v->listening)
    {
        pthread_mutex_unlock(&vlock);
        return werr(10056); /* WSAEISCONN */
    }
    id = next_bridge_id++;
    v->bridge_id = id;
    v->connecting = 1;
    v->peer_address = a;
    v->peer_port = port;
    if (!v->bound)
    {
        v->bound = 1;
        v->port = next_ephemeral++;
        v->address = 0x0100007f;
    }
    if (!receive_started)
    {
        receive_started = 1;
        pthread_create(&receive_thread, NULL, receive_main, NULL);
        pthread_detach(receive_thread);
    }
    pthread_mutex_unlock(&vlock);
    SRW_LOG(SRW_LOG_NET, "Connect(%u) to %s:%u through the proxy", (unsigned int) handle_of(v), host, (unsigned int) port);
    net_js_open(id, url);

    if (v->nonblocking) return werr(10035); /* WSAEWOULDBLOCK, the socket becomes writable when it is connected */
    for (;;)
    {
        pthread_mutex_lock(&vlock);
        if (!v->connecting)
        {
            int failed = v->connect_failed;

            pthread_mutex_unlock(&vlock);
            if (waiting) wait_end(held);
            return failed ? werr(10061) : 0;
        }
        pthread_mutex_unlock(&vlock);
        if (!waiting) { wait_begin(&held); waiting = 1; }
        pthread_mutex_lock(&vlock);
        if (v->connecting)
        {
            struct timespec wake;

            deadline_after(50, &wake);
            pthread_cond_timedwait(&vchanged, &vlock, &wake);
        }
        pthread_mutex_unlock(&vlock);
    }
}

static uint32_t bridge_send(VSock *v, const void *buf, uint32_t len)
{
    uint32_t done = 0;

    if (!v->connected) return werr(v->eof ? 10054 : 10057);
    while (done < len)
    {
        uint32_t n = len - done > 65536 ? 65536 : len - done;

        if (net_js_send(v->bridge_id, (const uint8_t *) buf + done, (int) n) < 0) return werr(10054); /* WSAECONNRESET */
        done += n;
    }
    return len;
}

static void release_socket(VSock *v)
{
    int i;

    for (i = 0; i < v->backlog_count; i++)
    {
        VSock *pending = v->backlog[i];

        detach(pending);
        free(pending->data);
        pending->used = 0;
    }
    detach(v);
    free(v->data);
    v->data = NULL;
    v->used = 0;
}

uint32_t CCALL socket_c(uint32_t af, uint32_t type, uint32_t protocol)
{
    VSock *v;

    SRW_LOG(SRW_LOG_NET, "Socket(%u, %u, %u) called", (unsigned int) af, (unsigned int) type, (unsigned int) protocol);
    if (af != 2) return werr(10047); /* WSAEAFNOSUPPORT */
    pthread_mutex_lock(&vlock);
    v = allocate_socket((int) type);
    if (v == NULL)
    {
        pthread_mutex_unlock(&vlock);
        return werr(10024); /* WSAEMFILE */
    }
    if (type != 1)
    {
        /* datagrams and the like are left to the POSIX sockets */
        v->fd = socket((int) af, (int) type, (int) protocol);
        if (v->fd < 0)
        {
            v->used = 0;
            pthread_mutex_unlock(&vlock);
            return fail();
        }
    }
    pthread_mutex_unlock(&vlock);
    SRW_LOG(SRW_LOG_NET, "Socket(%u, %u, %u) = %u", (unsigned int) af, (unsigned int) type, (unsigned int) protocol, (unsigned int) handle_of(v));
    return handle_of(v);
}

uint32_t CCALL closesocket_c(uint32_t s)
{
    VSock *v = sock_of(s);

    if (v == NULL) return werr(10038);
    if (v->bridge_id != 0) net_js_close(v->bridge_id);
    pthread_mutex_lock(&vlock);
    if (v->fd >= 0) close(v->fd);
    release_socket(v);
    pthread_cond_broadcast(&vchanged);
    pthread_mutex_unlock(&vlock);
    return 0;
}

uint32_t CCALL shutdown_c(uint32_t s, uint32_t how)
{
    VSock *v = sock_of(s);

    if (v == NULL) return werr(10038);
    if (v->fd >= 0) return shutdown(v->fd, (int) how) != 0 ? fail() : 0;
    if (!v->connected) return werr(10057);
    pthread_mutex_lock(&vlock);
    if (how != 0 && v->peer != NULL) v->peer->eof = 1;
    pthread_cond_broadcast(&vchanged);
    pthread_mutex_unlock(&vlock);
    return 0;
}

/* HOSTENT with 32-bit pointers, as the program has it */
uint32_t CCALL gethostbyname_c(const char *name)
{
    static _Thread_local struct {
        uint32_t h_name;
        uint32_t h_aliases;
        int16_t h_addrtype;
        int16_t h_length;
        uint32_t h_addr_list;
    } entry;
    static _Thread_local char host_name[256];
    static _Thread_local uint32_t aliases[1], addresses[2], address;
    struct hostent *found;

    SRW_LOG(SRW_LOG_NET, "Gethostbyname(\"%s\")", name);
    if (net_proxy() != NULL && inet_addr(name) == INADDR_NONE)
    {
        /* the proxy resolves the name; the program gets an address that stands for it */
        int i;

        pthread_mutex_lock(&vlock);
        for (i = 0; i < fake_host_count && strcasecmp(fake_hosts[i], name) != 0; i++) { }
        if (i == fake_host_count && fake_host_count < FAKE_HOST_MAX)
        {
            snprintf(fake_hosts[i], sizeof(fake_hosts[i]), "%s", name);
            fake_host_count++;
        }
        pthread_mutex_unlock(&vlock);
        if (i >= FAKE_HOST_MAX)
        {
            socket_error = 11001;
            return 0;
        }
        snprintf(host_name, sizeof(host_name), "%s", name);
        address = 10u | (255u << 8) | ((uint32_t) (i + 1) << 24);
        aliases[0] = 0;
        addresses[0] = (uint32_t) (uintptr_t) &address;
        addresses[1] = 0;
        entry.h_name = (uint32_t) (uintptr_t) host_name;
        entry.h_aliases = (uint32_t) (uintptr_t) aliases;
        entry.h_addrtype = 2;
        entry.h_length = 4;
        entry.h_addr_list = (uint32_t) (uintptr_t) addresses;
        return (uint32_t) (uintptr_t) &entry;
    }
    found = gethostbyname(name);

    if (found == NULL || found->h_addrtype != AF_INET || found->h_addr_list[0] == NULL)
    {
        socket_error = 11001; /* WSAHOST_NOT_FOUND */
        return 0;
    }
    snprintf(host_name, sizeof(host_name), "%s", name);
    memcpy(&address, found->h_addr_list[0], 4);
    aliases[0] = 0;
    addresses[0] = (uint32_t) (uintptr_t) &address;
    addresses[1] = 0;
    entry.h_name = (uint32_t) (uintptr_t) host_name;
    entry.h_aliases = (uint32_t) (uintptr_t) aliases;
    entry.h_addrtype = 2;
    entry.h_length = 4;
    entry.h_addr_list = (uint32_t) (uintptr_t) addresses;
    return (uint32_t) (uintptr_t) &entry;
}

uint32_t CCALL gethostname_c(void *name, uint32_t namelen)
{
    if (namelen < 7) return WIN_SOCKET_ERROR;
    memcpy(name, "srwweb", 7);
    return 0;
}


uint32_t CCALL bind_c(uint32_t s, void *name, uint32_t namelen)
{
    VSock *v = sock_of(s);
    const SockAddrIn *sa = name;
    uint32_t result = 0;

    if (v == NULL) return werr(10038);
    if (namelen < 8 || sa->family != 2) return werr(10047);
    if (v->fd >= 0) return bind(v->fd, (const struct sockaddr *) name, namelen) != 0 ? fail() : 0;

    pthread_mutex_lock(&vlock);
    if (v->bound)
    {
        result = werr(10022); /* WSAEINVAL: already bound */
    }
    else
    {
        uint16_t port = swap16(sa->port);

        if (port == 0)
        {
            while (port_in_use(next_ephemeral, v)) next_ephemeral = (uint16_t) (next_ephemeral + 1 < EPHEMERAL_FIRST ? EPHEMERAL_FIRST : next_ephemeral + 1);
            port = next_ephemeral++;
            if (next_ephemeral < EPHEMERAL_FIRST) next_ephemeral = EPHEMERAL_FIRST;
        }
        else if (port_in_use(port, v))
        {
            result = werr(10048); /* WSAEADDRINUSE */
        }
        if (result == 0)
        {
            v->bound = 1;
            v->port = port;
            v->address = sa->address;
        }
    }
    pthread_mutex_unlock(&vlock);
    SRW_LOG(SRW_LOG_NET, "Bind(%u, port %u address %08x) = %d", (unsigned int) s, (unsigned int) swap16(sa->port), (unsigned int) sa->address, result == 0 ? 0 : -1);
    return result;
}

uint32_t CCALL listen_c(uint32_t s, uint32_t backlog)
{
    VSock *v = sock_of(s);

    (void) backlog;
    if (v == NULL) return werr(10038);
    if (v->fd >= 0) return listen(v->fd, (int) backlog) != 0 ? fail() : 0;
    if (!v->bound) return werr(10022);
    v->listening = 1;
    return 0;
}

uint32_t CCALL connect_c(uint32_t s, void *name, uint32_t namelen)
{
    VSock *v = sock_of(s);
    const SockAddrIn *sa = name;
    uint32_t result = 0;
    uint16_t port;

    if (v == NULL) return werr(10038);
    if (namelen < 8 || sa->family != 2) return werr(10047);
    v->connect_pending = 1;
    if (v->fd >= 0) return connect(v->fd, (const struct sockaddr *) name, namelen) != 0 ? fail() : 0;
    port = swap16(sa->port);

    if (!is_loopback(sa->address))
    {
        if (net_proxy() != NULL) return bridge_connect(v, sa, port);
        if (!real_sockets_enabled)
        {
            logprintf("Connect to %u.%u.%u.%u:%u: there is no network connection", sa->address & 0xff, (sa->address >> 8) & 0xff,
                      (sa->address >> 16) & 0xff, sa->address >> 24, (unsigned int) port);
            return werr(10051); /* WSAENETUNREACH */
        }
        v->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (v->fd < 0) return fail();
        if (v->nonblocking) fcntl(v->fd, F_SETFL, fcntl(v->fd, F_GETFL, 0) | O_NONBLOCK);
        return connect(v->fd, (const struct sockaddr *) name, namelen) != 0 ? fail() : 0;
    }

    pthread_mutex_lock(&vlock);
    if (v->connected || v->listening)
    {
        result = werr(10056); /* WSAEISCONN */
    }
    else
    {
        VSock *listener = NULL, *server = NULL;
        int i;

        for (i = 0; i < MAX_SOCKETS; i++)
        {
            if (sockets[i].used && sockets[i].listening && sockets[i].fd < 0 && sockets[i].port == port)
            {
                listener = &sockets[i];
                break;
            }
        }
        if (listener != NULL && listener->backlog_count < BACKLOG_MAX) server = allocate_socket(1);
        if (server == NULL)
        {
            result = werr(10061); /* WSAECONNREFUSED */
        }
        else
        {
            if (!v->bound)
            {
                while (port_in_use(next_ephemeral, v)) next_ephemeral++;
                v->bound = 1;
                v->port = next_ephemeral++;
                v->address = 0x0100007f;
            }
            server->bound = 1;
            server->connected = 1;
            server->port = port;
            server->address = sa->address;
            server->peer_address = v->address == 0 ? 0x0100007f : v->address;
            server->peer_port = v->port;
            server->peer = v;
            v->peer = server;
            v->connected = 1;
            v->peer_address = sa->address;
            v->peer_port = port;
            listener->backlog[listener->backlog_count++] = server;
            pthread_cond_broadcast(&vchanged);
        }
    }
    pthread_mutex_unlock(&vlock);
    SRW_LOG(SRW_LOG_NET, "Connect(%u, port %u) = %d", (unsigned int) s, (unsigned int) port, result == 0 ? 0 : -1);
    return result;
}

uint32_t CCALL accept_c(uint32_t s, void *addr, void *addrlen)
{
    VSock *v = sock_of(s);
    int held = 0, waiting = 0;
    uint32_t result;

    if (v == NULL) return werr(10038);
    if (v->fd >= 0)
    {
        int accepted = accept(v->fd, (struct sockaddr *) addr, (socklen_t *) addrlen);
        VSock *n;

        if (accepted < 0) return fail();
        n = allocate_socket(1);
        if (n == NULL) { close(accepted); return werr(10024); }
        n->fd = accepted;
        return handle_of(n);
    }
    if (!v->listening) return werr(10022);

    for (;;)
    {
        pthread_mutex_lock(&vlock);
        if (!v->used)
        {
            result = werr(10004); /* WSAEINTR: closed while waiting */
            pthread_mutex_unlock(&vlock);
            break;
        }
        if (v->backlog_count > 0)
        {
            VSock *n = v->backlog[0];

            memmove(v->backlog, v->backlog + 1, (size_t) (v->backlog_count - 1) * sizeof(v->backlog[0]));
            v->backlog_count--;
            fill_address(addr, addrlen, n->peer_address, n->peer_port);
            result = handle_of(n);
            pthread_mutex_unlock(&vlock);
            break;
        }
        if (v->nonblocking)
        {
            result = werr(10035);
            pthread_mutex_unlock(&vlock);
            break;
        }
        pthread_mutex_unlock(&vlock);
        if (!waiting) { wait_begin(&held); waiting = 1; }
        pthread_mutex_lock(&vlock);
        if (v->used && v->backlog_count == 0) pthread_cond_wait(&vchanged, &vlock);
        pthread_mutex_unlock(&vlock);
    }
    if (waiting) wait_end(held);
    return result;
}

uint32_t CCALL getpeername_c(uint32_t s, void *name, void *namelen)
{
    VSock *v = sock_of(s);

    if (v == NULL) return werr(10038);
    if (v->fd >= 0) return getpeername(v->fd, (struct sockaddr *) name, (socklen_t *) namelen) != 0 ? fail() : 0;
    if (!v->connected) return werr(10057);
    fill_address(name, namelen, v->peer_address, v->peer_port);
    return 0;
}

uint32_t CCALL getsockname_c(uint32_t s, void *name, void *namelen)
{
    VSock *v = sock_of(s);

    if (v == NULL) return werr(10038);
    if (v->fd >= 0) return getsockname(v->fd, (struct sockaddr *) name, (socklen_t *) namelen) != 0 ? fail() : 0;
    fill_address(name, namelen, v->address, v->bound ? v->port : 0);
    return 0;
}

/* the options of a socket have other numbers in Winsock */
static int map_option(uint32_t level, uint32_t option, int *out_level, int *out_option)
{
    if (level == 0xffff)
    {
        *out_level = SOL_SOCKET;
        switch (option)
        {
        case 0x0004: *out_option = SO_REUSEADDR; return 1;
        case 0x0008: *out_option = SO_KEEPALIVE; return 1;
        case 0x0080: *out_option = SO_LINGER; return 1;
        case 0x1001: *out_option = SO_SNDBUF; return 1;
        case 0x1002: *out_option = SO_RCVBUF; return 1;
        case 0x1007: *out_option = SO_ERROR; return 1;
        default: return 0;
        }
    }
    *out_level = (int) level;
    *out_option = (int) option;
    return level == 6 && option == 1; /* TCP_NODELAY */
}


uint32_t CCALL setsockopt_c(uint32_t s, uint32_t level, uint32_t optname, void *optval, uint32_t optlen)
{
    VSock *v = sock_of(s);
    int l, o;

    if (v == NULL) return werr(10038);
    if (v->fd < 0) return 0; /* nothing to set on the sockets of the process */
    if (!map_option(level, optname, &l, &o)) return 0; /* the options that are not understood are ignored */
    if (setsockopt(v->fd, l, o, optval, optlen) != 0) return fail();
    return 0;
}

uint32_t CCALL getsockopt_c(uint32_t s, uint32_t level, uint32_t optname, void *optval, void *optlen)
{
    VSock *v = sock_of(s);
    int l, o;

    if (v == NULL) return werr(10038);
    if (v->fd < 0 || !map_option(level, optname, &l, &o))
    {
        uint32_t value = 0;

        if (level == 0xffff && optname == 0x1007) /* SO_ERROR */
        {
            value = v->error;
            v->error = 0;
        }
        if (optlen != NULL && *(uint32_t *) optlen >= 4 && optval != NULL)
        {
            *(uint32_t *) optval = value;
            *(uint32_t *) optlen = 4;
        }
        return 0;
    }
    if (getsockopt(v->fd, l, o, optval, (socklen_t *) optlen) != 0) return fail();
    return 0;
}

uint32_t CCALL ioctlsocket_c(uint32_t s, uint32_t cmd, void *argp)
{
    VSock *v = sock_of(s);

    SRW_LOG(SRW_LOG_NET, "Ioctlsocket(%u, %x)", (unsigned int) s, (unsigned int) cmd);
    if (v == NULL) return werr(10038);
    if (cmd == 0x8004667eu) /* FIONBIO */
    {
        v->nonblocking = *(uint32_t *) argp != 0;
        if (v->fd >= 0)
        {
            int flags = fcntl(v->fd, F_GETFL, 0);

            if (flags < 0) return fail();
            flags = v->nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
            if (fcntl(v->fd, F_SETFL, flags) != 0) return fail();
        }
        return 0;
    }
    if (cmd == 0x4004667fu) /* FIONREAD */
    {
        int available = 0;

        if (v->fd >= 0)
        {
            if (ioctl(v->fd, FIONREAD, &available) != 0) return fail();
        }
        else
        {
            available = (int) v->data_length;
        }
        *(uint32_t *) argp = (uint32_t) available;
        return 0;
    }
    return werr(10022);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* data */

/* appends bytes to what a socket has received (vlock is held) */
static void push_data(VSock *to, const void *buf, uint32_t len)
{
    if (len > 0)
    {
        if (to->data_start + to->data_length + len > to->data_capacity)
        {
            /* make room: move the unread bytes to the front, grow when it is still not enough */
            if (to->data_start > 0)
            {
                memmove(to->data, to->data + to->data_start, to->data_length);
                to->data_start = 0;
            }
            if (to->data_length + len > to->data_capacity)
            {
                uint32_t capacity = to->data_capacity ? to->data_capacity : 4096;

                while (capacity < to->data_length + len) capacity *= 2;
                to->data = realloc(to->data, capacity);
                to->data_capacity = capacity;
            }
        }
        memcpy(to->data + to->data_start + to->data_length, buf, len);
        to->data_length += len;
    }
}

static uint32_t stream_send(VSock *v, const void *buf, uint32_t len)
{
    VSock *to;

    if (v->bridge_id != 0) return bridge_send(v, buf, len);

    pthread_mutex_lock(&vlock);
    if (!v->connected)
    {
        pthread_mutex_unlock(&vlock);
        return werr(10057); /* WSAENOTCONN */
    }
    to = v->peer;
    if (to == NULL)
    {
        pthread_mutex_unlock(&vlock);
        return werr(10054); /* WSAECONNRESET */
    }
    push_data(to, buf, len);
    pthread_cond_broadcast(&vchanged);
    pthread_mutex_unlock(&vlock);
    return len;
}

static uint32_t stream_recv(VSock *v, void *buf, uint32_t len, uint32_t flags)
{
    int held = 0, waiting = 0;
    uint32_t result;

    if (!v->connected) return werr(10057);
    for (;;)
    {
        pthread_mutex_lock(&vlock);
        if (!v->used)
        {
            result = werr(10004);
            pthread_mutex_unlock(&vlock);
            break;
        }
        if (v->data_length > 0)
        {
            uint32_t n = len < v->data_length ? len : v->data_length;

            memcpy(buf, v->data + v->data_start, n);
            if (!(flags & 2)) /* MSG_PEEK */
            {
                v->data_start += n;
                v->data_length -= n;
                if (v->data_length == 0) v->data_start = 0;
            }
            result = n;
            v->read_armed = 1;   /* the next FD_READ comes when data is left */
            pthread_cond_broadcast(&vchanged);
            pthread_mutex_unlock(&vlock);
            break;
        }
        if (v->eof)
        {
            result = 0; /* orderly close */
            pthread_mutex_unlock(&vlock);
            break;
        }
        if (v->nonblocking)
        {
            result = werr(10035);
            pthread_mutex_unlock(&vlock);
            break;
        }
        pthread_mutex_unlock(&vlock);
        if (!waiting) { wait_begin(&held); waiting = 1; }
        pthread_mutex_lock(&vlock);
        if (v->used && v->data_length == 0 && !v->eof) pthread_cond_wait(&vchanged, &vlock);
        pthread_mutex_unlock(&vlock);
    }
    if (waiting) wait_end(held);
    return result;
}

uint32_t CCALL recv_c(uint32_t s, void *buf, uint32_t len, uint32_t flags)
{
    VSock *v = sock_of(s);
    ssize_t got;

    if (v == NULL) return werr(10038);
    if (v->fd < 0)
    {
        uint32_t result = stream_recv(v, buf, len, flags);

        SRW_LOG(SRW_LOG_NET, "Recv(%u, %u bytes) = %d", (unsigned int) s, (unsigned int) len, (int) result);
        return result;
    }
    got = recv(v->fd, buf, len, (int) (flags & 3));
    if (got < 0) return fail();
    return (uint32_t) got;
}

uint32_t CCALL send_c(uint32_t s, void *buf, uint32_t len, uint32_t flags)
{
    VSock *v = sock_of(s);
    ssize_t put;

    (void) flags;
    if (v == NULL) return werr(10038);
    if (v->fd < 0)
    {
        SRW_LOG(SRW_LOG_NET, "Send(%u, %u bytes)", (unsigned int) s, (unsigned int) len);
        return stream_send(v, buf, len);
    }
    put = send(v->fd, buf, len, 0);
    if (put < 0) return fail();
    return (uint32_t) put;
}

uint32_t CCALL recvfrom_c(uint32_t s, void *buf, uint32_t len, uint32_t flags, void *from, void *fromlen)
{
    VSock *v = sock_of(s);
    ssize_t got;

    if (v == NULL) return werr(10038);
    if (v->fd < 0)
    {
        uint32_t result = stream_recv(v, buf, len, flags);

        if (result != WIN_SOCKET_ERROR) fill_address(from, fromlen, v->peer_address, v->peer_port);
        return result;
    }
    got = recvfrom(v->fd, buf, len, (int) (flags & 3), (struct sockaddr *) from, (socklen_t *) fromlen);
    if (got < 0) return fail();
    return (uint32_t) got;
}

uint32_t CCALL sendto_c(uint32_t s, void *buf, uint32_t len, uint32_t flags, void *to, uint32_t tolen)
{
    VSock *v = sock_of(s);
    ssize_t put;

    (void) flags;
    if (v == NULL) return werr(10038);
    if (v->fd < 0) return stream_send(v, buf, len);
    put = sendto(v->fd, buf, len, 0, (struct sockaddr *) to, tolen);
    if (put < 0) return fail();
    return (uint32_t) put;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* select */

/* fd_set of Winsock: a count and an array of sockets */
typedef struct {
    uint32_t count;
    uint32_t sockets[64];
} WinFdSet;

static int real_ready(int fd, int kind)
{
    fd_set set;
    struct timeval tv = { 0, 0 };
    int result;

    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (kind == 0) result = select(fd + 1, &set, NULL, NULL, &tv);
    else if (kind == 1) result = select(fd + 1, NULL, &set, NULL, &tv);
    else result = select(fd + 1, NULL, NULL, &set, &tv);
    return result > 0;
}

/* kind: 0 readable, 1 writable, 2 error */
static int socket_ready(const VSock *v, int kind)
{
    if (v->fd >= 0) return real_ready(v->fd, kind);
    switch (kind)
    {
    case 0: return v->listening ? v->backlog_count > 0 : (v->data_length > 0 || v->eof);
    case 1: return v->connected;
    default: return v->connect_failed;
    }
}

/* keeps the sockets of the set that are ready; returns how many */
static uint32_t filter_ready(const WinFdSet *in, WinFdSet *out, int kind)
{
    uint32_t i, kept = 0;

    out->count = 0;
    if (in == NULL) return 0;
    for (i = 0; i < in->count && i < 64; i++)
    {
        const VSock *v = sock_of(in->sockets[i]);

        if (v != NULL && socket_ready(v, kind)) out->sockets[kept++] = in->sockets[i];
    }
    out->count = kept;
    return kept;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* WSAEventSelect / WSAEnumNetworkEvents: a thread watches the sockets that have an event and sets it when something new happens
   (FD_READ again after the program has read, FD_WRITE once the connection stands, FD_CONNECT, FD_CLOSE, FD_ACCEPT) */

#define FD_READ_ 0x01
#define FD_WRITE_ 0x02
#define FD_ACCEPT_ 0x08
#define FD_CONNECT_ 0x10
#define FD_CLOSE_ 0x20

static pthread_t event_thread;
static int event_thread_started;

/* the events that came up since the last look (vlock is held) */
static uint32_t net_collect(VSock *v)
{
    uint32_t m = v->net_mask, add = 0;

    if (v->net_event == 0) return 0;
    if (v->connected && !v->connected_seen)
    {
        v->connected_seen = 1;
        v->write_armed = 1;
    }
    if ((m & FD_READ_) && v->read_armed && v->data_length > 0) { add |= FD_READ_; v->read_armed = 0; }
    if ((m & FD_WRITE_) && v->write_armed && v->connected) { add |= FD_WRITE_; v->write_armed = 0; }
    if ((m & FD_ACCEPT_) && v->listening && v->backlog_count > 0 && !(v->net_pending & FD_ACCEPT_)) add |= FD_ACCEPT_;
    if ((m & FD_CONNECT_) && v->connect_pending && (v->connected || v->connect_failed))
    {
        add |= FD_CONNECT_;
        v->connect_pending = 0;
        v->net_errors[4] = v->connect_failed ? v->error : 0;
    }
    if ((m & FD_CLOSE_) && v->eof && v->data_length == 0 && !v->close_reported) { add |= FD_CLOSE_; v->close_reported = 1; }
    v->net_pending |= add;
    return add;
}

static void *event_main(void *argument)
{
    (void) argument;
    for (;;)
    {
        uint32_t to_set[MAX_SOCKETS];
        int count = 0, i;
        struct timespec deadline;

        pthread_mutex_lock(&vlock);
        deadline_after(20, &deadline);
        pthread_cond_timedwait(&vchanged, &vlock, &deadline);
        for (i = 0; i < MAX_SOCKETS; i++)
        {
            if (sockets[i].used && sockets[i].net_event != 0 && net_collect(&sockets[i]) != 0) to_set[count++] = sockets[i].net_event;
        }
        pthread_mutex_unlock(&vlock);
        for (i = 0; i < count; i++) SRW_SetEventNative(to_set[i]);
    }
    return NULL;
}

uint32_t CCALL WSAEventSelect_c(uint32_t s, uint32_t hEvent, uint32_t events)
{
    VSock *v = sock_of(s);
    uint32_t pending_now;

    if (v == NULL) return werr(10038);
    pthread_mutex_lock(&vlock);
    v->net_event = events != 0 ? hEvent : 0;
    v->net_mask = events;
    /* events that happened and were not fetched stay (a second WSAEventSelect with another mask must not lose the connection) */
    v->read_armed = 1;
    v->write_armed = 0;
    v->connected_seen = 0;
    v->close_reported = 0;
    if (events != 0) v->nonblocking = 1;   /* as in Winsock: the socket is nonblocking now */
    if (v->fd >= 0 && events != 0) fcntl(v->fd, F_SETFL, fcntl(v->fd, F_GETFL, 0) | O_NONBLOCK);
    if (events != 0 && !event_thread_started)
    {
        event_thread_started = 1;
        pthread_create(&event_thread, NULL, event_main, NULL);
    }
    pending_now = v->net_pending & events;
    pthread_cond_broadcast(&vchanged);
    pthread_mutex_unlock(&vlock);
    if (pending_now != 0) SRW_SetEventNative(hEvent);
    SRW_LOG(SRW_LOG_NET, "WSAEventSelect(%u, event %u, events %x)", (unsigned int) s, (unsigned int) hEvent, (unsigned int) events);
    return 0;
}

uint32_t CCALL WSAEnumNetworkEvents_c(uint32_t s, uint32_t hEvent, void *lpEvents)
{
    VSock *v = sock_of(s);
    uint32_t *out = lpEvents;
    uint32_t event;

    if (v == NULL) return werr(10038);
    pthread_mutex_lock(&vlock);
    net_collect(v);
    out[0] = v->net_pending & v->net_mask;
    memcpy(out + 1, v->net_errors, sizeof(v->net_errors));
    v->net_pending = 0;
    memset(v->net_errors, 0, sizeof(v->net_errors));
    event = hEvent != 0 ? hEvent : v->net_event;
    if (event != 0) SRW_ResetEventNative(event);
    pthread_cond_broadcast(&vchanged);
    pthread_mutex_unlock(&vlock);
    SRW_LOG(SRW_LOG_NET, "WSAEnumNetworkEvents(%u) = %x", (unsigned int) s, (unsigned int) out[0]);
    return 0;
}

uint32_t CCALL select_c(uint32_t nfds, void *readfds, void *writefds, void *exceptfds, void *timeout)
{
    const uint32_t *win_timeout = timeout;
    WinFdSet r, w, e;
    uint32_t ready;
    struct timespec deadline;
    int infinite = win_timeout == NULL, held, has_real = 0, i;

    (void) nfds;
    if (!infinite) deadline_after(win_timeout[0] * 1000u + win_timeout[1] / 1000u, &deadline);
    for (i = 0; i < MAX_SOCKETS; i++)
    {
        if (sockets[i].used && sockets[i].fd >= 0) has_real = 1;
    }
    if (win_timeout != NULL && win_timeout[0] == 0 && win_timeout[1] == 0) SRW_GilPoll();

    for (;;)
    {
        pthread_mutex_lock(&vlock);
        ready = filter_ready(readfds, &r, 0) + filter_ready(writefds, &w, 1) + filter_ready(exceptfds, &e, 2);
        if (ready > 0)
        {
            pthread_mutex_unlock(&vlock);
            break;
        }
        {
            struct timespec now;

            clock_gettime(CLOCK_REALTIME, &now);
            if (!infinite && (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)))
            {
                pthread_mutex_unlock(&vlock);
                break;
            }
        }
        pthread_mutex_unlock(&vlock);

        /* nothing is ready: wait with the big lock released, for a change of the sockets of the process, for the real ones by polling */
        wait_begin(&held);
        pthread_mutex_lock(&vlock);
        {
            struct timespec wake;

            deadline_after(has_real ? 10 : 50, &wake);
            if (!infinite && (deadline.tv_sec < wake.tv_sec || (deadline.tv_sec == wake.tv_sec && deadline.tv_nsec < wake.tv_nsec))) wake = deadline;
            pthread_cond_timedwait(&vchanged, &vlock, &wake);
        }
        pthread_mutex_unlock(&vlock);
        wait_end(held);
    }
    if (readfds != NULL) *(WinFdSet *) readfds = r;
    if (writefds != NULL) *(WinFdSet *) writefds = w;
    if (exceptfds != NULL) *(WinFdSet *) exceptfds = e;
    return ready;
}

uint32_t CCALL __WSAFDIsSet_c(uint32_t s, void *set)
{
    const WinFdSet *fds = set;
    uint32_t i;

    for (i = 0; i < fds->count && i < 64; i++)
    {
        if (fds->sockets[i] == s) return 1;
    }
    return 0;
}
