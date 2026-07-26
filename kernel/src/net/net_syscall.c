#include <arch/arch.h>
#include <net/net_syscall.h>
#include <net/real_socket.h>
#include <net/socket.h>
#include <task/task.h>
#include <fs/fs_syscall.h>
#include <fs/vfs/vfs.h>
#include <drivers/logger.h>
#include <net/netlink.h>

#define SOCKET_MMSG_VLEN_MAX 1024U
#define SOCKET_IOV_MAX 1024U
#define SOCKET_OPT_MAX_LEN (1U << 20)
#define SOCKET_MSG_PAYLOAD_MAX (16U * 1024U * 1024U)
#define SOCKET_MSG_NAME_MAX 256U
#define SOCKET_MSG_CONTROL_MAX (64U * 1024U)

typedef struct socket_msghdr_copy {
    struct msghdr user_shadow;
    struct msghdr kernel_msg;
    struct iovec *user_iov;
    struct iovec *kernel_iov;
    void **kernel_iov_allocs;
    void *name_buf;
    void *control_buf;
} socket_msghdr_copy_t;

static int socket_copy_msghdr_back_to_user(struct msghdr *user_msg,
                                           const struct msghdr *user_shadow,
                                           const struct msghdr *kernel_msg);

static int socket_iov_total_len(const struct iovec *iov, size_t iovlen,
                                size_t *out_total) {
    size_t total = 0;

    if (!out_total)
        return -EINVAL;

    *out_total = 0;
    if (iovlen && !iov)
        return -EFAULT;

    for (size_t i = 0; i < iovlen; i++) {
        if (iov[i].len > SIZE_MAX - total)
            return -EMSGSIZE;
        total += iov[i].len;
    }

    *out_total = total;
    return 0;
}

static bool is_socket(fd_t *fd) {
    if (!(S_ISSOCK(fd->node->i_mode)))
        return false;
    return true;
}

static int socket_validate_user_buffer(const void *ptr, size_t len) {
    if (!len)
        return 0;
    if (!ptr || check_user_overflow((uint64_t)ptr, len))
        return -EFAULT;
    return 0;
}

static int socket_validate_user_mapped_buffer(const void *ptr, size_t len) {
    if (!len)
        return 0;
    if (!ptr || check_user_overflow((uint64_t)ptr, len))
        return -EFAULT;
    return 0;
}

static int socket_alloc_copy_from_user(const void *user_ptr, size_t len,
                                       void **out_buf) {
    int ret = socket_validate_user_buffer(user_ptr, len);
    if (ret < 0)
        return ret;

    void *buf = malloc(len ?: 1);
    if (!buf)
        return -ENOMEM;
    if (copy_from_user(buf, user_ptr, len)) {
        free(buf);
        return -EFAULT;
    }

    *out_buf = buf;
    return 0;
}

static int socket_copy_sockaddr_from_user(const struct sockaddr_un *user_addr,
                                          socklen_t addrlen,
                                          struct sockaddr_un **out_addr) {
    if (addrlen > SOCKET_MSG_NAME_MAX)
        return -EINVAL;
    return socket_alloc_copy_from_user(user_addr, addrlen, (void **)out_addr);
}

static int socket_validate_user_struct(const void *ptr, size_t len) {
    if (!ptr || check_user_overflow((uint64_t)ptr, len))
        return -EFAULT;
    return 0;
}

static int socket_validate_mmsg_array(const struct mmsghdr *msgvec,
                                      unsigned int vlen) {
    if (!vlen || vlen > SOCKET_MMSG_VLEN_MAX)
        return -EINVAL;

    return socket_validate_user_struct(msgvec,
                                       (size_t)vlen * sizeof(struct mmsghdr));
}

static void socket_release_msghdr_copy(socket_msghdr_copy_t *copy) {
    if (!copy)
        return;

    if (copy->kernel_iov_allocs) {
        for (size_t i = 0; i < copy->user_shadow.msg_iovlen; i++) {
            free(copy->kernel_iov_allocs[i]);
        }
    }

    free(copy->kernel_iov_allocs);
    free(copy->kernel_iov);
    free(copy->user_iov);
    free(copy->name_buf);
    free(copy->control_buf);

    copy->kernel_iov_allocs = NULL;
    copy->kernel_iov = NULL;
    copy->user_iov = NULL;
    copy->name_buf = NULL;
    copy->control_buf = NULL;
}

