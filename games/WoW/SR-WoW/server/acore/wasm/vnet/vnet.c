/* Virtual sockets for the AzerothCore server module (wasm).

   The server's socket calls are redirected here by the linker (--wrap=socket ...; see vnet-link-flags.txt). Descriptors from VFD_BASE up are virtual:
     - pipes (Boost.Asio's reactor interrupter) live entirely in this module;
     - listeners and stream sockets are served by the host through Module.acoreNet (listen/accept/readable/recv/send/shutdown/close; the
       page's one is browser_backend.js).
   Waiting (select, poll, blocking accept/recv) uses ONE futex word: whoever changes something (a pipe write, a close here; the host when bytes arrive)
   bumps it and wakes the waiters, so reactor threads sleep instead of spinning. Everything else (real files, real fds below VFD_BASE) goes to libc.
   Only what the server uses is implemented: AF_INET stream sockets, no connect (the server only accepts). */
#define _GNU_SOURCE
#include <emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>

#define VFD_BASE 700   /* below FD_SETSIZE (1024): Asio's select reactor refuses larger descriptors */
#define VFD_COUNT 300

typedef enum { V_FREE = 0, V_SOCKET, V_LISTEN, V_STREAM, V_PIPE_R, V_PIPE_W } VKind;

typedef struct VSock
{
    VKind kind;
    int nonblock;
    struct sockaddr_in local, peer;
    int conn;                 /* host connection id of a stream */
    int port;                 /* listener */
    /* pipes: bytes written by the write end, read by the read end */
    uint8_t* buf;
    size_t cap, head, len;
    int other;                /* the other end of a pipe (index), -1 */
    int write_closed;         /* pipe read end: the write end is closed */
    int read_closed;          /* pipe write end: the read end is closed */
} VSock;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static VSock g_socks[VFD_COUNT];
volatile uint32_t g_vnet_wake;   /* the futex word; the host bumps it through acoreWake() */

int __real_close(int fd);
int __real_fcntl(int fd, int cmd, ...);
int __real_ioctl(int fd, int req, ...);
ssize_t __real_read(int fd, void* buf, size_t n);
ssize_t __real_write(int fd, const void* buf, size_t n);
int __real_select(int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* t);
int __real_poll(struct pollfd* fds, nfds_t n, int timeout);

/* ---- host calls: the host (Module.acoreNet) lives on the main thread; the server's threads are workers, so these are proxied synchronously ---- */
static void vnet_register_wake(uint32_t* addr) { MAIN_THREAD_EM_ASM({ Module['acoreNet'].setWakeAddress($0); }, addr); }
static int host_listen(int port) { return MAIN_THREAD_EM_ASM_INT({ return Module['acoreNet'].listen($0); }, port); }
static int host_accept(int port, uint32_t* peer_ip, uint32_t* peer_port) { return MAIN_THREAD_EM_ASM_INT({ return Module['acoreNet'].accept($0, $1, $2); }, port, peer_ip, peer_port); }
static int host_pending(int port) { return MAIN_THREAD_EM_ASM_INT({ return Module['acoreNet'].pending($0); }, port); }
static int host_readable(int conn) { return MAIN_THREAD_EM_ASM_INT({ return Module['acoreNet'].readable($0); }, conn); }
static int host_recv(int conn, void* ptr, int max) { return MAIN_THREAD_EM_ASM_INT({ return Module['acoreNet'].recv($0, $1, $2); }, conn, ptr, max); }
static int host_send(int conn, const void* ptr, int len) { return MAIN_THREAD_EM_ASM_INT({ return Module['acoreNet'].send($0, $1, $2); }, conn, ptr, len); }
static void host_shutdown(int conn, int how) { MAIN_THREAD_EM_ASM({ Module['acoreNet'].shutdown($0, $1); }, conn, how); }
static void host_close(int conn) { MAIN_THREAD_EM_ASM({ Module['acoreNet'].close($0); }, conn); }

static int is_virtual(int fd) { return fd >= VFD_BASE && fd < VFD_BASE + VFD_COUNT; }
static VSock* sock_of(int fd) { return is_virtual(fd) && g_socks[fd - VFD_BASE].kind != V_FREE ? &g_socks[fd - VFD_BASE] : NULL; }

static void wake_all(void)
{
    __atomic_add_fetch(&g_vnet_wake, 1, __ATOMIC_SEQ_CST);
    emscripten_futex_wake((void*) &g_vnet_wake, INT_MAX);
}

