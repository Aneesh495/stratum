#include <kernel/socket.h>
#include <kernel/event.h>
#include <kernel/process.h>
#include <kernel/slab.h>
#include <kernel/string.h>
#include <kernel/kernel.h>
#include <shared/errno.h>

static kmem_cache_t *socket_cache;
static fd_table_t kernel_fds;

static fd_table_t *active_fds(void) {
    process_t *process = process_get_current();
    return process ? &process->fds : &kernel_fds;
}

static file_t *socket_fd(int fd) {
    file_t *file = fd_get(active_fds(), fd);
    if (file && file->type != FILE_TYPE_SOCKET) {
        file_close(file);
        return NULL;
    }
    return file;
}

static int64_t socket_read(file_t *file, void *buf, size_t len) {
    socket_t *socket = file->priv;
    return socket->type == SOCK_STREAM ? tcp_recv(socket->pcb.tcp, buf, len) :
                                         udp_recv(socket->pcb.udp, buf, len);
}

static int64_t socket_write(file_t *file, const void *buf, size_t len) {
    socket_t *socket = file->priv;
    return socket->type == SOCK_STREAM ? tcp_send(socket->pcb.tcp, buf, len) :
                                         udp_send(socket->pcb.udp, buf, len, 0, 0);
}

static int socket_poll(file_t *file, uint32_t events) {
    socket_t *socket = file->priv;
    return socket->type == SOCK_STREAM ? tcp_poll(socket->pcb.tcp, events) :
                                         udp_poll(socket->pcb.udp, events);
}

static void destroy_socket(socket_t *socket) {
    if (socket->type == SOCK_STREAM) tcp_close(socket->pcb.tcp);
    else udp_close(socket->pcb.udp);
    kmem_cache_free(socket_cache, socket);
}

static int socket_close(file_t *file) {
    destroy_socket(file->priv);
    return 0;
}

static const file_ops_t socket_ops = {
    .read = socket_read,
    .write = socket_write,
    .poll = socket_poll,
    .close = socket_close,
};

void socket_init(void) {
    socket_cache = kmem_cache_create("socket_cache", sizeof(socket_t), 8);
    kassert(socket_cache != NULL);
    fd_table_init(&kernel_fds);
}

static int install_socket(socket_t *socket) {
    file_t *file = file_alloc(FILE_TYPE_SOCKET, &socket_ops, socket);
    if (!file) {
        destroy_socket(socket);
        return -STRATUM_ENOMEM;
    }
    int fd = fd_alloc(active_fds(), file);
    if (fd < 0) file_close(file);
    return fd;
}

int sys_socket(int domain, int type, int protocol) {
    if (domain != AF_INET) return -STRATUM_EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -STRATUM_ESOCKTNOSUPPORT;
    if (protocol && protocol != (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP)) {
        return -STRATUM_EPROTONOSUPPORT;
    }
    if (!socket_cache) return -STRATUM_ENODEV;
    socket_t *socket = kmem_cache_alloc(socket_cache);
    if (!socket) return -STRATUM_ENOMEM;
    socket->type = type;
    if (type == SOCK_STREAM) socket->pcb.tcp = tcp_new();
    else socket->pcb.udp = udp_new();
    if ((type == SOCK_STREAM && !socket->pcb.tcp) || (type == SOCK_DGRAM && !socket->pcb.udp)) {
        kmem_cache_free(socket_cache, socket);
        return -STRATUM_ENOBUFS;
    }
    return install_socket(socket);
}

static int parse_address(const struct sockaddr *addr, uint32_t len, struct sockaddr_in *result) {
    if (!addr || len < sizeof(*result)) return -STRATUM_EINVAL;
    memcpy(result, addr, sizeof(*result));
    return result->sin_family == AF_INET ? 0 : -STRATUM_EAFNOSUPPORT;
}