static int socket_prepare_msghdr_from_user(const struct msghdr *user_msg,
                                           socket_msghdr_copy_t *copy,
                                           bool copy_payloads) {
    if (!user_msg || !copy)
        return -EINVAL;

    memset(copy, 0, sizeof(*copy));

    int ret = socket_validate_user_struct(user_msg, sizeof(*user_msg));
    if (ret < 0)
        return ret;

    if (copy_from_user(&copy->user_shadow, user_msg, sizeof(copy->user_shadow)))
        return -EFAULT;

    if (copy->user_shadow.msg_iovlen > SOCKET_IOV_MAX)
        return -EMSGSIZE;
    if (copy->user_shadow.msg_namelen > SOCKET_MSG_NAME_MAX)
        return -ENAMETOOLONG;
    if (copy->user_shadow.msg_controllen > SOCKET_MSG_CONTROL_MAX)
        return -EMSGSIZE;

    if (copy->user_shadow.msg_iovlen > 0) {
        if (!copy->user_shadow.msg_iov)
            return -EFAULT;
        if (copy->user_shadow.msg_iovlen > SIZE_MAX / sizeof(struct iovec))
            return -EINVAL;

        size_t iov_bytes = copy->user_shadow.msg_iovlen * sizeof(struct iovec);
        ret = socket_validate_user_struct(copy->user_shadow.msg_iov, iov_bytes);
        if (ret < 0)
            return ret;

        copy->user_iov = malloc(iov_bytes);
        if (!copy->user_iov)
            return -ENOMEM;
        if (copy_from_user(copy->user_iov, copy->user_shadow.msg_iov,
                           iov_bytes)) {
            socket_release_msghdr_copy(copy);
            return -EFAULT;
        }

        size_t total_len = 0;
        ret = socket_iov_total_len(copy->user_iov, copy->user_shadow.msg_iovlen,
                                   &total_len);
        if (ret < 0) {
            socket_release_msghdr_copy(copy);
            return ret;
        }
        if (total_len > SOCKET_MSG_PAYLOAD_MAX) {
            socket_release_msghdr_copy(copy);
            return -EMSGSIZE;
        }
        (void)total_len;

        copy->kernel_iov =
            calloc(copy->user_shadow.msg_iovlen, sizeof(*copy->kernel_iov));
        if (!copy->kernel_iov) {
            socket_release_msghdr_copy(copy);
            return -ENOMEM;
        }
        copy->kernel_iov_allocs = calloc(copy->user_shadow.msg_iovlen,
                                         sizeof(*copy->kernel_iov_allocs));
        if (!copy->kernel_iov_allocs) {
            socket_release_msghdr_copy(copy);
            return -ENOMEM;
        }

        for (size_t i = 0; i < copy->user_shadow.msg_iovlen; i++) {
            if (!copy->user_iov[i].len)
                continue;

            ret = socket_validate_user_mapped_buffer(copy->user_iov[i].iov_base,
                                                     copy->user_iov[i].len);
            if (ret < 0) {
                socket_release_msghdr_copy(copy);
                return ret;
            }

            copy->kernel_iov[i].len = copy->user_iov[i].len;
            copy->kernel_iov[i].iov_base = malloc(copy->user_iov[i].len);
            if (!copy->kernel_iov[i].iov_base) {
                socket_release_msghdr_copy(copy);
                return -ENOMEM;
            }
            copy->kernel_iov_allocs[i] = copy->kernel_iov[i].iov_base;

            if (copy_payloads && copy_from_user(copy->kernel_iov[i].iov_base,
                                                copy->user_iov[i].iov_base,
                                                copy->user_iov[i].len)) {
                socket_release_msghdr_copy(copy);
                return -EFAULT;
            }
        }
    }

    if (copy->user_shadow.msg_namelen > 0) {
        ret = socket_validate_user_mapped_buffer(copy->user_shadow.msg_name,
                                                 copy->user_shadow.msg_namelen);
        if (ret < 0) {
            socket_release_msghdr_copy(copy);
            return ret;
        }

        copy->name_buf = calloc(1, copy->user_shadow.msg_namelen);
        if (!copy->name_buf) {
            socket_release_msghdr_copy(copy);
            return -ENOMEM;
        }

        if (copy_payloads &&
            copy_from_user(copy->name_buf, copy->user_shadow.msg_name,
                           copy->user_shadow.msg_namelen)) {
            socket_release_msghdr_copy(copy);
            return -EFAULT;
        }
    }

    if (copy->user_shadow.msg_controllen > 0) {
        ret = socket_validate_user_mapped_buffer(
            copy->user_shadow.msg_control, copy->user_shadow.msg_controllen);
        if (ret < 0) {
            socket_release_msghdr_copy(copy);
            return ret;
        }

        copy->control_buf = calloc(1, copy->user_shadow.msg_controllen);
        if (!copy->control_buf) {
            socket_release_msghdr_copy(copy);
            return -ENOMEM;
        }

        if (copy_payloads &&
            copy_from_user(copy->control_buf, copy->user_shadow.msg_control,
                           copy->user_shadow.msg_controllen)) {
            socket_release_msghdr_copy(copy);
            return -EFAULT;
        }
    }

    copy->kernel_msg = copy->user_shadow;
    copy->kernel_msg.msg_iov = copy->kernel_iov;
    copy->kernel_msg.msg_name = copy->name_buf;
    copy->kernel_msg.msg_control = copy->control_buf;
    return 0;
}

