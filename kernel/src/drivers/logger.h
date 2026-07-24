#pragma once

#include <libs/klibc.h>
#include <stdarg.h>
#include <fs/vfs/vfs.h>
#include <libs/flanterm/flanterm_backends/fb.h>
#include <libs/flanterm/flanterm.h>

extern struct flanterm_context *ft_ctx;

int printk(const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int serial_fprintk(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buffer, size_t capacity, const char *fmt, ...);

size_t logger_kmsg_buffer_size(void);
ssize_t logger_kmsg_read(fd_t *file, void *buf, size_t len, uint64_t flags);
ssize_t logger_kmsg_write(const void *buf, size_t len);
ssize_t logger_kmsg_poll(fd_t *file, int events);
void logger_kmsg_bind_node(vfs_node_t *node);

uint64_t sys_syslog(int type, const char *buf, size_t len);
