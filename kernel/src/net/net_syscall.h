#pragma once

#include <net/socket.h>

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

struct mmsghdr;
struct timespec;

/**
 * Linux contract: disable receive, send, or both halves of a socket.
 * Current kernel: delegates to the socket backend's shutdown hook.
 * Notes: shutdown affects future I/O on the selected half of the connection;
 * it is not the same operation as close(), and buffered readable data may
 * still be observable after SHUT_WR.
 * Gaps: sockets without a shutdown hook fail with -ENOSYS instead of providing
 * a Linux-compatible fallback.
 */
uint64_t sys_shutdown(uint64_t fd, uint64_t how);
/**
 * Linux contract: return the remote peer address for a connected socket.
 * Current kernel: copies the backend-provided peer address back to userspace.
 * Notes: callers should treat addrlen as in/out state and expect truncation if
 * the supplied userspace buffer is too small for the concrete address family.
 * Gaps: unsupported socket backends fail with -ENOSYS.
 */
uint64_t sys_getpeername(int fd, struct sockaddr_un *addr, socklen_t *addrlen);
/**
 * Linux contract: return the local address currently bound to a socket.
 * Current kernel: copies the backend-provided local address back to userspace.
 * Notes: an unbound socket can still return a valid local address on Linux
 * once an implicit bind has happened, so callers should not assume that an
 * explicit bind() was required beforehand.
 * Gaps: unsupported socket backends fail with -ENOSYS.
 */
uint64_t sys_getsockname(int sockfd, struct sockaddr_un *addr,
                         socklen_t *addrlen);
/**
 * Linux contract: create a socket in the requested domain/type/protocol.
 * Current kernel: dispatches to the registered in-kernel socket family table.
 * Notes: the low bits of type carry the actual socket kind, but callers often
 * also pass SOCK_NONBLOCK and SOCK_CLOEXEC here, so implementations must not
 * confuse creation flags with the transport semantics.
 * Gaps: unsupported domains fail with -EAFNOSUPPORT and there is no generic
 * fallback path outside the families registered with the kernel socket layer.
 */
uint64_t sys_socket(int domain, int type, int protocol);
/**
 * Linux contract: create a connected socket pair.
 * Current kernel: works only for domains that expose a socketpair backend.
 * Notes: this is commonly used for local control channels, so callers tend to
 * expect the returned descriptors to share all the usual fd flag semantics such
 * as O_NONBLOCK and close-on-exec handling.
 * Gaps: Linux domains without a registered socketpair implementation fail
 * immediately.
 */
uint64_t sys_socketpair(int family, int type, int protocol, int *sv);
/**
 * Linux contract: bind a socket to a local address.
 * Current kernel: forwards the request to the backend after copying the socket
 * address from userspace.
 * Notes: callers frequently use bind() as a one-shot claim on a local name or
 * port, so backends need to reject duplicate ownership rather than silently
 * rebinding the same endpoint.
 * Gaps: if the backend has no bind hook the syscall currently returns success,
 * which is not Linux-compatible.
 */
uint64_t sys_bind(int sockfd, const struct sockaddr_un *addr,
                  socklen_t addrlen);
/**
 * Linux contract: mark a passive socket as listening.
 * Current kernel: delegates to the backend listen hook.
 * Notes: listen() changes the role of the socket; it does not create a new
 * endpoint. Backlog handling therefore matters for accept() behavior, not for
 * whether the file descriptor itself stays valid.
 * Gaps: missing backend support currently returns success instead of an error.
 */
uint64_t sys_listen(int sockfd, int backlog);
/**
 * Linux contract: accept a pending connection and optionally return the peer
 * address.
 * Current kernel: delegates to the backend accept hook and copies the peer
 * address back to userspace when supplied.
 * Notes: accept-style calls are expected to inherit the underlying open file
 * description semantics, while flags such as SOCK_NONBLOCK/SOCK_CLOEXEC only
 * affect the newly created fd.
 * Gaps: a socket without an accept hook currently falls through as success.
 */
uint64_t sys_accept(int sockfd, struct sockaddr_un *addr, socklen_t *addrlen,
                    uint64_t flags);
/**
 * Linux contract: actively connect a socket to a peer.
 * Current kernel: forwards the request to the backend connect hook.
 * Notes: nonblocking callers often rely on EINPROGRESS/EALREADY style states,
 * so backends should be careful not to collapse every in-flight connect into a
 * generic hard failure.
 * Gaps: missing backend support currently returns success instead of an error.
 */
uint64_t sys_connect(int sockfd, const struct sockaddr_un *addr,
                     socklen_t addrlen);