static int socket_copy_recv_msghdr_to_user(struct msghdr *user_msg,
                                           socket_msghdr_copy_t *copy,
                                           size_t bytes_read) {
    if (!user_msg || !copy)
        return -EINVAL;

    size_t remaining = bytes_read;
    for (size_t i = 0; i < copy->user_shadow.msg_iovlen && remaining > 0; i++) {
        if (!copy->user_iov[i].iov_base || !copy->user_iov[i].len)
            continue;

        size_t to_copy = MIN(copy->user_iov[i].len, remaining);
        if (copy_to_user(copy->user_iov[i].iov_base,
                         copy->kernel_iov[i].iov_base, to_copy)) {
            return -EFAULT;
        }
        remaining -= to_copy;
    }

    if (copy->name_buf && copy->user_shadow.msg_name &&
        copy->kernel_msg.msg_namelen > 0) {
        size_t name_len = MIN((size_t)copy->user_shadow.msg_namelen,
                              (size_t)copy->kernel_msg.msg_namelen);
        if (name_len && copy_to_user(copy->user_shadow.msg_name, copy->name_buf,
                                     name_len)) {
            return -EFAULT;
        }
    }

    if (copy->control_buf && copy->user_shadow.msg_control &&
        copy->kernel_msg.msg_controllen > 0) {
        size_t control_len = MIN(copy->user_shadow.msg_controllen,
                                 copy->kernel_msg.msg_controllen);
        if (control_len && copy_to_user(copy->user_shadow.msg_control,
                                        copy->control_buf, control_len)) {
            return -EFAULT;
        }
    }

    return socket_copy_msghdr_back_to_user(user_msg, &copy->user_shadow,
                                           &copy->kernel_msg);
}

static int socket_copy_msghdr_back_to_user(struct msghdr *user_msg,
                                           const struct msghdr *user_shadow,
                                           const struct msghdr *kernel_msg) {
    if (!user_msg || !user_shadow || !kernel_msg)
        return -EINVAL;

    struct msghdr out = *user_shadow;
    out.msg_namelen = kernel_msg->msg_namelen;
    out.msg_controllen = kernel_msg->msg_controllen;
    out.msg_flags = kernel_msg->msg_flags;

    return copy_to_user(user_msg, &out, sizeof(out)) ? -EFAULT : 0;
}

static int socket_copy_timeout_from_user(const struct timespec *user_timeout,
                                         uint64_t *timeout_ns_out) {
    if (!timeout_ns_out)
        return -EINVAL;

    if (!user_timeout) {
        *timeout_ns_out = UINT64_MAX;
        return 0;
    }

    int ret = socket_validate_user_struct(user_timeout, sizeof(*user_timeout));
    if (ret < 0)
        return ret;

    struct timespec timeout;
    if (copy_from_user(&timeout, user_timeout, sizeof(timeout)))
        return -EFAULT;

    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 ||
        timeout.tv_nsec >= 1000000000L)
        return -EINVAL;

    uint64_t timeout_ns = (uint64_t)timeout.tv_nsec;
    if ((uint64_t)timeout.tv_sec > UINT64_MAX / 1000000000ULL)
        timeout_ns = UINT64_MAX;
    else {
        uint64_t sec_ns = (uint64_t)timeout.tv_sec * 1000000000ULL;
        if (sec_ns > UINT64_MAX - timeout_ns)
            timeout_ns = UINT64_MAX;
        else
            timeout_ns += sec_ns;
    }

    *timeout_ns_out = timeout_ns;
    return 0;
}

static uint64_t socket_timeout_deadline(uint64_t timeout_ns) {
    if (timeout_ns == UINT64_MAX)
        return UINT64_MAX;

    uint64_t now = nano_time();
    if (timeout_ns > UINT64_MAX - now)
        return UINT64_MAX;
    return now + timeout_ns;
}

static int64_t socket_deadline_remaining_ns(uint64_t deadline) {
    if (deadline == UINT64_MAX)
        return -1;

    uint64_t now = nano_time();
    if (now >= deadline)
        return 0;

    uint64_t remaining = deadline - now;
    if (remaining > (uint64_t)INT64_MAX)
        return INT64_MAX;
    return (int64_t)remaining;
}

static int socket_wait_fd_event(fd_t *fd, uint32_t events, int64_t timeout_ns,
                                const char *reason) {
    if (!fd || !fd->node)
        return -EBADF;

    bool infinite_timeout = timeout_ns < 0;
    uint64_t deadline_ns =
        infinite_timeout ? UINT64_MAX : nano_time() + (uint64_t)timeout_ns;
    uint32_t want = events | EPOLLERR | EPOLLHUP | EPOLLNVAL | EPOLLRDHUP;

    while (true) {
        vfs_poll_wait_table_t table;
        vfs_poll_wait_table_init(&table, current_task);

        int polled = vfs_poll_with_table(fd, want, &table.pt);
        int table_error = vfs_poll_wait_table_error(&table);
        if (table_error) {
            vfs_poll_wait_table_cleanup(&table);
            return table_error;
        }
        if (polled < 0) {
            vfs_poll_wait_table_cleanup(&table);
            return polled;
        }

        if ((uint32_t)polled & want) {
            vfs_poll_wait_table_cleanup(&table);
            return 0;
        }

        if (task_signal_has_deliverable(current_task)) {
            vfs_poll_wait_table_cleanup(&table);
            return -EINTR;
        }
        if (vfs_poll_wait_table_seq_changed(&table)) {
            vfs_poll_wait_table_cleanup(&table);
            continue;
        }

        int64_t sleep_ns = -1;
        if (!infinite_timeout) {
            uint64_t now = nano_time();
            if (now >= deadline_ns) {
                vfs_poll_wait_table_cleanup(&table);
                return -ETIMEDOUT;
            }
            sleep_ns = (int64_t)(deadline_ns - now);
        }

        int block_reason =
            task_block(current_task, TASK_BLOCKING, sleep_ns, reason);
        vfs_poll_wait_table_cleanup(&table);

        if (block_reason == ETIMEDOUT)
            return -ETIMEDOUT;
        if (block_reason < 0)
            return block_reason;
        if (block_reason != EOK && task_signal_has_deliverable(current_task))
            return -EINTR;
    }
}