int sys_bind(int fd, const struct sockaddr *addr, uint32_t addrlen) {
    struct sockaddr_in sin;
    int res = parse_address(addr, addrlen, &sin);
    if (res != 0) return res;
    uint32_t ip = ntohl(sin.sin_addr.s_addr);
    if (ip && ip != net_get_default_if()->ip_addr) return -STRATUM_EADDRNOTAVAIL;
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    socket_t *socket = file->priv;
    uint16_t port = ntohs(sin.sin_port);
    res = socket->type == SOCK_STREAM ? tcp_bind(socket->pcb.tcp, port) :
                                       udp_bind(socket->pcb.udp, port);
    file_close(file);
    return res;
}

int sys_listen(int fd, int backlog) {
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    socket_t *socket = file->priv;
    int res = socket->type == SOCK_STREAM ? tcp_listen(socket->pcb.tcp, backlog) : -STRATUM_EOPNOTSUPP;
    file_close(file);
    return res;
}

int sys_accept(int fd, struct sockaddr *addr, uint32_t *addrlen) {
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    socket_t *listener = file->priv;
    if (listener->type != SOCK_STREAM) {
        file_close(file);
        return -STRATUM_EOPNOTSUPP;
    }

    tcp_pcb_t *pcb = tcp_accept(listener->pcb.tcp);
    file_close(file);
    if (!pcb) return -STRATUM_EAGAIN;
    socket_t *socket = kmem_cache_alloc(socket_cache);
    if (!socket) {
        tcp_close(pcb);
        return -STRATUM_ENOMEM;
    }
    socket->type = SOCK_STREAM;
    socket->pcb.tcp = pcb;
    struct sockaddr_in peer = {
        .sin_family = AF_INET,
        .sin_port = htons(pcb->remote_port),
        .sin_addr.s_addr = htonl(pcb->remote_ip),
    };
    int child = install_socket(socket);
    if (child >= 0 && addr && addrlen) {
        size_t n = *addrlen < sizeof(peer) ? *addrlen : sizeof(peer);
        memcpy(addr, &peer, n);
        *addrlen = sizeof(peer);
    }
    return child;
}

int sys_connect(int fd, const struct sockaddr *addr, uint32_t addrlen) {
    struct sockaddr_in sin;
    int res = parse_address(addr, addrlen, &sin);
    if (res != 0) return res;
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    socket_t *socket = file->priv;
    uint32_t ip = ntohl(sin.sin_addr.s_addr);
    uint16_t port = ntohs(sin.sin_port);
    res = socket->type == SOCK_STREAM ? tcp_connect(socket->pcb.tcp, ip, port) :
                                       udp_connect(socket->pcb.udp, ip, port);
    if (!res) readiness_notify();
    file_close(file);
    return res;
}

int64_t sys_send(int fd, const void *buf, size_t len, int flags) {
    if (flags) return -STRATUM_EOPNOTSUPP;
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    int64_t res = socket_write(file, buf, len);
    file_close(file);
    return res;
}

int64_t sys_recv(int fd, void *buf, size_t len, int flags) {
    if (flags) return -STRATUM_EOPNOTSUPP;
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    int64_t res = socket_read(file, buf, len);
    file_close(file);
    return res;
}

int sys_shutdown(int fd, int how) {
    if (how != SHUT_WR) return -STRATUM_EOPNOTSUPP;
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    socket_t *socket = file->priv;
    int res = socket->type == SOCK_STREAM ? tcp_shutdown(socket->pcb.tcp) : -STRATUM_EOPNOTSUPP;
    if (!res) readiness_notify();
    file_close(file);
    return res;
}

int socket_fd_type(int fd) {
    file_t *file = socket_fd(fd);
    if (!file) return -STRATUM_EBADF;
    int type = ((socket_t *)file->priv)->type;
    file_close(file);
    return type;
}

int socket_close_fd(int fd) {
    return fd_close(active_fds(), fd);
}