static void ensure_registered(void)
{
    static int done;
    if (!done)
    {
        done = 1;
        vnet_register_wake((uint32_t*) &g_vnet_wake);
    }
}

static int alloc_sock(VKind kind)
{
    for (int i = 0; i < VFD_COUNT; ++i)
        if (g_socks[i].kind == V_FREE)
        {
            memset(&g_socks[i], 0, sizeof(VSock));
            g_socks[i].kind = kind;
            g_socks[i].conn = 0;
            g_socks[i].other = -1;
            return i;
        }
    return -1;
}

static void pipe_append(VSock* p, const uint8_t* data, size_t n)
{
    if (p->len + n > p->cap)
    {
        size_t cap = p->cap ? p->cap : 256;
        while (cap < p->len + n)
            cap *= 2;
        uint8_t* nb = (uint8_t*) malloc(cap);
        for (size_t i = 0; i < p->len; ++i)
            nb[i] = p->buf[(p->head + i) % p->cap];
        free(p->buf);
        p->buf = nb;
        p->cap = cap;
        p->head = 0;
    }
    for (size_t i = 0; i < n; ++i)
        p->buf[(p->head + p->len + i) % p->cap] = data[i];
    p->len += n;
}

/* readiness (g_lock held); host calls may block the thread for a moment but never wait for I/O */
static int readable_locked(VSock* s)
{
    switch (s->kind)
    {
        case V_LISTEN: return host_pending(s->port) > 0;
        case V_STREAM: return host_readable(s->conn) != 0;   /* data, or end of file (-1) */
        case V_PIPE_R: return s->len > 0 || s->write_closed;
        default: return 0;
    }
}

static int writable_locked(VSock* s)
{
    return s->kind == V_STREAM || s->kind == V_PIPE_W;
}

/* ---- sockets ---- */
int __wrap_socket(int domain, int type, int protocol)
{
    (void) protocol;
    if (domain != AF_INET || (type & 0xff) != SOCK_STREAM) { errno = EAFNOSUPPORT; return -1; }
    ensure_registered();
    pthread_mutex_lock(&g_lock);
    int i = alloc_sock(V_SOCKET);
    if (i >= 0)
        g_socks[i].nonblock = (type & SOCK_NONBLOCK) != 0;
    pthread_mutex_unlock(&g_lock);
    if (i < 0) { errno = EMFILE; return -1; }
    return VFD_BASE + i;
}

int __wrap_setsockopt(int fd, int level, int name, const void* v, socklen_t l) { (void) level; (void) name; (void) v; (void) l; if (!sock_of(fd)) { errno = EBADF; return -1; } return 0; }

int __wrap_getsockopt(int fd, int level, int name, void* v, socklen_t* l)
{
    if (!sock_of(fd)) { errno = EBADF; return -1; }
    if (v && l && *l >= sizeof(int)) { *(int*) v = 0; *l = sizeof(int); }
    (void) level; (void) name;
    return 0;
}

int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len)
{
    VSock* s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    if (addr->sa_family != AF_INET || len < sizeof(struct sockaddr_in)) { errno = EAFNOSUPPORT; return -1; }
    s->local = *(const struct sockaddr_in*) addr;
    return 0;
}

int __wrap_listen(int fd, int backlog)
{
    (void) backlog;
    VSock* s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    int port = ntohs(s->local.sin_port);
    if (host_listen(port) != 0) { errno = EADDRINUSE; return -1; }
    s->kind = V_LISTEN;
    s->port = port;
    return 0;
}

int __wrap_connect(int fd, const struct sockaddr* addr, socklen_t len) { (void) fd; (void) addr; (void) len; errno = ENETUNREACH; return -1; }