static int socket_store_mmsg_len(struct mmsghdr *msgvec, unsigned int idx,
                                 int64_t len) {
    unsigned int msg_len =
        len > (int64_t)UINT32_MAX ? UINT32_MAX : (unsigned int)len;
    return copy_to_user(&msgvec[idx].msg_len, &msg_len, sizeof(msg_len))
               ? -EFAULT
               : 0;
}

uint64_t sys_shutdown(uint64_t fd, uint64_t how) {
    fd_t *node = task_get_file(current_task, (int)fd);
    uint64_t ret;

    if (!node)
        return -EBADF;

    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (!handle || !handle->op || !handle->op->shutdown) {
        vfs_file_put(node);
        return -ENOSYS;
    }

    ret = handle->op->shutdown(fd, how);
    vfs_file_put(node);
    return ret;
}

uint64_t sys_getpeername(int fd, struct sockaddr_un *addr, socklen_t *addrlen) {
    fd_t *node = task_get_file(current_task, fd);
    if (fd < 0 || !node)
        return -EBADF;
    if (!addrlen) {
        vfs_file_put(node);
        return -EFAULT;
    }

    uint32_t user_len32 = 0;
    if (copy_from_user(&user_len32, addrlen, sizeof(user_len32))) {
        vfs_file_put(node);
        return (uint64_t)-EFAULT;
    }
    socklen_t user_len = (socklen_t)user_len32;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    void *kaddr = calloc(1, PAGE_SIZE);
    socklen_t kaddrlen = user_len;

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->getpeername) {
        uint64_t ret = handle->op->getpeername(fd, kaddr, &kaddrlen);
        if ((int64_t)ret < 0) {
            free(kaddr);
            vfs_file_put(node);
            return ret;
        }

        size_t copy_len = MIN((size_t)user_len, (size_t)kaddrlen);
        if (copy_len && addr && copy_to_user(addr, kaddr, copy_len)) {
            free(kaddr);
            vfs_file_put(node);
            return (uint64_t)-EFAULT;
        }

        free(kaddr);

        uint32_t out_len32 = (uint32_t)kaddrlen;
        if (copy_to_user(addrlen, &out_len32, sizeof(out_len32))) {
            vfs_file_put(node);
            return (uint64_t)-EFAULT;
        }

        vfs_file_put(node);
        return ret;
    }

    free(kaddr);
    vfs_file_put(node);

    return -ENOSYS;
}

uint64_t sys_getsockname(int sockfd, struct sockaddr_un *addr,
                         socklen_t *addrlen) {
    fd_t *node = task_get_file(current_task, sockfd);
    if (sockfd < 0 || !node)
        return -EBADF;
    if (!addrlen) {
        vfs_file_put(node);
        return -EFAULT;
    }

    uint32_t user_len32 = 0;
    if (copy_from_user(&user_len32, addrlen, sizeof(user_len32))) {
        vfs_file_put(node);
        return (uint64_t)-EFAULT;
    }
    socklen_t user_len = (socklen_t)user_len32;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    void *kaddr = calloc(1, PAGE_SIZE);
    socklen_t kaddrlen = user_len;

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->getsockname) {
        uint64_t ret = handle->op->getsockname(sockfd, kaddr, &kaddrlen);
        if ((int64_t)ret < 0) {
            free(kaddr);
            vfs_file_put(node);
            return ret;
        }

        size_t copy_len = MIN((size_t)user_len, (size_t)kaddrlen);
        if (copy_len && addr && copy_to_user(addr, kaddr, copy_len)) {
            free(kaddr);
            vfs_file_put(node);
            return (uint64_t)-EFAULT;
        }

        free(kaddr);

        uint32_t out_len32 = (uint32_t)kaddrlen;
        if (copy_to_user(addrlen, &out_len32, sizeof(out_len32))) {
            vfs_file_put(node);
            return (uint64_t)-EFAULT;
        }

        vfs_file_put(node);
        return ret;
    }

    free(kaddr);
    vfs_file_put(node);

    return -ENOSYS;
}

