#pragma once

#include <fs/vfs/vfs.h>

#ifndef IN_ACCESS
#define IN_ACCESS 0x00000001U
#endif
#ifndef IN_MODIFY
#define IN_MODIFY 0x00000002U
#endif
#ifndef IN_ATTRIB
#define IN_ATTRIB 0x00000004U
#endif
#ifndef IN_CLOSE_WRITE
#define IN_CLOSE_WRITE 0x00000008U
#endif
#ifndef IN_CLOSE_NOWRITE
#define IN_CLOSE_NOWRITE 0x00000010U
#endif
#ifndef IN_OPEN
#define IN_OPEN 0x00000020U
#endif
#ifndef IN_MOVED_FROM
#define IN_MOVED_FROM 0x00000040U
#endif
#ifndef IN_MOVED_TO
#define IN_MOVED_TO 0x00000080U
#endif
#ifndef IN_CREATE
#define IN_CREATE 0x00000100U
#endif
#ifndef IN_DELETE
#define IN_DELETE 0x00000200U
#endif
#ifndef IN_DELETE_SELF
#define IN_DELETE_SELF 0x00000400U
#endif
#ifndef IN_MOVE_SELF
#define IN_MOVE_SELF 0x00000800U
#endif
#ifndef IN_UNMOUNT
#define IN_UNMOUNT 0x00002000U
#endif
#ifndef IN_Q_OVERFLOW
#define IN_Q_OVERFLOW 0x00004000U
#endif
#ifndef IN_IGNORED
#define IN_IGNORED 0x00008000U
#endif
#ifndef IN_ONLYDIR
#define IN_ONLYDIR 0x01000000U
#endif
#ifndef IN_DONT_FOLLOW
#define IN_DONT_FOLLOW 0x02000000U
#endif
#ifndef IN_EXCL_UNLINK
#define IN_EXCL_UNLINK 0x04000000U
#endif
#ifndef IN_MASK_CREATE
#define IN_MASK_CREATE 0x10000000U
#endif
#ifndef IN_MASK_ADD
#define IN_MASK_ADD 0x20000000U
#endif
#ifndef IN_ISDIR
#define IN_ISDIR 0x40000000U
#endif
#ifndef IN_ONESHOT
#define IN_ONESHOT 0x80000000U
#endif

#define IN_ALL_EVENTS                                                          \
    (IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE |   \
     IN_OPEN | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE |           \
     IN_DELETE_SELF | IN_MOVE_SELF)

#ifndef IN_CLOEXEC
#define IN_CLOEXEC O_CLOEXEC
#endif
#ifndef IN_NONBLOCK
#define IN_NONBLOCK O_NONBLOCK
#endif

typedef struct notifyfs_watch notifyfs_watch_t;
typedef struct notifyfs_handle notifyfs_handle_t;

#define FAN_CLOEXEC 0x00000001U
#define FAN_NONBLOCK 0x00000002U
#define FAN_CLASS_NOTIF 0x00000000U
#define FAN_ALL_CLASS_BITS 0x0000000cU
#define FAN_REPORT_FID 0x00000200U
#define FAN_REPORT_DIR_FID 0x00000400U
#define FAN_REPORT_NAME 0x00000800U
#define FAN_REPORT_DFID_NAME (FAN_REPORT_DIR_FID | FAN_REPORT_NAME)

#define FAN_MARK_ADD 0x00000001U
#define FAN_MARK_REMOVE 0x00000002U
#define FAN_MARK_DONT_FOLLOW 0x00000004U
#define FAN_MARK_ONLYDIR 0x00000008U
#define FAN_MARK_MOUNT 0x00000010U
#define FAN_MARK_FILESYSTEM 0x00000100U

#define FAN_EVENT_ON_CHILD 0x08000000U
#define FAN_ONDIR 0x40000000U

notifyfs_handle_t *notifyfs_file_handle(struct vfs_file *file);
int notifyfs_is_file(struct vfs_file *file);

int notifyfs_create_handle_file(struct vfs_file **out_file,
                                unsigned int open_flags,
                                notifyfs_handle_t **out_handle);
int notifyfs_handle_add_watch(notifyfs_handle_t *handle,
                              struct vfs_inode *owner_inode,
                              struct vfs_inode *watch_inode, uint64_t mask,
                              uint64_t *wd_out);
int notifyfs_handle_remove_watch(notifyfs_handle_t *handle, uint64_t wd);
int notifyfs_handle_remove_watch_inode(notifyfs_handle_t *handle,
                                       struct vfs_inode *watch_inode);
int notifyfs_handle_set_fanotify(notifyfs_handle_t *handle,
                                 unsigned int init_flags);
bool notifyfs_handle_is_fanotify(notifyfs_handle_t *handle);

uint32_t notifyfs_next_cookie(void);
bool notifyfs_queue_inode_event(struct vfs_inode *watch_inode,
                                struct vfs_inode *changed_inode,
                                const char *name, uint64_t mask,
                                uint32_t cookie);

void notifyfs_init(void);