static int do_accept(int fd, struct sockaddr* addr, socklen_t* len, int flags)
{
    ensure_registered();
    for (;;)
    {
        uint32_t seen = __atomic_load_n(&g_vnet_wake, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&g_lock);
        VSock* s = sock_of(fd);
        if (!s || s->kind != V_LISTEN) { pthread_mutex_unlock(&g_lock); errno = EINVAL; return -1; }
        uint32_t ip = 0, port = 0;
        int conn = host_accept(s->port, &ip, &port);
        if (conn > 0)
        {
            int i = alloc_sock(V_STREAM);
            if (i < 0) { host_close(conn); pthread_mutex_unlock(&g_lock); errno = EMFILE; return -1; }
            VSock* c = &g_socks[i];
            c->conn = conn;
            c->nonblock = (flags & SOCK_NONBLOCK) != 0;
            c->local = s->local;
            c->peer.sin_family = AF_INET;
            c->peer.sin_addr.s_addr = ip;
            c->peer.sin_port = htons((uint16_t) port);
            if (addr && len)
            {
                socklen_t n = *len < sizeof(c->peer) ? *len : sizeof(c->peer);
                memcpy(addr, &c->peer, n);
                *len = sizeof(c->peer);
            }
            pthread_mutex_unlock(&g_lock);
            return VFD_BASE + i;
        }
        int nonblock = s->nonblock;
        pthread_mutex_unlock(&g_lock);
        if (nonblock) { errno = EAGAIN; return -1; }
        emscripten_futex_wait((void*) &g_vnet_wake, seen, 1000.0);
    }
}

int __wrap_accept(int fd, struct sockaddr* addr, socklen_t* len) { return do_accept(fd, addr, len, 0); }
int __wrap_accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags) { return do_accept(fd, addr, len, flags); }

int __wrap_getsockname(int fd, struct sockaddr* addr, socklen_t* len)
{
    VSock* s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    socklen_t n = *len < sizeof(s->local) ? *len : sizeof(s->local);
    memcpy(addr, &s->local, n);
    *len = sizeof(s->local);
    return 0;
}

int __wrap_getpeername(int fd, struct sockaddr* addr, socklen_t* len)
{
    VSock* s = sock_of(fd);
    if (!s || s->kind != V_STREAM) { errno = s ? ENOTCONN : EBADF; return -1; }
    socklen_t n = *len < sizeof(s->peer) ? *len : sizeof(s->peer);
    memcpy(addr, &s->peer, n);
    *len = sizeof(s->peer);
    return 0;
}

/* ---- data ---- */
static ssize_t stream_recv(int fd, void* buf, size_t n)
{
    ensure_registered();
    for (;;)
    {
        uint32_t seen = __atomic_load_n(&g_vnet_wake, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&g_lock);
        VSock* s = sock_of(fd);
        if (!s) { pthread_mutex_unlock(&g_lock); errno = EBADF; return -1; }
        if (s->kind == V_STREAM)
        {
            int got = host_recv(s->conn, buf, (int) n);
            if (got > 0) { pthread_mutex_unlock(&g_lock); return got; }
            if (got < 0) { pthread_mutex_unlock(&g_lock); return 0; }   /* end of file */
        }
        else if (s->kind == V_PIPE_R)
        {
            if (s->len > 0)
            {
                size_t k = n < s->len ? n : s->len;
                for (size_t i = 0; i < k; ++i)
                    ((uint8_t*) buf)[i] = s->buf[(s->head + i) % s->cap];
                s->head = (s->head + k) % s->cap;
                s->len -= k;
                pthread_mutex_unlock(&g_lock);
                return (ssize_t) k;
            }
            if (s->write_closed) { pthread_mutex_unlock(&g_lock); return 0; }
        }
        else { pthread_mutex_unlock(&g_lock); errno = ENOTCONN; return -1; }
        int nonblock = s->nonblock;
        pthread_mutex_unlock(&g_lock);
        if (nonblock) { errno = EAGAIN; return -1; }
        emscripten_futex_wait((void*) &g_vnet_wake, seen, 1000.0);
    }
}

static ssize_t stream_send(int fd, const void* buf, size_t n)
{
    ensure_registered();
    pthread_mutex_lock(&g_lock);
    VSock* s = sock_of(fd);
    if (!s) { pthread_mutex_unlock(&g_lock); errno = EBADF; return -1; }
    ssize_t result;
    if (s->kind == V_STREAM)
        result = host_send(s->conn, buf, (int) n);
    else if (s->kind == V_PIPE_W)
    {
        VSock* r = s->other >= 0 ? &g_socks[s->other] : NULL;
        if (!r || r->kind != V_PIPE_R || s->read_closed) { pthread_mutex_unlock(&g_lock); errno = EPIPE; return -1; }
        pipe_append(r, (const uint8_t*) buf, n);
        result = (ssize_t) n;
    }
    else { pthread_mutex_unlock(&g_lock); errno = ENOTCONN; return -1; }
    pthread_mutex_unlock(&g_lock);
    if (result < 0) { errno = EPIPE; return -1; }
    wake_all();   /* a pipe reader may wait for this */
    return result;
}