uint64_t sys_setsockopt(int fd, int level, int optname, const void *optval,
                        socklen_t optlen) {
    if (optlen > SOCKET_OPT_MAX_LEN)
        return -ENOMEM;

    fd_t *node = task_get_file(current_task, fd);
    if (fd < 0 || !node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->setsockopt) {
        void *koptval = NULL;
        int ret = socket_alloc_copy_from_user(optval, optlen, &koptval);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }

        uint64_t out =
            handle->op->setsockopt(fd, level, optname, koptval, optlen);
        free(koptval);
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return -ENOSYS;
}

uint64_t sys_getsockopt(int fd, int level, int optname, void *optval,
                        socklen_t *optlen) {
    fd_t *node = task_get_file(current_task, fd);
    if (fd < 0 || !node)
        return -EBADF;
    if (!optlen) {
        vfs_file_put(node);
        return -EFAULT;
    }
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->getsockopt) {
        socklen_t user_len = 0;
        if (copy_from_user(&user_len, optlen, sizeof(user_len))) {
            vfs_file_put(node);
            return -EFAULT;
        }
        if (user_len > SOCKET_OPT_MAX_LEN) {
            vfs_file_put(node);
            return -ENOMEM;
        }
        if (user_len && !optval) {
            vfs_file_put(node);
            return -EFAULT;
        }

        void *koptval = NULL;
        if (user_len) {
            koptval = calloc(1, user_len);
            if (!koptval) {
                vfs_file_put(node);
                return -ENOMEM;
            }
        }

        socklen_t koptlen = user_len;
        uint64_t ret =
            handle->op->getsockopt(fd, level, optname, koptval, &koptlen);
        if ((int64_t)ret < 0) {
            free(koptval);
            vfs_file_put(node);
            return ret;
        }

        size_t copy_len = MIN((size_t)user_len, (size_t)koptlen);
        int returned_pidfd = -1;
        if (level == SOL_SOCKET && optname == SO_PEERPIDFD &&
            copy_len >= sizeof(int)) {
            returned_pidfd = *(int *)koptval;
        }

        if (copy_len && copy_to_user(optval, koptval, copy_len)) {
            if (returned_pidfd >= 0)
                sys_close((uint64_t)returned_pidfd);
            free(koptval);
            vfs_file_put(node);
            return -EFAULT;
        }
        free(koptval);

        if (copy_to_user(optlen, &koptlen, sizeof(koptlen))) {
            if (returned_pidfd >= 0)
                sys_close((uint64_t)returned_pidfd);
            vfs_file_put(node);
            return -EFAULT;
        }
        vfs_file_put(node);
        return ret;
    }
    vfs_file_put(node);
    return -ENOSYS;
}

uint64_t sys_socket(int domain, int type, int protocol) {
    int fd = -EAFNOSUPPORT;
    for (int i = 0; i < socket_num; i++) {
        if (real_sockets[i]->domain == domain) {
            fd = real_sockets[i]->socket(domain, type, protocol);
            break;
        }
    }

    return fd;
}

uint64_t sys_socketpair(int domain, int type, int protocol, int *sv) {
    if (!sv || check_user_overflow((uint64_t)sv, sizeof(int) * 2)) {
        return -EFAULT;
    }

    int ksv[2] = {-1, -1};
    int fd = -EAFNOSUPPORT;
    for (int i = 0; i < socket_num; i++) {
        if (real_sockets[i]->domain == domain) {
            fd = real_sockets[i]->socketpair(domain, type, protocol, ksv);
            break;
        }
    }

    if (fd < 0) {
        return fd;
    }

    if (copy_to_user(sv, ksv, sizeof(ksv))) {
        if (ksv[0] >= 0)
            sys_close((uint64_t)ksv[0]);
        if (ksv[1] >= 0)
            sys_close((uint64_t)ksv[1]);
        return -EFAULT;
    }

    return fd;
}

uint64_t sys_bind(int sockfd, const struct sockaddr_un *addr,
                  socklen_t addrlen) {
    fd_t *node = task_get_file(current_task, sockfd);
    if (sockfd < 0 || !node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->bind) {
        struct sockaddr_un *kaddr = NULL;
        int ret = socket_copy_sockaddr_from_user(addr, addrlen, &kaddr);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }
        uint64_t out = handle->op->bind(sockfd, kaddr, addrlen);
        free(kaddr);
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return 0;
}