/**
 * Linux contract: send data or sendto(2)-style datagrams.
 * Current kernel: copies the user buffer into kernel memory and delegates to
 * the backend send hook.
 * Notes: for connection-oriented sockets, a non-NULL destination address is
 * usually ignored or rejected depending on Linux socket type rules, while for
 * datagram sockets it may select the peer per call.
 * Gaps: sockets without send support currently return 0 instead of failing,
 * and zero-copy/MSG_* Linux features are limited to what each backend
 * explicitly implements.
 */
int64_t sys_send(int sockfd, void *buff, size_t len, int flags,
                 struct sockaddr_un *dest_addr, socklen_t addrlen);
/**
 * Linux contract: receive data or recvfrom(2)-style datagrams.
 * Current kernel: delegates to the backend receive hook and copies payload and
 * optional address data back to userspace.
 * Notes: callers often assume that a return of 0 on a stream socket means EOF,
 * which is very different from a 0-length datagram or a temporary empty queue,
 * so the backend-facing semantics here need to stay precise.
 * Gaps: sockets without receive support currently return 0 instead of failing,
 * and advanced Linux MSG_* behavior is limited to what each backend implements.
 */
int64_t sys_recv(int sockfd, void *buff, size_t len, int flags,
                 struct sockaddr_un *dest_addr, socklen_t *addrlen);
/**
 * Linux contract: send a scatter/gather message described by msghdr.
 * Current kernel: copies the user msghdr into a kernel shadow structure and
 * calls the backend sendmsg hook.
 * Notes: Unix-domain STREAM sockets are byte streams with no message boundary,
 * while DGRAM and SEQPACKET preserve message boundaries.
 * Gaps: unsupported backends currently return 0 instead of an error.
 */
int64_t sys_sendmsg(int sockfd, const struct msghdr *msg, int flags);
/**
 * Linux contract: receive a scatter/gather message described by msghdr.
 * Current kernel: uses a kernel shadow msghdr and copies the completed result
 * back to userspace.
 * Notes: the observed read granularity depends on the socket type; Unix STREAM
 * reads can span data from many sends, whereas DGRAM/SEQPACKET consume one
 * packet at a time and may report MSG_TRUNC.
 * Gaps: unsupported backends currently return 0 instead of an error.
 */
int64_t sys_recvmsg(int sockfd, struct msghdr *msg, int flags);

/*
 * VFS vectored I/O entry points for sockets. The iovec array is already a
 * kernel copy, while each iov_base still names userspace memory. Keeping this
 * conversion in the socket layer lets readv()/writev() retain the single
 * message boundary required by DGRAM and SEQPACKET sockets.
 */
int64_t socket_send_iov(int sockfd, const struct iovec *iov, size_t iovlen,
                        int flags);
int64_t socket_recv_iov(int sockfd, const struct iovec *iov, size_t iovlen,
                        int flags);
/**
 * Linux contract: send multiple datagrams/messages with one syscall.
 * Current kernel: loops over sys_sendmsg() one entry at a time.
 * Notes: partial progress is observable. Once one entry has been sent, later
 * failure does not roll the earlier messages back, so callers must inspect the
 * per-entry lengths they get back.
 * Gaps: inherits the same backend limitations as sys_sendmsg().
 */
int64_t sys_sendmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
                     int flags);
/**
 * Linux contract: receive multiple datagrams/messages with an optional timeout.
 * Current kernel: loops over sys_recvmsg() and interprets the timeout in
 * userspace ABI form.
 * Notes: the timeout applies to the receive loop as a whole, not as a fresh
 * per-message budget. Callers that expect record-by-record timing need to build
 * that behavior themselves.
 * Gaps: inherits the same backend limitations as sys_recvmsg(), and timeout
 * behavior is only as precise as the local socket wait implementation.
 */
int64_t sys_recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
                     int flags, struct timespec *timeout);

/**
 * Linux contract: set a socket option at the requested protocol level.
 * Current kernel: copies the option value into kernel memory and delegates to
 * the backend setsockopt hook.
 * Notes: optlen is part of the ABI, not just a convenience parameter. Backends
 * should validate it carefully because Linux userspace does rely on EINVAL and
 * EFAULT differences here.
 * Gaps: unsupported backends fail with -ENOSYS.
 */
uint64_t sys_setsockopt(int fd, int level, int optname, const void *optval,
                        socklen_t optlen);
/**
 * Linux contract: query a socket option and copy the resulting value to the
 * caller's buffer.
 * Current kernel: allocates a kernel-side option buffer and delegates to the
 * backend getsockopt hook.
 * Notes: optlen is in/out state here. A caller may intentionally pass a short
 * buffer and expect truncation plus the written-back final length.
 * Gaps: unsupported backends fail with -ENOSYS.
 */
uint64_t sys_getsockopt(int fd, int level, int optname, void *optval,
                        socklen_t *optlen);