ssize_t __wrap_recv(int fd, void* buf, size_t n, int flags) { (void) flags; return is_virtual(fd) ? stream_recv(fd, buf, n) : (errno = ENOTSOCK, -1); }
ssize_t __wrap_send(int fd, const void* buf, size_t n, int flags) { (void) flags; return is_virtual(fd) ? stream_send(fd, buf, n) : (errno = ENOTSOCK, -1); }
ssize_t __wrap_recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* a, socklen_t* l) { (void) a; (void) l; return __wrap_recv(fd, buf, n, flags); }
ssize_t __wrap_sendto(int fd, const void* buf, size_t n, int flags, const struct sockaddr* a, socklen_t l) { (void) a; (void) l; return __wrap_send(fd, buf, n, flags); }

ssize_t __wrap_recvmsg(int fd, struct msghdr* m, int flags)
{
    (void) flags;
    ssize_t total = 0;
    for (size_t i = 0; i < m->msg_iovlen; ++i)
    {
        if (!m->msg_iov[i].iov_len)
            continue;
        ssize_t got = stream_recv(fd, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len);
        if (got < 0) { if (total) return total; return -1; }
        total += got;
        if ((size_t) got < m->msg_iov[i].iov_len)
            break;
        /* later buffers: only if more is available right now */
        VSock* s = sock_of(fd);
        if (!s || !readable_locked(s))
            break;
    }
    m->msg_flags = 0;
    return total;
}

ssize_t __wrap_sendmsg(int fd, const struct msghdr* m, int flags)
{
    (void) flags;
    ssize_t total = 0;
    for (size_t i = 0; i < m->msg_iovlen; ++i)
    {
        if (!m->msg_iov[i].iov_len)
            continue;
        ssize_t sent = stream_send(fd, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len);
        if (sent < 0) return total ? total : -1;
        total += sent;
    }
    return total;
}

int __wrap_shutdown(int fd, int how)
{
    VSock* s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    if (s->kind == V_STREAM)
        host_shutdown(s->conn, how);
    return 0;
}

int __wrap_pipe(int fds[2])
{
    pthread_mutex_lock(&g_lock);
    int r = alloc_sock(V_PIPE_R), w = alloc_sock(V_PIPE_W);
    if (r < 0 || w < 0)
    {
        if (r >= 0) g_socks[r].kind = V_FREE;
        if (w >= 0) g_socks[w].kind = V_FREE;
        pthread_mutex_unlock(&g_lock);
        errno = EMFILE;
        return -1;
    }
    g_socks[r].other = w;
    g_socks[w].other = r;
    pthread_mutex_unlock(&g_lock);
    fds[0] = VFD_BASE + r;
    fds[1] = VFD_BASE + w;
    return 0;
}

int __wrap_pipe2(int fds[2], int flags)
{
    int rc = __wrap_pipe(fds);
    if (rc == 0 && (flags & O_NONBLOCK))
    {
        g_socks[fds[0] - VFD_BASE].nonblock = 1;
        g_socks[fds[1] - VFD_BASE].nonblock = 1;
    }
    return rc;
}

int __wrap_close(int fd)
{
    if (!is_virtual(fd))
        return __real_close(fd);
    pthread_mutex_lock(&g_lock);
    VSock* s = sock_of(fd);
    if (!s) { pthread_mutex_unlock(&g_lock); errno = EBADF; return -1; }
    if (s->kind == V_STREAM)
        host_close(s->conn);
    else if (s->kind == V_PIPE_W && s->other >= 0)
        g_socks[s->other].write_closed = 1;
    else if (s->kind == V_PIPE_R && s->other >= 0)
        g_socks[s->other].read_closed = 1;
    free(s->buf);
    s->buf = NULL;
    s->kind = V_FREE;
    pthread_mutex_unlock(&g_lock);
    wake_all();
    return 0;
}

ssize_t __wrap_read(int fd, void* buf, size_t n) { return is_virtual(fd) ? stream_recv(fd, buf, n) : __real_read(fd, buf, n); }
ssize_t __wrap_write(int fd, const void* buf, size_t n) { return is_virtual(fd) ? stream_send(fd, buf, n) : __real_write(fd, buf, n); }

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (!is_virtual(fd))
        return __real_fcntl(fd, cmd, arg);
    VSock* s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    switch (cmd)
    {
        case F_GETFL: return O_RDWR | (s->nonblock ? O_NONBLOCK : 0);
        case F_SETFL: s->nonblock = (arg & O_NONBLOCK) != 0; return 0;
        case F_GETFD: return 0;
        case F_SETFD: return 0;
        default: errno = EINVAL; return -1;
    }
}