uint64_t sys_listen(int sockfd, int backlog) {
    fd_t *node = task_get_file(current_task, sockfd);
    if (sockfd < 0 || !node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->listen) {
        uint64_t out = handle->op->listen(sockfd, backlog);
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return 0;
}

uint64_t sys_accept(int sockfd, struct sockaddr_un *addr, socklen_t *addrlen,
                    uint64_t flags) {
    if (!current_task || sockfd < 0)
        return -EBADF;

    fd_t *node = task_get_file(current_task, sockfd);
    if (!node)
        return -EBADF;
    if (fd_get_flags(node) & O_PATH) {
        vfs_file_put(node);
        return -EBADF;
    }
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    uint64_t ret = 0;
    if (handle && handle->op && handle->op->accept) {
        struct sockaddr_un *kaddr = NULL;
        socklen_t kaddrlen = 0;
        socklen_t *kaddrlenp = NULL;

        if (addr) {
            if (!addrlen) {
                vfs_file_put(node);
                return -EFAULT;
            }
            if (copy_from_user(&kaddrlen, addrlen, sizeof(kaddrlen))) {
                vfs_file_put(node);
                return -EFAULT;
            }
            if (kaddrlen > SOCKET_MSG_NAME_MAX) {
                vfs_file_put(node);
                return -EINVAL;
            }
            if (kaddrlen) {
                kaddr = calloc(1, kaddrlen);
                if (!kaddr) {
                    vfs_file_put(node);
                    return -ENOMEM;
                }
            }
            kaddrlenp = &kaddrlen;
        }

        ret = handle->op->accept(sockfd, kaddr, kaddrlenp, flags);
        if ((int64_t)ret >= 0 && kaddrlenp) {
            size_t copy_len = MIN((size_t)kaddrlen, (size_t)*kaddrlenp);
            if (copy_len && copy_to_user(addr, kaddr, copy_len))
                ret = -EFAULT;
            if ((int64_t)ret >= 0 &&
                copy_to_user(addrlen, kaddrlenp, sizeof(*kaddrlenp)))
                ret = -EFAULT;
        }

        free(kaddr);
    }

    vfs_file_put(node);
    return ret;
}

uint64_t sys_connect(int sockfd, const struct sockaddr_un *addr,
                     socklen_t addrlen) {
    fd_t *node = task_get_file(current_task, sockfd);
    if (sockfd < 0 || !node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->connect) {
        struct sockaddr_un *kaddr = NULL;
        int ret = socket_copy_sockaddr_from_user(addr, addrlen, &kaddr);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }
        uint64_t out = handle->op->connect(sockfd, kaddr, addrlen);
        free(kaddr);
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return 0;
}

int64_t sys_send(int sockfd, void *buff, size_t len, int flags,
                 struct sockaddr_un *dest_addr, socklen_t addrlen) {
    if (sockfd < 0)
        return -EBADF;
    if (len > SOCKET_MSG_PAYLOAD_MAX)
        return -EMSGSIZE;
    if (socket_validate_user_mapped_buffer(buff, len) < 0)
        return -EFAULT;

    fd_t *node = task_get_file(current_task, sockfd);
    if (!node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->sendto) {
        void *kbuf = NULL;
        int ret = socket_alloc_copy_from_user(buff, len, &kbuf);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }

        struct sockaddr_un *kaddr = NULL;
        if (dest_addr && addrlen) {
            ret = socket_copy_sockaddr_from_user(dest_addr, addrlen, &kaddr);
            if (ret < 0) {
                free(kbuf);
                vfs_file_put(node);
                return ret;
            }
        }

        int64_t out =
            handle->op->sendto(sockfd, kbuf, len, flags, kaddr, addrlen);
        free(kbuf);
        free(kaddr);
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return 0;
}

int64_t sys_recv(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr_un *dest_addr, socklen_t *addrlen) {
    if (sockfd < 0)
        return -EBADF;
    if (len > SOCKET_MSG_PAYLOAD_MAX)
        return -EMSGSIZE;
    if (socket_validate_user_mapped_buffer(buf, len) < 0)
        return -EFAULT;

    fd_t *node = task_get_file(current_task, sockfd);
    if (!node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->recvfrom) {
        uint8_t *kbuf = NULL;
        if (len > 0) {
            kbuf = malloc(len);
            if (!kbuf) {
                vfs_file_put(node);
                return -ENOMEM;
            }
        }

        struct sockaddr_un *kaddr = NULL;
        socklen_t user_addrlen = 0;
        socklen_t *kaddrlenp = NULL;

        if (dest_addr) {
            if (!addrlen) {
                free(kbuf);
                vfs_file_put(node);
                return -EFAULT;
            }
            if (copy_from_user(&user_addrlen, addrlen, sizeof(user_addrlen))) {
                free(kbuf);
                vfs_file_put(node);
                return -EFAULT;
            }
            if (user_addrlen > SOCKET_MSG_NAME_MAX) {
                free(kbuf);
                vfs_file_put(node);
                return -EINVAL;
            }
            if (user_addrlen) {
                kaddr = calloc(1, user_addrlen);
                if (!kaddr) {
                    free(kbuf);
                    vfs_file_put(node);
                    return -ENOMEM;
                }
            }
            kaddrlenp = &user_addrlen;
        }

        int64_t ret =
            handle->op->recvfrom(sockfd, kbuf, len, flags, kaddr, kaddrlenp);
        if (ret >= 0) {
            size_t copy_len = MIN(len, (size_t)ret);
            if (copy_len && copy_to_user(buf, kbuf, copy_len))
                ret = -EFAULT;
        }
        if (ret >= 0 && kaddrlenp) {
            size_t copy_len = MIN((size_t)user_addrlen, (size_t)*kaddrlenp);
            if (copy_len && copy_to_user(dest_addr, kaddr, copy_len))
                ret = -EFAULT;
            if (ret >= 0 &&
                copy_to_user(addrlen, kaddrlenp, sizeof(*kaddrlenp)))
                ret = -EFAULT;
        }

        free(kbuf);
        free(kaddr);
        vfs_file_put(node);
        return ret;
    }
    vfs_file_put(node);
    return 0;
}

int64_t sys_sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    if (sockfd < 0)
        return -EBADF;

    fd_t *node = task_get_file(current_task, sockfd);
    if (!node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->sendmsg) {
        socket_msghdr_copy_t msg_copy;
        int ret = socket_prepare_msghdr_from_user(msg, &msg_copy, true);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }

        int64_t out = handle->op->sendmsg(sockfd, &msg_copy.kernel_msg, flags);
        socket_release_msghdr_copy(&msg_copy);
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return 0;
}

int64_t sys_recvmsg(int sockfd, struct msghdr *msg, int flags) {
    if (sockfd < 0)
        return -EBADF;

    fd_t *node = task_get_file(current_task, sockfd);
    if (!node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    socket_handle_t *handle = (socket_handle_t *)node->node->i_private;
    if (handle && handle->op && handle->op->recvmsg) {
        socket_msghdr_copy_t msg_copy;
        int ret = socket_prepare_msghdr_from_user(msg, &msg_copy, false);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }

        int64_t out = handle->op->recvmsg(sockfd, &msg_copy.kernel_msg, flags);
        int copy_back_ret = 0;
        if (out >= 0) {
            size_t iov_total = 0;
            ret = socket_iov_total_len(
                msg_copy.user_iov, msg_copy.user_shadow.msg_iovlen, &iov_total);
            if (ret < 0)
                copy_back_ret = ret;
            else
                copy_back_ret = socket_copy_recv_msghdr_to_user(
                    msg, &msg_copy, MIN((size_t)out, iov_total));
        }

        socket_release_msghdr_copy(&msg_copy);
        if (copy_back_ret < 0) {
            vfs_file_put(node);
            return copy_back_ret;
        }
        vfs_file_put(node);
        return out;
    }
    vfs_file_put(node);
    return 0;
}

static int socket_prepare_iov_from_kernel(const struct iovec *iov,
                                          size_t iovlen,
                                          socket_msghdr_copy_t *copy,
                                          bool copy_payloads) {
    size_t iov_bytes;
    size_t total_len = 0;
    int ret;

    if (!copy || (iovlen && !iov))
        return -EINVAL;
    if (iovlen > SOCKET_IOV_MAX || iovlen > SIZE_MAX / sizeof(struct iovec))
        return -EMSGSIZE;

    memset(copy, 0, sizeof(*copy));
    copy->user_shadow.msg_iovlen = iovlen;
    if (!iovlen)
        return 0;

    iov_bytes = iovlen * sizeof(struct iovec);
    copy->user_iov = malloc(iov_bytes);
    if (!copy->user_iov)
        return -ENOMEM;
    memcpy(copy->user_iov, iov, iov_bytes);

    ret = socket_iov_total_len(copy->user_iov, iovlen, &total_len);
    if (ret < 0)
        goto fail;
    if (total_len > SOCKET_MSG_PAYLOAD_MAX) {
        ret = -EMSGSIZE;
        goto fail;
    }

    copy->kernel_iov = calloc(iovlen, sizeof(*copy->kernel_iov));
    copy->kernel_iov_allocs = calloc(iovlen, sizeof(*copy->kernel_iov_allocs));
    if (!copy->kernel_iov || !copy->kernel_iov_allocs) {
        ret = -ENOMEM;
        goto fail;
    }

    for (size_t i = 0; i < iovlen; i++) {
        size_t len = copy->user_iov[i].len;

        if (!len)
            continue;
        ret =
            socket_validate_user_mapped_buffer(copy->user_iov[i].iov_base, len);
        if (ret < 0)
            goto fail;

        copy->kernel_iov[i].iov_base = malloc(len);
        if (!copy->kernel_iov[i].iov_base) {
            ret = -ENOMEM;
            goto fail;
        }
        copy->kernel_iov[i].len = len;
        copy->kernel_iov_allocs[i] = copy->kernel_iov[i].iov_base;

        if (copy_payloads && copy_from_user(copy->kernel_iov[i].iov_base,
                                            copy->user_iov[i].iov_base, len)) {
            ret = -EFAULT;
            goto fail;
        }
    }

    copy->kernel_msg.msg_iov = copy->kernel_iov;
    copy->kernel_msg.msg_iovlen = iovlen;
    return 0;

fail:
    socket_release_msghdr_copy(copy);
    return ret;
}