int __wrap_ioctl(int fd, int req, ...)
{
    va_list ap;
    va_start(ap, req);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (!is_virtual(fd))
        return __real_ioctl(fd, req, arg);
    VSock* s = sock_of(fd);
    if (!s) { errno = EBADF; return -1; }
    if (req == FIONBIO) { s->nonblock = arg && *(int*) arg; return 0; }
    if (req == FIONREAD) { if (arg) *(int*) arg = s->kind == V_STREAM ? (host_readable(s->conn) > 0 ? host_readable(s->conn) : 0) : (int) s->len; return 0; }
    errno = EINVAL;
    return -1;
}

/* ---- waiting ---- */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* one scan over the sets: fills the result sets, returns how many descriptors are ready (g_lock taken inside) */
static int scan(int nfds, const fd_set* r, const fd_set* w, fd_set* ro, fd_set* wo)
{
    int ready = 0;
    pthread_mutex_lock(&g_lock);
    for (int fd = VFD_BASE; fd < nfds && fd < VFD_BASE + VFD_COUNT; ++fd)
    {
        VSock* s = sock_of(fd);
        if (!s)
            continue;
        if (r && FD_ISSET(fd, r) && readable_locked(s)) { FD_SET(fd, ro); ++ready; }
        if (w && FD_ISSET(fd, w) && writable_locked(s)) { FD_SET(fd, wo); ++ready; }
    }
    pthread_mutex_unlock(&g_lock);
    return ready;
}

int __wrap_select(int nfds, fd_set* r, fd_set* w, fd_set* e, struct timeval* timeout)
{
    ensure_registered();
    if (nfds <= VFD_BASE || (!r && !w))
        return __real_select(nfds, r, w, e, timeout);   /* only real descriptors */
    double deadline = timeout ? now_ms() + timeout->tv_sec * 1000.0 + timeout->tv_usec / 1000.0 : -1;
    fd_set rin, win;
    if (r) rin = *r; else FD_ZERO(&rin);
    if (w) win = *w; else FD_ZERO(&win);
    for (;;)
    {
        uint32_t seen = __atomic_load_n(&g_vnet_wake, __ATOMIC_SEQ_CST);
        fd_set ro, wo;
        FD_ZERO(&ro);
        FD_ZERO(&wo);
        int ready = scan(nfds, &rin, &win, &ro, &wo);
        if (ready > 0 || (deadline >= 0 && now_ms() >= deadline))
        {
            if (r) *r = ro;
            if (w) *w = wo;
            if (e) FD_ZERO(e);
            return ready;
        }
        double wait = deadline >= 0 ? deadline - now_ms() : 1000.0;
        emscripten_futex_wait((void*) &g_vnet_wake, seen, wait < 1000.0 ? wait : 1000.0);
    }
}

int __wrap_poll(struct pollfd* fds, nfds_t n, int timeout)
{
    ensure_registered();
    int any_virtual = 0;
    for (nfds_t i = 0; i < n; ++i)
        any_virtual |= is_virtual(fds[i].fd);
    if (!any_virtual)
        return __real_poll(fds, n, timeout);
    double deadline = timeout >= 0 ? now_ms() + timeout : -1;
    for (;;)
    {
        uint32_t seen = __atomic_load_n(&g_vnet_wake, __ATOMIC_SEQ_CST);
        int ready = 0;
        pthread_mutex_lock(&g_lock);
        for (nfds_t i = 0; i < n; ++i)
        {
            VSock* s = sock_of(fds[i].fd);
            fds[i].revents = 0;
            if (!s)
                continue;
            if ((fds[i].events & POLLIN) && readable_locked(s)) fds[i].revents |= POLLIN;
            if ((fds[i].events & POLLOUT) && writable_locked(s)) fds[i].revents |= POLLOUT;
            if (fds[i].revents) ++ready;
        }
        pthread_mutex_unlock(&g_lock);
        if (ready > 0 || (deadline >= 0 && now_ms() >= deadline))
            return ready;
        double wait = deadline >= 0 ? deadline - now_ms() : 1000.0;
        emscripten_futex_wait((void*) &g_vnet_wake, seen, wait < 1000.0 ? wait : 1000.0);
    }
}