int64_t socket_send_iov(int sockfd, const struct iovec *iov, size_t iovlen,
                        int flags) {
    socket_msghdr_copy_t copy;
    fd_t *node;
    socket_handle_t *handle;
    int ret;
    int64_t out;

    if (sockfd < 0)
        return -EBADF;
    ret = socket_prepare_iov_from_kernel(iov, iovlen, &copy, true);
    if (ret < 0)
        return ret;

    node = task_get_file(current_task, sockfd);
    if (!node) {
        socket_release_msghdr_copy(&copy);
        return -EBADF;
    }
    if (!is_socket(node)) {
        vfs_file_put(node);
        socket_release_msghdr_copy(&copy);
        return -ENOTSOCK;
    }

    handle = sockfs_file_handle(node);
    if (!handle || !handle->op || !handle->op->sendmsg)
        out = -EOPNOTSUPP;
    else
        out = handle->op->sendmsg(sockfd, &copy.kernel_msg, flags);

    vfs_file_put(node);
    socket_release_msghdr_copy(&copy);
    return out;
}

int64_t socket_recv_iov(int sockfd, const struct iovec *iov, size_t iovlen,
                        int flags) {
    socket_msghdr_copy_t copy;
    fd_t *node;
    socket_handle_t *handle;
    size_t total_len = 0;
    int ret;
    int64_t out;

    if (sockfd < 0)
        return -EBADF;
    ret = socket_prepare_iov_from_kernel(iov, iovlen, &copy, false);
    if (ret < 0)
        return ret;

    node = task_get_file(current_task, sockfd);
    if (!node) {
        socket_release_msghdr_copy(&copy);
        return -EBADF;
    }
    if (!is_socket(node)) {
        vfs_file_put(node);
        socket_release_msghdr_copy(&copy);
        return -ENOTSOCK;
    }

    handle = sockfs_file_handle(node);
    if (!handle || !handle->op || !handle->op->recvmsg)
        out = -EOPNOTSUPP;
    else
        out = handle->op->recvmsg(sockfd, &copy.kernel_msg, flags);

    if (out >= 0) {
        ret = socket_iov_total_len(copy.user_iov, iovlen, &total_len);
        if (ret < 0)
            out = ret;
        else {
            size_t remaining = MIN((size_t)out, total_len);

            for (size_t i = 0; i < iovlen && remaining > 0; i++) {
                size_t len = MIN(copy.user_iov[i].len, remaining);

                if (len && copy_to_user(copy.user_iov[i].iov_base,
                                        copy.kernel_iov[i].iov_base, len)) {
                    out = -EFAULT;
                    break;
                }
                remaining -= len;
            }
        }
    }

    vfs_file_put(node);
    socket_release_msghdr_copy(&copy);
    return out;
}

int64_t sys_sendmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
                     int flags) {
    int ret = socket_validate_mmsg_array(msgvec, vlen);
    if (ret < 0)
        return ret;

    unsigned int sent = 0;
    for (; sent < vlen; sent++) {
        int64_t out = sys_sendmsg(sockfd, &msgvec[sent].msg_hdr, flags);
        if (out < 0)
            return sent ? (int64_t)sent : out;

        ret = socket_store_mmsg_len(msgvec, sent, out);
        if (ret < 0)
            return ret;
    }

    return sent;
}

int64_t sys_recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
                     int flags, struct timespec *timeout) {
    int ret = socket_validate_mmsg_array(msgvec, vlen);
    if (ret < 0)
        return ret;

    uint64_t timeout_ns = UINT64_MAX;
    ret = socket_copy_timeout_from_user(timeout, &timeout_ns);
    if (ret < 0)
        return ret;

    if (sockfd < 0)
        return -EBADF;

    fd_t *node = task_get_file(current_task, sockfd);
    if (!node)
        return -EBADF;
    if (!is_socket(node)) {
        vfs_file_put(node);
        return -ENOTSOCK;
    }

    uint64_t deadline = socket_timeout_deadline(timeout_ns);
    unsigned int received = 0;
    int recv_flags = flags;

    while (received < vlen) {
        bool should_wait = !(recv_flags & MSG_DONTWAIT);
        if (should_wait) {
            int64_t remaining_ns = socket_deadline_remaining_ns(deadline);
            if (remaining_ns == 0)
                break;

            ret = socket_wait_fd_event(node, EPOLLIN, remaining_ns, "recvmmsg");
            if (ret == -ETIMEDOUT)
                break;
            if (ret < 0) {
                vfs_file_put(node);
                return received ? (int64_t)received : ret;
            }
        }

        int call_flags = (recv_flags & ~MSG_WAITFORONE) | MSG_DONTWAIT;
        int64_t out =
            sys_recvmsg(sockfd, &msgvec[received].msg_hdr, call_flags);
        if (out == -(EWOULDBLOCK))
            out = -EAGAIN;
        if (out == -EAGAIN) {
            if (!should_wait) {
                vfs_file_put(node);
                return received ? (int64_t)received : out;
            }
            continue;
        }
        if (out < 0) {
            vfs_file_put(node);
            return received ? (int64_t)received : out;
        }

        ret = socket_store_mmsg_len(msgvec, received, out);
        if (ret < 0) {
            vfs_file_put(node);
            return ret;
        }

        received++;
        if ((flags & MSG_WAITFORONE) && received == 1)
            recv_flags |= MSG_DONTWAIT;
    }

    vfs_file_put(node);
    return received;
}
