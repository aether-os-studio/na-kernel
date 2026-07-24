#include <boot/boot.h>
#include <fs/vfs/vfs.h>
#include <fs/vfs/cgroup/cgroupfs.h>
#include <init/callbacks.h>
#include <libs/string_builder.h>
#include <mm/fault.h>
#include <mm/mm.h>
#include <mm/page_table.h>
#include <irq/irq_manager.h>
#include <task/sched.h>
#include <task/keyring.h>
#include <task/ptrace.h>
#include <task/task_syscall.h>
#include <task/wait.h>

extern sched_rq_t schedulers[MAX_CPU_NUM];
extern hashmap_t task_parent_map;
extern hashmap_t task_pgid_map;
extern struct llist_header should_free_tasks;
extern spinlock_t should_free_lock;

static inline bool task_page_table_levels_valid(uint64_t levels) {
    return levels > 0 && levels <= ARCH_MAX_PT_LEVEL;
}

static uint64_t sys_clone_internal(struct pt_regs *regs, uint64_t flags,
                                   uint64_t newsp, int *parent_tid,
                                   int *child_tid, uint64_t tls, int cgroup_fd);
static uint64_t task_sleep_until_ns(uint64_t target_ns, int clock_id,
                                    bool interruptible,
                                    struct timespec *remain);
static inline uint64_t task_ns_add_saturate(uint64_t left, uint64_t right);
static inline uint64_t task_timespec_to_ns_saturate(const struct timespec *ts);
static void task_wait_vfork_done_poll(task_t *parent);

static void task_child_wait_arm(wait_queue_entry_t *wait) {
    task_prepare_block(current_task);
    wait_queue_entry_init(wait, current_task, EPOLLIN, NULL, NULL);
    wait_queue_add(&current_task->child_wait, wait);
}

static void task_child_wait_disarm(wait_queue_entry_t *wait) {
    wait_queue_remove(&current_task->child_wait, wait);
    task_cancel_block_prepare(current_task);
}

static bool waitpid_task_matches(const task_t *task, int64_t wait_pid,
                                 uint64_t caller_pgid) {
    if (!task)
        return false;

    if (wait_pid > 0)
        return task->pid == (uint64_t)wait_pid;
    if (wait_pid == 0)
        return task->pgid == (int64_t)caller_pgid;
    if (wait_pid < -1)
        return task->pgid == -wait_pid;
    return wait_pid == -1;
}

static bool waitpid_has_specific_target(const task_t *waiter, int wait_pid) {
    if (!waiter)
        return false;
    if (wait_pid <= 0)
        return false;

    task_t *task = task_find_by_pid((uint64_t)wait_pid);
    if (!task)
        return false;

    if (task_parent_wait_key(task) ==
        task_effective_wait_parent_pid((task_t *)waiter))
        return true;
    return ptrace_matches_waiter(task, waiter);
}

static bool waitid_task_matches(const task_t *task, int idtype,
                                uint64_t match_id) {
    if (!task)
        return false;

    switch (idtype) {
    case P_PID:
        return task->pid == match_id;
    case P_PGID:
        return task->pgid == (int64_t)match_id;
    case P_ALL:
        return true;
    default:
        return false;
    }
}

static bool wait_has_ptraced_target_waitpid(task_t *waiter, int64_t wait_pid) {
    if (!waiter)
        return false;

    spin_lock(&task_queue_lock);
    for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
        hashmap_entry_t *entry = &task_pid_map.buckets[i];
        task_t *task;

        if (!hashmap_entry_is_occupied(entry))
            continue;

        task = (task_t *)entry->value;
        if (!ptrace_matches_waiter(task, waiter))
            continue;
        if (waitpid_task_matches(task, wait_pid, waiter->pgid)) {
            spin_unlock(&task_queue_lock);
            return true;
        }
    }
    spin_unlock(&task_queue_lock);

    return false;
}

static bool wait_has_ptraced_target_waitid(task_t *waiter, int idtype,
                                           uint64_t match_id) {
    if (!waiter)
        return false;

    spin_lock(&task_queue_lock);
    for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
        hashmap_entry_t *entry = &task_pid_map.buckets[i];
        task_t *task;

        if (!hashmap_entry_is_occupied(entry))
            continue;

        task = (task_t *)entry->value;
        if (!ptrace_matches_waiter(task, waiter))
            continue;
        if (waitid_task_matches(task, idtype, match_id)) {
            spin_unlock(&task_queue_lock);
            return true;
        }
    }
    spin_unlock(&task_queue_lock);

    return false;
}

static void waitpid_mark_waiters_locked(task_t *waiter,
                                        uint64_t wait_parent_pid,
                                        int64_t wait_pid) {
    if (!waiter)
        return;

    task_index_bucket_t *children =
        task_index_bucket_lookup(&task_parent_map, wait_parent_pid);
    struct llist_header *node = children ? children->tasks.next : NULL;
    while (children && node != &children->tasks) {
        task_t *task = list_entry(node, task_t, parent_node);
        node = node->next;

        if (!task || task_parent_wait_key(task) != wait_parent_pid)
            continue;
        if (task->state == TASK_DIED || task_is_reaped(task))
            continue;
        if (waitpid_task_matches(task, wait_pid, waiter->pgid))
            task->waitpid = waiter->pid;
    }

    for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
        hashmap_entry_t *entry = &task_pid_map.buckets[i];
        task_t *task;

        if (!hashmap_entry_is_occupied(entry))
            continue;

        task = (task_t *)entry->value;
        if (!ptrace_matches_waiter(task, waiter))
            continue;
        if (task->state == TASK_DIED || ptrace_has_wait_event(task))
            continue;
        if (waitpid_task_matches(task, wait_pid, waiter->pgid))
            task->waitpid = waiter->pid;
    }
}

static void waitid_mark_waiters_locked(task_t *waiter, uint64_t wait_parent_pid,
                                       int idtype, uint64_t match_id) {
    if (!waiter)
        return;

    task_index_bucket_t *children =
        task_index_bucket_lookup(&task_parent_map, wait_parent_pid);
    struct llist_header *node = children ? children->tasks.next : NULL;
    while (children && node != &children->tasks) {
        task_t *task = list_entry(node, task_t, parent_node);
        node = node->next;

        if (!task || task_parent_wait_key(task) != wait_parent_pid)
            continue;
        if (task->state == TASK_DIED || task_is_reaped(task))
            continue;
        if (waitid_task_matches(task, idtype, match_id))
            task->waitpid = waiter->pid;
    }

    for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
        hashmap_entry_t *entry = &task_pid_map.buckets[i];
        task_t *task;

        if (!hashmap_entry_is_occupied(entry))
            continue;

        task = (task_t *)entry->value;
        if (!ptrace_matches_waiter(task, waiter))
            continue;
        if (task->state == TASK_DIED || ptrace_has_wait_event(task))
            continue;
        if (waitid_task_matches(task, idtype, match_id))
            task->waitpid = waiter->pid;
    }
}

static inline bool timer_clockid_supported(clockid_t clockid) {
    return clockid == CLOCK_REALTIME || clockid == CLOCK_MONOTONIC;
}

static inline uint64_t realtime_now_ns(void) {
    return boot_get_boottime() * 1000000000ULL + nano_time();
}

static inline uint64_t timer_current_time_ns(clockid_t clockid) {
    if (clockid == CLOCK_REALTIME)
        return realtime_now_ns();

    return nano_time();
}

static inline int timer_validate_spec(const struct itimerspec *spec) {
    if (!spec)
        return -EINVAL;

    if (spec->it_value.tv_sec < 0 || spec->it_value.tv_nsec < 0 ||
        spec->it_interval.tv_sec < 0 || spec->it_interval.tv_nsec < 0 ||
        spec->it_value.tv_nsec >= 1000000000L ||
        spec->it_interval.tv_nsec >= 1000000000L) {
        return -EINVAL;
    }

    return 0;
}

static inline uint64_t timer_spec_to_ns(const struct timerfd_timespec *ts) {
    if (!ts)
        return 0;

    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static inline void timer_ns_to_spec(uint64_t ns, struct timerfd_timespec *ts) {
    if (!ts)
        return;

    ts->tv_sec = ns / 1000000000ULL;
    ts->tv_nsec = ns % 1000000000ULL;
}

static int read_task_file_into_user_memory(task_t *task, vfs_node_t *node,
                                           uint64_t uaddr, size_t offset,
                                           size_t size) {
    if (!task || !task->arch_context || !task->mm || !node)
        return -EFAULT;
    if (size == 0)
        return 0;
    if (check_user_overflow(uaddr, size))
        return -EFAULT;

    uint64_t *pgdir = (uint64_t *)phys_to_virt(task->mm->page_table_addr);
    uint64_t va = uaddr;
    size_t remain = size;
    size_t file_off = offset;

    while (remain > 0) {
        uint64_t page_va = PADDING_DOWN(va, PAGE_SIZE);
        uint64_t pa = translate_address(pgdir, page_va);
        if (!pa)
            return -EFAULT;

        size_t in_page = va - page_va;
        size_t chunk = MIN(remain, PAGE_SIZE - in_page);
        size_t loaded = 0;
        fd_t fd = {
            .f_op = node->i_fop,
            .f_inode = node,
            .node = node,
            .f_flags = O_RDONLY,
        };
        while (loaded < chunk) {
            loff_t pos = (loff_t)(file_off + loaded);
            ssize_t ret = vfs_read_kernel_file(
                &fd, (void *)(phys_to_virt(pa) + in_page + loaded),
                chunk - loaded, &pos);
            if (ret < 0)
                return ret;
            if (ret == 0)
                break;
            loaded += (size_t)ret;
        }

        va += chunk;
        file_off += chunk;
        remain -= chunk;

        if (loaded < chunk)
            break;
    }

    return 0;
}

static int zero_task_user_memory(task_t *task, uint64_t uaddr, size_t size) {
    if (!task || !task->arch_context || !task->mm)
        return -EFAULT;
    if (size == 0)
        return 0;
    if (check_user_overflow(uaddr, size))
        return -EFAULT;

    uint64_t *pgdir = (uint64_t *)phys_to_virt(task->mm->page_table_addr);
    uint64_t va = uaddr;
    size_t remain = size;

    while (remain > 0) {
        uint64_t page_va = PADDING_DOWN(va, PAGE_SIZE);
        uint64_t pa = translate_address(pgdir, page_va);
        if (!pa)
            return -EFAULT;

        size_t in_page = va - page_va;
        size_t chunk = MIN(remain, PAGE_SIZE - in_page);
        memset((void *)(phys_to_virt(pa) + in_page), 0, chunk);

        va += chunk;
        remain -= chunk;
    }

    return 0;
}

static int sync_task_user_instruction_memory(task_t *task, uint64_t uaddr,
                                             size_t size) {
    if (!task || !task->arch_context || !task->mm)
        return -EFAULT;
    if (size == 0)
        return 0;
    if (check_user_overflow(uaddr, size))
        return -EFAULT;

    uint64_t *pgdir = (uint64_t *)phys_to_virt(task->mm->page_table_addr);
    uint64_t va = uaddr;
    size_t remain = size;

    while (remain > 0) {
        uint64_t page_va = PADDING_DOWN(va, PAGE_SIZE);
        uint64_t page_pa = translate_address(pgdir, page_va);
        if (!page_pa)
            return -EFAULT;

        size_t in_page = va - page_va;
        size_t chunk = MIN(remain, PAGE_SIZE - in_page);
        sync_instruction_memory_range((void *)(phys_to_virt(page_pa) + in_page),
                                      chunk);

        va += chunk;
        remain -= chunk;
    }

    return 0;
}

static uint64_t task_user_translate_access(task_t *task, uint64_t uaddr,
                                           bool write) {
    if (!task || !task->mm || !uaddr)
        return 0;

    uint64_t *pgdir = (uint64_t *)phys_to_virt(task->mm->page_table_addr);
    uint64_t levels = arch_page_table_levels();
    if (!task_page_table_levels_valid(levels))
        return 0;
    uint64_t indexs[ARCH_MAX_PT_LEVEL];
    for (uint64_t i = 0; i < levels; i++)
        indexs[i] = PAGE_TABLE_LEVEL_INDEX(uaddr, i + 1, levels);

    for (uint64_t i = 0; i < levels - 1; i++) {
        uint64_t entry = pgdir[indexs[i]];
        if (ARCH_PT_IS_LARGE(entry)) {
            uint64_t flags = ARCH_READ_PTE_FLAG(entry);
            if (!(flags & ARCH_PT_FLAG_USER))
                return 0;
            if (write && !arch_page_table_flags_writable(flags))
                return 0;
            uint64_t mask = PAGE_TABLE_LEVEL_MASK(i + 1, levels);
            return (ARCH_READ_PTE(entry) & ~mask) + (uaddr & mask);
        }
        if (!ARCH_PT_IS_TABLE(entry))
            return 0;
        pgdir = (uint64_t *)phys_to_virt(ARCH_READ_PTE(entry));
    }

    uint64_t pte = pgdir[indexs[levels - 1]];
    if (!(pte & ARCH_PT_FLAG_VALID))
        return 0;

    uint64_t flags = ARCH_READ_PTE_FLAG(pte);
    if (!(flags & ARCH_PT_FLAG_USER))
        return 0;
    if (write && !arch_page_table_flags_writable(flags))
        return 0;

    return ARCH_READ_PTE(pte) + (uaddr & PAGE_TABLE_LEVEL_MASK(levels, levels));
}

static uint64_t task_user_translate_or_fault(task_t *task, uint64_t uaddr,
                                             bool write) {
    uint64_t pa = task_user_translate_access(task, uaddr, write);
    if (pa)
        return pa;

    if (handle_page_fault_flags(task, uaddr,
                                write ? PF_ACCESS_WRITE : PF_ACCESS_READ) !=
        PF_RES_OK) {
        return 0;
    }

    return task_user_translate_access(task, uaddr, write);
}

int write_task_user_memory(task_t *task, uint64_t uaddr, const void *src,
                           size_t size) {
    if (!task || !task->arch_context || !task->mm)
        return -EFAULT;
    if (!src || size == 0)
        return 0;
    if (check_user_overflow(uaddr, size))
        return -EFAULT;

    const uint8_t *in = (const uint8_t *)src;
    uint64_t va = uaddr;
    size_t remain = size;

    while (remain > 0) {
        uint64_t pa = task_user_translate_or_fault(task, va, true);
        if (!pa)
            return -EFAULT;

        size_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        size_t chunk = MIN(remain, page_left);
        memcpy((void *)phys_to_virt(pa), in, chunk);

        va += chunk;
        in += chunk;
        remain -= chunk;
    }

    return 0;
}

int read_task_user_memory(task_t *task, uint64_t uaddr, void *dst,
                          size_t size) {
    if (!task || !task->arch_context || !task->mm)
        return -EFAULT;
    if (!dst || size == 0)
        return 0;
    if (check_user_overflow(uaddr, size))
        return -EFAULT;

    uint8_t *out = (uint8_t *)dst;
    uint64_t va = uaddr;
    size_t remain = size;

    while (remain > 0) {
        uint64_t pa = task_user_translate_or_fault(task, va, false);
        if (!pa)
            return -EFAULT;

        size_t page_left = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        size_t chunk = MIN(remain, page_left);
        memcpy(out, (const void *)phys_to_virt(pa), chunk);

        va += chunk;
        out += chunk;
        remain -= chunk;
    }

    return 0;
}

static uint64_t process_vm_rw(uint64_t pid, const struct iovec *lvec,
                              uint64_t liovcnt, const struct iovec *rvec,
                              uint64_t riovcnt, uint64_t flags, bool write) {
    task_t *target;
    struct iovec *local_iov = NULL;
    struct iovec *remote_iov = NULL;
    uint8_t bounce[256];
    uint64_t copied = 0;
    uint64_t li = 0;
    uint64_t ri = 0;
    size_t loff = 0;
    size_t roff = 0;
    size_t local_iov_bytes;
    size_t remote_iov_bytes;

    if (flags != 0)
        return (uint64_t)-EINVAL;
    if (!lvec || !rvec || liovcnt == 0 || riovcnt == 0)
        return (uint64_t)-EINVAL;
    if (liovcnt > 1024 || riovcnt > 1024)
        return (uint64_t)-EINVAL;
    if (check_user_array_overflow((uint64_t)lvec, (size_t)liovcnt,
                                  sizeof(*lvec), &local_iov_bytes) ||
        check_user_array_overflow((uint64_t)rvec, (size_t)riovcnt,
                                  sizeof(*rvec), &remote_iov_bytes)) {
        return (uint64_t)-EFAULT;
    }

    target = task_find_by_pid(pid);
    if (!target || !target->mm)
        return (uint64_t)-ESRCH;

    local_iov = malloc(local_iov_bytes);
    remote_iov = malloc(remote_iov_bytes);
    if (!local_iov || !remote_iov) {
        free(local_iov);
        free(remote_iov);
        return (uint64_t)-ENOMEM;
    }

    if (copy_from_user(local_iov, lvec, local_iov_bytes) ||
        copy_from_user(remote_iov, rvec, remote_iov_bytes)) {
        free(local_iov);
        free(remote_iov);
        return (uint64_t)-EFAULT;
    }

    while (li < liovcnt && ri < riovcnt) {
        uint8_t *lbase = local_iov[li].iov_base;
        uint8_t *rbase = remote_iov[ri].iov_base;
        size_t llen = (size_t)local_iov[li].len;
        size_t rlen = (size_t)remote_iov[ri].len;
        size_t lremain = llen > loff ? llen - loff : 0;
        size_t rremain = rlen > roff ? rlen - roff : 0;
        size_t chunk = MIN(sizeof(bounce), MIN(lremain, rremain));

        if (lremain == 0) {
            li++;
            loff = 0;
            continue;
        }
        if (rremain == 0) {
            ri++;
            roff = 0;
            continue;
        }

        if (write) {
            if (copy_from_user(bounce, lbase + loff, chunk)) {
                free(local_iov);
                free(remote_iov);
                return copied ? copied : (uint64_t)-EFAULT;
            }
            if (write_task_user_memory(target, (uint64_t)(rbase + roff), bounce,
                                       chunk) < 0) {
                free(local_iov);
                free(remote_iov);
                return copied ? copied : (uint64_t)-EFAULT;
            }
        } else {
            if (read_task_user_memory(target, (uint64_t)(rbase + roff), bounce,
                                      chunk) < 0) {
                free(local_iov);
                free(remote_iov);
                return copied ? copied : (uint64_t)-EFAULT;
            }
            if (copy_to_user(lbase + loff, bounce, chunk)) {
                free(local_iov);
                free(remote_iov);
                return copied ? copied : (uint64_t)-EFAULT;
            }
        }

        copied += chunk;
        loff += chunk;
        roff += chunk;
    }

    free(local_iov);
    free(remote_iov);
    return copied;
}

uint64_t sys_process_vm_readv(uint64_t pid, const struct iovec *lvec,
                              uint64_t liovcnt, const struct iovec *rvec,
                              uint64_t riovcnt, uint64_t flags) {
    return process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, false);
}

uint64_t sys_process_vm_writev(uint64_t pid, const struct iovec *lvec,
                               uint64_t liovcnt, const struct iovec *rvec,
                               uint64_t riovcnt, uint64_t flags) {
    return process_vm_rw(pid, lvec, liovcnt, rvec, riovcnt, flags, true);
}

static uint64_t elf_segment_vma_flags(uint32_t p_flags) {
    uint64_t vm_flags = 0;

    if (p_flags & PF_R)
        vm_flags |= VMA_READ;
    if (p_flags & PF_W)
        vm_flags |= VMA_WRITE;
    if (p_flags & PF_X)
        vm_flags |= VMA_EXEC;

    return vm_flags;
}

static uint64_t elf_segment_pt_flags(uint32_t p_flags) {
    uint64_t pt_flags = PT_FLAG_U;

    if (p_flags & PF_R)
        pt_flags |= PT_FLAG_R;
    if (p_flags & PF_W)
        pt_flags |= PT_FLAG_W;
    if (p_flags & PF_X)
        pt_flags |= PT_FLAG_X;

    return pt_flags;
}

static int map_task_elf_segment(task_t *task, vfs_node_t *node,
                                uint64_t load_base, const Elf64_Phdr *phdr) {
    if (!task || !task->mm || !node || !phdr || phdr->p_type != PT_LOAD)
        return -EINVAL;

    uint64_t seg_addr = load_base + phdr->p_vaddr;
    uint64_t aligned_addr = PADDING_DOWN(seg_addr, PAGE_SIZE);
    uint64_t aligned_offset = PADDING_DOWN(phdr->p_offset, PAGE_SIZE);
    uint64_t page_prefix = seg_addr - aligned_addr;
    uint64_t file_map_size = phdr->p_filesz + page_prefix;
    uint64_t mem_map_size = phdr->p_memsz + page_prefix;
    uint64_t alloc_size = PADDING_UP(mem_map_size, PAGE_SIZE);
    uint64_t final_flags = elf_segment_pt_flags(phdr->p_flags);
    uint64_t load_flags = final_flags | PT_FLAG_W;

    if (file_map_size < phdr->p_filesz || mem_map_size < phdr->p_memsz ||
        alloc_size < mem_map_size || phdr->p_filesz > phdr->p_memsz) {
        return -EINVAL;
    }

    uint64_t map_ret =
        map_page_range((uint64_t *)phys_to_virt(task->mm->page_table_addr),
                       aligned_addr, (uint64_t)-1, alloc_size, load_flags);
    if (map_ret != 0)
        return -ENOMEM;

    int ret = read_task_file_into_user_memory(task, node, aligned_addr,
                                              aligned_offset, file_map_size);
    if (ret != 0) {
        unmap_page_range((uint64_t *)phys_to_virt(task->mm->page_table_addr),
                         aligned_addr, alloc_size);
        return ret;
    }

    if (mem_map_size > file_map_size) {
        ret = zero_task_user_memory(task, aligned_addr + file_map_size,
                                    mem_map_size - file_map_size);
        if (ret != 0) {
            unmap_page_range(
                (uint64_t *)phys_to_virt(task->mm->page_table_addr),
                aligned_addr, alloc_size);
            return ret;
        }
    }

    if (phdr->p_flags & PF_X) {
        ret = sync_task_user_instruction_memory(task, aligned_addr, alloc_size);
        if (ret != 0) {
            unmap_page_range(
                (uint64_t *)phys_to_virt(task->mm->page_table_addr),
                aligned_addr, alloc_size);
            return ret;
        }
    }

    if (load_flags != final_flags) {
        map_change_attribute_range(
            (uint64_t *)phys_to_virt(task->mm->page_table_addr), aligned_addr,
            alloc_size, final_flags, true);
    }

    return 0;
}

uint64_t timeval_to_ms(struct timeval tv) {
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000; // 微秒转毫秒
}

void ms_to_timeval(uint64_t ms, struct timeval *tv) {
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000; // 转换为微秒保持结构体定义
}

extern task_index_bucket_t *task_index_bucket_get_or_create(hashmap_t *map,
                                                            uint64_t key);

extern void task_index_bucket_destroy_if_empty(hashmap_t *map, uint64_t key);

extern void task_parent_index_attach_locked(task_t *task);

extern void task_parent_index_detach_locked(task_t *task, bool prune_bucket);

static void task_pgid_index_attach_locked(task_t *task) {
    if (!task_should_index_pgid(task, task->pgid) ||
        !llist_empty(&task->pgid_node)) {
        return;
    }

    task_index_bucket_t *bucket =
        task_index_bucket_get_or_create(&task_pgid_map, (uint64_t)task->pgid);
    if (!bucket) {
        return;
    }

    llist_append(&bucket->tasks, &task->pgid_node);
    bucket->count++;
}

static void task_pgid_index_detach_locked(task_t *task) {
    if (!task_should_index_pgid(task, task->pgid) ||
        llist_empty(&task->pgid_node)) {
        return;
    }

    uint64_t pgid = (uint64_t)task->pgid;
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_pgid_map, pgid);
    llist_delete(&task->pgid_node);

    if (bucket && bucket->count) {
        bucket->count--;
    }

    task_index_bucket_destroy_if_empty(&task_pgid_map, pgid);
}

static void task_pid_index_remove_locked(task_t *task) {
    if (!task || !task->pid) {
        return;
    }

    hashmap_remove(&task_pid_map, task->pid);
}

static void task_set_pgid_locked(task_t *task, int64_t pgid) {
    if (!task || task->pgid == pgid) {
        return;
    }

    task_pgid_index_detach_locked(task);
    task->pgid = pgid;
    task_pgid_index_attach_locked(task);
}

static void task_set_thread_group_pgid_locked(task_t *task, int64_t pgid) {
    if (!task)
        return;

    uint64_t tgid = task_effective_tgid(task);
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    if (!bucket)
        return;

    struct llist_header *node = bucket->tasks.next;
    while (node != &bucket->tasks) {
        task_t *peer = list_entry(node, task_t, tgid_node);
        node = node->next;
        task_set_pgid_locked(peer, pgid);
    }
}

static void task_set_thread_group_sid_locked(task_t *task, int64_t sid) {
    if (!task)
        return;

    uint64_t tgid = task_effective_tgid(task);
    task_index_bucket_t *bucket =
        task_index_bucket_lookup(&task_tgid_map, tgid);
    if (!bucket)
        return;

    struct llist_header *node = bucket->tasks.next;
    while (node != &bucket->tasks) {
        task_t *peer = list_entry(node, task_t, tgid_node);
        node = node->next;
        peer->sid = sid;
    }
}

extern void task_enqueue_should_free(task_t *task);
extern task_t *task_dequeue_should_free(void);

static inline struct timeval task_ns_to_timeval(uint64_t ns) {
    struct timeval tv;
    tv.tv_sec = (long)(ns / 1000000000ULL);
    tv.tv_usec = (long)((ns % 1000000000ULL) / 1000ULL);
    return tv;
}

static inline long task_ns_to_clock_ticks(uint64_t ns) {
    /*
     * Linux userspace commonly assumes _SC_CLK_TCK == 100 on modern systems.
     * Report times() in that unit to match libc expectations.
     */
    return (long)(ns / 10000000ULL);
}

void task_fd_info_put(fd_info_t *fd_info, task_t *task) {
    if (!fd_info)
        return;

    if (task_fd_info_ref_put(fd_info) > 0)
        return;

    size_t max_fds = fd_info->max_fds;
    struct vfs_file **to_close = calloc(max_fds, sizeof(*to_close));
    if (!to_close) {
        size_t scan = 0;
        while (scan < max_fds) {
            struct vfs_file *entry = NULL;
            size_t fd = scan;

            with_fd_info_lock(fd_info, {
                while (scan < fd_info->max_fds) {
                    if (fd_info->fds[scan].file) {
                        fd = scan;
                        entry = task_fd_slot_take(fd_info, (int)scan, NULL);
                        scan++;
                        break;
                    }
                    scan++;
                }
            });

            if (!entry)
                break;
            on_close_file_call(task, fd, entry);
            vfs_close_file_for_task(entry, task);
        }
        task_fd_info_free(fd_info);
        return;
    }

    with_fd_info_lock(fd_info, {
        for (size_t i = 0; i < fd_info->max_fds; i++) {
            if (!fd_info->fds[i].file)
                continue;

            struct vfs_file *entry = task_fd_slot_take(fd_info, (int)i, NULL);
            to_close[i] = entry;
        }
    });

    for (size_t i = 0; i < max_fds; i++) {
        if (!to_close[i])
            continue;
        on_close_file_call(task, i, to_close[i]);
        vfs_close_file_for_task(to_close[i], task);
    }

    free(to_close);
    task_fd_info_free(fd_info);
}

void task_cleanup_partial(task_t *task, bool kernel_mm) {
    if (!task)
        return;

    spin_lock(&task_queue_lock);
    task_detach_children_from_parent_locked(task);
    task_tgid_index_detach_locked(task);
    task_pid_index_remove_locked(task);
    task_parent_index_detach_locked(task, true);
    task_pgid_index_detach_locked(task);
    spin_unlock(&task_queue_lock);

    if (task->shm_ids && task->arch_context && task->mm)
        shm_exit(task);

    fd_info_t *fd_info = task_fd_info_detach(task);
    if (fd_info)
        task_fd_info_put(fd_info, task);

    if (task->exec_file) {
        vfs_close_file(task->exec_file);
        task->exec_file = NULL;
    }

    if (task->fs) {
        task_fs_put(task->fs);
        task->fs = NULL;
    }

    if (task->nsproxy) {
        task_ns_proxy_put(task->nsproxy);
        task->nsproxy = NULL;
    }

    if (task->signal) {
        task_signal_free(task->signal);
        task->signal = NULL;
    }

    task_keyring_release_task(task);
    task_groups_release(task);

    if (task->arch_context) {
        arch_context_free(task->arch_context);
        free(task->arch_context);
        task->arch_context = NULL;
    }

    if (task->mm) {
        if (kernel_mm) {
            free(task->mm);
        } else {
            free_page_table(task->mm);
        }
        task->mm = NULL;
    }

    if (task->syscall_stack) {
        free_frames_bytes(task->syscall_stack_base, STACK_SIZE);
        task->syscall_stack_base = NULL;
        task->syscall_stack = 0;
    }

    if (task->kernel_stack) {
        free_frames_bytes(task->kernel_stack_base, STACK_SIZE);
        task->kernel_stack_base = NULL;
        task->kernel_stack = 0;
    }

    if (task->sched_info) {
        if (task->cpu_id < MAX_CPU_NUM)
            remove_sched_entity(task, &schedulers[task->cpu_id]);
        free(task->sched_info);
        task->sched_info = NULL;
    }

    if (task->pid == 0 && task->cpu_id < MAX_CPU_NUM &&
        idle_tasks[task->cpu_id] == task) {
        idle_tasks[task->cpu_id] = NULL;
    }

    free(task);
}

static inline void task_fill_rusage(task_t *task, bool include_children,
                                    struct rusage *rusage) {
    if (!rusage)
        return;

    memset(rusage, 0, sizeof(*rusage));
    if (!task)
        return;

    uint64_t utime_ns =
        include_children ? task_total_user_ns(task) : task_self_user_ns(task);
    uint64_t stime_ns =
        include_children ? task_total_system_ns(task) : task->system_time_ns;

    rusage->ru_utime = task_ns_to_timeval(utime_ns);
    rusage->ru_stime = task_ns_to_timeval(stime_ns);
}

extern void task_timeout_cancel(task_t *task);

void free_task(task_t *ptr) {
    if (!ptr)
        return;

    if (task_is_on_cpu(ptr)) {
        task_enqueue_should_free(ptr);
        return;
    }

    task_timeout_cancel(ptr);

    spin_lock(&should_free_lock);
    if (!llist_empty(&ptr->free_node))
        llist_delete(&ptr->free_node);
    spin_unlock(&should_free_lock);

    spin_lock(&task_queue_lock);
    task_detach_children_from_parent_locked(ptr);
    task_tgid_index_detach_locked(ptr);
    task_pid_index_remove_locked(ptr);
    task_parent_index_detach_locked(ptr, true);
    task_pgid_index_detach_locked(ptr);
    spin_unlock(&task_queue_lock);

    free_frames_bytes(ptr->syscall_stack_base, STACK_SIZE);
    ptr->syscall_stack_base = NULL;
    free_frames_bytes(ptr->kernel_stack_base, STACK_SIZE);
    ptr->kernel_stack_base = NULL;

    shm_exit(ptr);

    if (!ptr->is_kernel)
        free_page_table(ptr->mm);

    ptr->arg_start = 0;
    ptr->arg_end = 0;
    ptr->env_start = 0;
    ptr->env_end = 0;

    task_keyring_release_task(ptr);
    task_groups_release(ptr);

    task_signal_free(ptr->signal);
    ptr->signal = NULL;
    arch_context_free(ptr->arch_context);
    free(ptr->arch_context);

    struct sched_entity *entity = ptr->sched_info;
    if (entity) {
        if (ptr->cpu_id < MAX_CPU_NUM)
            remove_sched_entity(ptr, &schedulers[ptr->cpu_id]);
        free(entity);
        ptr->sched_info = NULL;
    }

    free(ptr);
}

size_t task_reap_deferred(size_t budget) {
    size_t reaped = 0;

    for (size_t i = 0; i < budget; i++) {
        task_t *to_free = task_dequeue_should_free();
        if (!to_free)
            break;

        if (!task_is_on_cpu(to_free))
            reaped++;
        free_task(to_free);
    }

    return reaped;
}

#define TASK_EXEC_MAX_ARGS 4096
#define TASK_EXEC_MAX_ARG_STRLEN VFS_PATH_MAX
#define TASK_EXEC_MAX_PHDRS 128
#define TASK_EXEC_MAX_INTERP_DEPTH 8
#define TASK_EXEC_INTERP_BASE_STEP (64ULL * 1024ULL * 1024ULL)

static void task_execve_free_string_array(char **strings, int count) {
    if (!strings) {
        return;
    }

    for (int i = 0; i < count; i++) {
        if (strings[i]) {
            free(strings[i]);
        }
    }

    free(strings);
}

static int task_execve_read_user_string_ptr(const char **strings, int index,
                                            const char **out) {
    if (!strings || !out || index < 0)
        return -EINVAL;
    memcpy(out, &strings[index], sizeof(*out));
    return 0;
}

static int task_execve_copy_user_string(const char *user, char **out) {
    char *copy;

    if (!user || !out)
        return -EINVAL;

    copy = calloc(1, TASK_EXEC_MAX_ARG_STRLEN);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, user, TASK_EXEC_MAX_ARG_STRLEN);

    *out = copy;
    return 0;
}

typedef struct task_execve_creds {
    int64_t uid;
    int64_t gid;
    int64_t euid;
    int64_t egid;
    int64_t suid;
    int64_t sgid;
    int64_t fsuid;
    int64_t fsgid;
    bool secure_exec;
} task_execve_creds_t;

typedef struct task_user_cap_header {
    uint32_t version;
    int32_t pid;
} task_user_cap_header_t;

typedef struct task_user_cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} task_user_cap_data_t;

#define LINUX_CAPABILITY_VERSION_1 0x19980330
#define LINUX_CAPABILITY_VERSION_2 0x20071026
#define LINUX_CAPABILITY_VERSION_3 0x20080522
#define TASK_CAP_LAST_CAP 40U
#define TASK_CAP_FULL_MASK ((UINT64_C(1) << (TASK_CAP_LAST_CAP + 1U)) - 1U)

typedef struct task_execve_elf_image {
    uint64_t load_base;
    uint64_t entry;
    uint64_t phdr_vaddr;
    uint64_t phnum;
} task_execve_elf_image_t;

typedef struct task_execve_interp_result {
    bool present;
    uint64_t entry;
    uint64_t at_base;
    bool aux_override;
    uint64_t aux_entry;
    uint64_t aux_phdr;
    uint64_t aux_phnum;
} task_execve_interp_result_t;

static task_execve_creds_t task_execve_prepare_creds(task_t *task,
                                                     struct vfs_file *file) {
    task_execve_creds_t creds = {
        .uid = task->uid,
        .gid = task->gid,
        .euid = task->euid,
        .egid = task->egid,
        .suid = task->suid,
        .sgid = task->sgid,
        .fsuid = task->fsuid,
        .fsgid = task->fsgid,
        .secure_exec = false,
    };
    struct vfs_inode *inode = file ? file->f_inode : NULL;
    bool nosuid = file && file->f_path.mnt &&
                  (file->f_path.mnt->mnt_flags & VFS_MNT_NOSUID);

    if (inode && !nosuid && S_ISREG(inode->i_mode)) {
        if (inode->i_mode & S_ISUID)
            creds.euid = inode->i_uid;

        if (inode->i_mode & S_ISGID)
            creds.egid = inode->i_gid;
    }

    creds.suid = creds.euid;
    creds.sgid = creds.egid;
    creds.fsuid = creds.euid;
    creds.fsgid = creds.egid;
    creds.secure_exec = creds.euid != task->euid || creds.egid != task->egid;
    return creds;
}

static void task_execve_commit_creds(task_t *task,
                                     const task_execve_creds_t *creds) {
    bool dropped_root;

    if (!task || !creds)
        return;

    dropped_root = task->euid == 0 && creds->euid != 0;

    task->uid = creds->uid;
    task->gid = creds->gid;
    task->euid = creds->euid;
    task->egid = creds->egid;
    task->suid = creds->suid;
    task->sgid = creds->sgid;
    task->fsuid = creds->fsuid;
    task->fsgid = creds->fsgid;

    if (dropped_root && !task->keep_caps) {
        task->cap_effective = 0;
        task->cap_permitted = 0;
        task->cap_ambient = 0;
    }
}

static int task_capability_u32_count(uint32_t version) {
    switch (version) {
    case LINUX_CAPABILITY_VERSION_1:
        return 1;
    case LINUX_CAPABILITY_VERSION_2:
    case LINUX_CAPABILITY_VERSION_3:
        return 2;
    default:
        return -EINVAL;
    }
}

static bool task_capability_pid_matches(int32_t pid) {
    if (!current_task)
        return false;
    return pid == 0 || pid == (int32_t)current_task->pid;
}

static inline uint64_t task_capability_mask(void) { return TASK_CAP_FULL_MASK; }

static inline bool task_capability_valid(unsigned long cap) {
    return cap <= TASK_CAP_LAST_CAP;
}

static inline uint64_t task_capability_bit(unsigned long cap) {
    return UINT64_C(1) << cap;
}

static inline void task_capability_clear_root_ambient(task_t *task) {
    if (!task)
        return;
    task->cap_ambient = 0;
}

static int task_capability_apply_sets(task_t *task, uint64_t effective,
                                      uint64_t permitted,
                                      uint64_t inheritable) {
    uint64_t mask = task_capability_mask();

    if (!task)
        return -EINVAL;

    effective &= mask;
    permitted &= mask;
    inheritable &= mask;

    if ((effective & ~permitted) != 0)
        return -EPERM;

    task->cap_effective = effective;
    task->cap_permitted = permitted;
    task->cap_inheritable = inheritable;
    task->cap_ambient &= permitted & inheritable;
    return 0;
}

uint64_t sys_capget(void *header_user, void *data_user) {
    task_user_cap_header_t header;

    if (!header_user || copy_from_user(&header, header_user, sizeof(header)))
        return (uint64_t)-EFAULT;

    int u32_count = task_capability_u32_count(header.version);
    if (u32_count < 0) {
        uint32_t preferred = LINUX_CAPABILITY_VERSION_3;
        copy_to_user(&((task_user_cap_header_t *)header_user)->version,
                     &preferred, sizeof(preferred));
        return (uint64_t)-EINVAL;
    }
    if (!task_capability_pid_matches(header.pid))
        return (uint64_t)-ESRCH;
    if (!data_user)
        return 0;

    task_user_cap_data_t data[2] = {0};
    uint64_t effective = current_task ? current_task->cap_effective : 0;
    uint64_t permitted = current_task ? current_task->cap_permitted : 0;
    uint64_t inheritable = current_task ? current_task->cap_inheritable : 0;

    data[0].effective = (uint32_t)(effective & UINT32_MAX);
    data[0].permitted = (uint32_t)(permitted & UINT32_MAX);
    data[0].inheritable = (uint32_t)(inheritable & UINT32_MAX);
    if (u32_count > 1) {
        data[1].effective = (uint32_t)(effective >> 32);
        data[1].permitted = (uint32_t)(permitted >> 32);
        data[1].inheritable = (uint32_t)(inheritable >> 32);
    }

    if (copy_to_user(data_user, data, sizeof(data[0]) * (size_t)u32_count))
        return (uint64_t)-EFAULT;
    return 0;
}

uint64_t sys_capset(void *header_user, const void *data_user) {
    task_user_cap_header_t header;

    if (!header_user || !data_user ||
        copy_from_user(&header, header_user, sizeof(header)))
        return (uint64_t)-EFAULT;

    int u32_count = task_capability_u32_count(header.version);
    if (u32_count < 0) {
        uint32_t preferred = LINUX_CAPABILITY_VERSION_3;
        copy_to_user(&((task_user_cap_header_t *)header_user)->version,
                     &preferred, sizeof(preferred));
        return (uint64_t)-EINVAL;
    }
    if (!task_capability_pid_matches(header.pid))
        return (uint64_t)-ESRCH;

    task_user_cap_data_t data[2] = {0};
    if (copy_from_user(data, data_user, sizeof(data[0]) * (size_t)u32_count))
        return (uint64_t)-EFAULT;

    uint64_t effective = (uint64_t)data[0].effective;
    uint64_t permitted = (uint64_t)data[0].permitted;
    uint64_t inheritable = (uint64_t)data[0].inheritable;

    if (u32_count > 1) {
        effective |= ((uint64_t)data[1].effective) << 32;
        permitted |= ((uint64_t)data[1].permitted) << 32;
        inheritable |= ((uint64_t)data[1].inheritable) << 32;
    }

    return (uint64_t)task_capability_apply_sets(current_task, effective,
                                                permitted, inheritable);
}

static uint64_t simple_rand() {
    uint32_t seed = boot_get_boottime() * 100 + nano_time() / 10;
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
    return ((uint64_t)seed << 32) | seed;
}

#define ALIGN_STACK_DOWN(stack_ptr, alignment)                                 \
    stack_ptr = (stack_ptr) & ~((alignment) - 1)

static bool push_user_bytes(uint64_t *stack_ptr, const void *data, size_t len) {
    *stack_ptr -= len;
    return copy_to_user((void *)(uintptr_t)*stack_ptr, data, len);
}

static bool push_user_u64(uint64_t *stack_ptr, uint64_t value) {
    return push_user_bytes(stack_ptr, &value, sizeof(value));
}

bool push_infos(task_t *task, uint64_t current_stack, char *argv[],
                int argv_count, char *envp[], int envp_count, uint64_t e_entry,
                uint64_t phdr, uint64_t phnum, uint64_t at_base,
                const char *execfn, const task_execve_creds_t *creds,
                uint64_t *stack_out) {
    uint64_t tmp_stack = current_stack;
    uint64_t arg_low = UINT64_MAX;
    uint64_t arg_high = 0;
    uint64_t env_low = UINT64_MAX;
    uint64_t env_high = 0;
    uint64_t *envp_addrs = NULL;
    uint64_t *argv_addrs = NULL;
    bool failed = true;

    if (!stack_out)
        return false;

    arch_enable_user_access();

    const char *execfn_name = execfn ? execfn : task->name;
    size_t name_len = strlen(execfn_name) + 1;
    if (push_user_bytes(&tmp_stack, execfn_name, name_len))
        goto out;
    uint64_t execfn_ptr = tmp_stack;

    uint64_t random_values[2] = {simple_rand(), simple_rand()};
    if (push_user_bytes(&tmp_stack, random_values, sizeof(random_values)))
        goto out;
    uint64_t random_ptr = tmp_stack;

    if (envp_count > 0 && envp != NULL) {
        envp_addrs = (uint64_t *)malloc(envp_count * sizeof(uint64_t));
        if (!envp_addrs)
            goto out;

        for (int i = envp_count - 1; i >= 0; i--) {
            size_t len = strlen(envp[i]) + 1;
            if (push_user_bytes(&tmp_stack, envp[i], len))
                goto out;
            envp_addrs[i] = tmp_stack;
            if (tmp_stack < env_low)
                env_low = tmp_stack;
            if (tmp_stack + len > env_high)
                env_high = tmp_stack + len;
        }
    }

    if (argv_count > 0 && argv != NULL) {
        argv_addrs = (uint64_t *)malloc(argv_count * sizeof(uint64_t));
        if (!argv_addrs)
            goto out;

        // 从后向前推送
        for (int i = argv_count - 1; i >= 0; i--) {
            size_t len = strlen(argv[i]) + 1;
            if (push_user_bytes(&tmp_stack, argv[i], len))
                goto out;
            argv_addrs[i] = tmp_stack;
            if (tmp_stack < arg_low)
                arg_low = tmp_stack;
            if (tmp_stack + len > arg_high)
                arg_high = tmp_stack + len;
        }
    }

    task->arg_start = 0;
    task->arg_end = 0;
    task->env_start = 0;
    task->env_end = 0;

    if (argv_count > 0 && argv_addrs != NULL && arg_low < arg_high) {
        task->arg_start = arg_low;
        task->arg_end = arg_high;
    }

    if (envp_count > 0 && envp_addrs != NULL && env_low < env_high) {
        task->env_start = env_low;
        task->env_end = env_high;
    }

    const size_t auxv_pairs = 16;
    size_t qwords_to_push =
        auxv_pairs * 2 + (size_t)argv_count + (size_t)envp_count + 3;

    tmp_stack &= ~0xFULL;
    if (qwords_to_push & 1)
        tmp_stack -= sizeof(uint64_t);

    if (push_user_u64(&tmp_stack, 0) || push_user_u64(&tmp_stack, AT_NULL) ||

        push_user_u64(&tmp_stack, execfn_ptr) ||
        push_user_u64(&tmp_stack, AT_EXECFN) ||

        push_user_u64(&tmp_stack, random_ptr) ||
        push_user_u64(&tmp_stack, AT_RANDOM) ||

        push_user_u64(&tmp_stack, creds ? creds->egid : task->egid) ||
        push_user_u64(&tmp_stack, AT_EGID) ||

        push_user_u64(&tmp_stack, creds ? creds->gid : task->gid) ||
        push_user_u64(&tmp_stack, AT_GID) ||

        push_user_u64(&tmp_stack, creds ? creds->euid : task->euid) ||
        push_user_u64(&tmp_stack, AT_EUID) ||

        push_user_u64(&tmp_stack, creds ? creds->uid : task->uid) ||
        push_user_u64(&tmp_stack, AT_UID) ||

        push_user_u64(&tmp_stack, e_entry) ||
        push_user_u64(&tmp_stack, AT_ENTRY) ||

        push_user_u64(&tmp_stack, 0) || push_user_u64(&tmp_stack, AT_FLAGS) ||

        push_user_u64(&tmp_stack, creds && creds->secure_exec ? 1 : 0) ||
        push_user_u64(&tmp_stack, AT_SECURE) ||

        push_user_u64(&tmp_stack, phnum) ||
        push_user_u64(&tmp_stack, AT_PHNUM) ||

        push_user_u64(&tmp_stack, sizeof(Elf64_Phdr)) ||
        push_user_u64(&tmp_stack, AT_PHENT) ||

        push_user_u64(&tmp_stack, phdr) || push_user_u64(&tmp_stack, AT_PHDR) ||

        push_user_u64(&tmp_stack, PAGE_SIZE) ||
        push_user_u64(&tmp_stack, AT_PAGESZ) ||

        push_user_u64(&tmp_stack, SCHED_HZ) ||
        push_user_u64(&tmp_stack, AT_CLKTCK) ||

        push_user_u64(&tmp_stack, at_base) ||
        push_user_u64(&tmp_stack, AT_BASE))
        goto out;

    // NULL 结束标记
    if (push_user_u64(&tmp_stack, 0))
        goto out;

    if (envp_count > 0 && envp_addrs != NULL) {
        for (int i = envp_count - 1; i >= 0; i--) {
            if (push_user_u64(&tmp_stack, envp_addrs[i]))
                goto out;
        }
    }

    // NULL 结束标记
    if (push_user_u64(&tmp_stack, 0))
        goto out;

    if (argv_count > 0 && argv_addrs != NULL) {
        for (int i = argv_count - 1; i >= 0; i--) {
            if (push_user_u64(&tmp_stack, argv_addrs[i]))
                goto out;
        }
    }

    if (push_user_u64(&tmp_stack, argv_count))
        goto out;

    *stack_out = tmp_stack;
    failed = false;

out:
    if (argv_addrs)
        free(argv_addrs);
    if (envp_addrs)
        free(envp_addrs);

    arch_disable_user_access();

    return !failed;
}

static int register_elf_load_vma(task_t *task, vfs_node_t *node,
                                 const char *name, uint64_t load_base,
                                 const Elf64_Phdr *phdr) {
    if (!task || !task->mm || !phdr || phdr->p_type != PT_LOAD ||
        phdr->p_memsz == 0) {
        return 0;
    }

    uint64_t seg_addr = load_base + phdr->p_vaddr;
    uint64_t aligned_addr = PADDING_DOWN(seg_addr, PAGE_SIZE);
    uint64_t aligned_offset = PADDING_DOWN(phdr->p_offset, PAGE_SIZE);
    uint64_t page_prefix = seg_addr - aligned_addr;
    uint64_t file_map_size = phdr->p_filesz + page_prefix;
    uint64_t mem_map_size = phdr->p_memsz + page_prefix;
    uint64_t map_size = PADDING_UP(mem_map_size, PAGE_SIZE);

    if (file_map_size < phdr->p_filesz || mem_map_size < phdr->p_memsz ||
        map_size < mem_map_size || phdr->p_filesz > phdr->p_memsz) {
        return -EINVAL;
    }

    vma_t *vma = vma_alloc();
    if (!vma)
        return -ENOMEM;

    vma->vm_start = aligned_addr;
    vma->vm_end = aligned_addr + map_size;
    vma->vm_flags = elf_segment_vma_flags(phdr->p_flags);
    vma->vm_type = VMA_TYPE_FILE;
    vma->vm_offset = aligned_offset;
    vma->vm_file_len = file_map_size;
    vma->vm_file_flags = 0;
    vma->node = node;
    if (node)
        vfs_igrab(node);
    if (name)
        vma->vm_name = strdup(name);

    if (vma_insert(&task->mm->task_vma_mgr, vma) != 0) {
        vma_free(vma);
        return -ENOMEM;
    }

    return 0;
}

static int task_execve_read_interp_path(struct vfs_file *file,
                                        const Elf64_Phdr *phdr,
                                        char **out_path) {
    if (!file || !phdr || !out_path || phdr->p_type != PT_INTERP)
        return -EINVAL;
    if (phdr->p_filesz == 0 || phdr->p_filesz > VFS_PATH_MAX)
        return -ENOEXEC;

    char *path = (char *)calloc(1, (size_t)phdr->p_filesz + 1);
    if (!path)
        return -ENOMEM;

    loff_t pos = (loff_t)phdr->p_offset;
    if (vfs_read_kernel_file(file, path, phdr->p_filesz, &pos) !=
        (ssize_t)phdr->p_filesz) {
        free(path);
        return -ENOEXEC;
    }

    path[phdr->p_filesz] = '\0';
    if (path[0] == '\0') {
        free(path);
        return -ENOEXEC;
    }

    *out_path = path;
    return 0;
}

static int task_execve_read_elf_phdrs(struct vfs_file *file,
                                      const Elf64_Ehdr *ehdr,
                                      Elf64_Phdr **out_phdr) {
    if (!file || !ehdr || !out_phdr)
        return -EINVAL;
    if (!arch_check_elf(ehdr) || ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
        ehdr->e_phnum == 0 || ehdr->e_phnum > TASK_EXEC_MAX_PHDRS ||
        ehdr->e_phoff >
            UINT64_MAX - (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr)) {
        return -ENOEXEC;
    }
    if (ehdr->e_type != ET_DYN && ehdr->e_type != ET_EXEC)
        return -ENOEXEC;

    size_t phdr_size = (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdr = (Elf64_Phdr *)malloc(phdr_size);
    if (!phdr)
        return -ENOMEM;

    loff_t pos = (loff_t)ehdr->e_phoff;
    if (vfs_read_kernel_file(file, phdr, phdr_size, &pos) !=
        (ssize_t)phdr_size) {
        free(phdr);
        return -ENOEXEC;
    }

    *out_phdr = phdr;
    return 0;
}

static int task_execve_load_elf_image(task_t *task, struct vfs_file *file,
                                      const char *path, const Elf64_Ehdr *ehdr,
                                      const Elf64_Phdr *phdr,
                                      uint64_t requested_base,
                                      task_execve_elf_image_t *image) {
    if (!task || !file || !ehdr || !phdr || !image)
        return -EINVAL;

    uint64_t load_base = ehdr->e_type == ET_DYN ? requested_base : 0;
    uint64_t load_start = UINT64_MAX;
    uint64_t phdr_vaddr = 0;
    bool has_load = false;

    memset(image, 0, sizeof(*image));

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_PHDR) {
            phdr_vaddr = load_base + phdr[i].p_vaddr;
            continue;
        }
        if (phdr[i].p_type != PT_LOAD)
            continue;

        uint64_t seg_addr = load_base + phdr[i].p_vaddr;
        uint64_t aligned_addr = PADDING_DOWN(seg_addr, PAGE_SIZE);
        if (aligned_addr < load_start)
            load_start = aligned_addr;

        int ret = register_elf_load_vma(task, file->f_inode, path, load_base,
                                        &phdr[i]);
        if (ret < 0)
            return ret;
        has_load = true;
    }

    if (!has_load)
        return -ENOEXEC;
    if (!phdr_vaddr)
        phdr_vaddr = load_start + ehdr->e_phoff;

    image->load_base = load_base;
    image->entry = load_base + ehdr->e_entry;
    image->phdr_vaddr = phdr_vaddr;
    image->phnum = ehdr->e_phnum;
    return 0;
}

static int task_execve_find_interp_path(struct vfs_file *file,
                                        const Elf64_Phdr *phdr, int phnum,
                                        char **out_path) {
    char *path = NULL;

    if (!file || !phdr || !out_path)
        return -EINVAL;

    for (int i = 0; i < phnum; i++) {
        if (phdr[i].p_type != PT_INTERP)
            continue;
        if (path) {
            free(path);
            return -ENOEXEC;
        }

        int ret = task_execve_read_interp_path(file, &phdr[i], &path);
        if (ret < 0)
            return ret;
    }

    *out_path = path;
    return 0;
}

static uint64_t task_execve_interp_base_at_depth(uint64_t first_base,
                                                 int depth) {
    uint64_t offset = (uint64_t)depth * TASK_EXEC_INTERP_BASE_STEP;

    if (first_base > offset)
        return first_base - offset;
    return first_base;
}

static int task_execve_load_interpreter_chain(
    task_t *task, const struct vfs_path *from_path, const char *interp_path,
    uint64_t first_base, int depth, task_execve_interp_result_t *result) {
    if (!task || !from_path || !interp_path || !result)
        return -EINVAL;
    if (depth >= TASK_EXEC_MAX_INTERP_DEPTH)
        return -ELOOP;

    struct vfs_open_how exec_how = {.flags = O_RDONLY};
    struct vfs_file *file = NULL;
    int ret;

    if (interp_path[0] == '/') {
        ret = vfs_openat(AT_FDCWD, interp_path, &exec_how, &file, true);
    } else {
        struct vfs_path start = {0};

        if (!vfs_path_set(&start, from_path->mnt, from_path->dentry->d_parent))
            return -ENOENT;
        ret = vfs_open_from(&start, interp_path, &exec_how, &file, true);
        vfs_path_put(&start);
    }
    if (ret < 0 || !file)
        return ret < 0 ? ret : -ENOENT;

    Elf64_Ehdr ehdr;
    loff_t hdr_pos = 0;
    if (vfs_read_kernel_file(file, &ehdr, sizeof(ehdr), &hdr_pos) !=
        (ssize_t)sizeof(ehdr)) {
        ret = -ENOEXEC;
        goto out_close;
    }

    Elf64_Phdr *phdr = NULL;
    ret = task_execve_read_elf_phdrs(file, &ehdr, &phdr);
    if (ret < 0)
        goto out_close;

    task_execve_elf_image_t image;
    uint64_t load_base = task_execve_interp_base_at_depth(first_base, depth);
    ret = task_execve_load_elf_image(task, file, interp_path, &ehdr, phdr,
                                     load_base, &image);
    if (ret < 0)
        goto out_free_phdr;

    char *next_interp_path = NULL;
    ret = task_execve_find_interp_path(file, phdr, ehdr.e_phnum,
                                       &next_interp_path);
    if (ret < 0)
        goto out_free_phdr;

    memset(result, 0, sizeof(*result));
    result->present = true;

    if (next_interp_path) {
        task_execve_interp_result_t next_result;
        ret = task_execve_load_interpreter_chain(task, &file->f_path,
                                                 next_interp_path, first_base,
                                                 depth + 1, &next_result);
        free(next_interp_path);
        if (ret < 0)
            goto out_free_phdr;

        result->entry = next_result.entry;
        result->at_base = next_result.at_base;
        result->aux_override = true;
        if (next_result.aux_override) {
            result->aux_entry = next_result.aux_entry;
            result->aux_phdr = next_result.aux_phdr;
            result->aux_phnum = next_result.aux_phnum;
        } else {
            result->aux_entry = image.entry;
            result->aux_phdr = image.phdr_vaddr;
            result->aux_phnum = image.phnum;
        }
    } else {
        result->entry = image.entry;
        result->at_base = image.load_base;
    }

out_free_phdr:
    free(phdr);
out_close:
    vfs_close_file(file);
    return ret;
}

static int task_execve_dethread(task_t *self) {
    if (!self)
        return -EINVAL;

    uint64_t tgid = task_effective_tgid(self);
    if (task_thread_group_count(tgid) <= 1)
        return 0;

    if (self->pid != tgid)
        return -ENOSYS;

    while (true) {
        bool others_alive = false;

        spin_lock(&task_queue_lock);
        task_index_bucket_t *bucket =
            task_index_bucket_lookup(&task_tgid_map, tgid);
        if (bucket) {
            struct llist_header *node = bucket->tasks.next;
            while (node != &bucket->tasks) {
                task_t *task = list_entry(node, task_t, tgid_node);
                node = node->next;
                if (!task || task == self || task->state == TASK_DIED ||
                    !task->arch_context)
                    continue;

                others_alive = true;
                task_send_signal(task, SIGKILL, SI_DETHREAD);
                if (task->state == TASK_BLOCKING ||
                    task->state == TASK_UNINTERRUPTABLE) {
                    task_unblock(task, EOK);
                }
            }
        }
        spin_unlock(&task_queue_lock);

        if (!others_alive)
            return 0;

        arch_enable_interrupt();
        arch_wait_for_interrupt();
        arch_disable_interrupt();
    }
}

static int task_execve_copy_path(const char *path_src, char *path_dst,
                                 size_t path_len) {
    if (!path_src || !path_dst || path_len == 0)
        return -EFAULT;

    if (!check_user_overflow((uint64_t)path_src, 1)) {
        if (copy_from_user_str(path_dst, path_src, path_len))
            return -EFAULT;
    } else {
        strncpy(path_dst, path_src, path_len);
        path_dst[path_len - 1] = '\0';
    }

    return 0;
}

static int task_execve_build_display_path(int dirfd, const char *pathname,
                                          uint64_t flags, char *display_path,
                                          size_t display_len) {
    int written;

    if (!pathname || !display_path || display_len == 0)
        return -EINVAL;

    if ((flags & AT_EMPTY_PATH) && pathname[0] == '\0') {
        written = snprintf(display_path, display_len, "/dev/fd/%d", dirfd);
    } else if (pathname[0] == '/' || dirfd == AT_FDCWD) {
        written = snprintf(display_path, display_len, "%s", pathname);
    } else {
        written = snprintf(display_path, display_len, "/dev/fd/%d/%s", dirfd,
                           pathname);
    }

    if (written < 0 || (size_t)written >= display_len)
        return -ENAMETOOLONG;

    return 0;
}

static int task_execve_open_file_from_path(const struct vfs_path *path,
                                           unsigned int open_flags,
                                           struct vfs_file **out) {
    struct vfs_file *file;
    int ret = 0;

    if (!path || !out)
        return -EINVAL;

    file = vfs_alloc_file(path, open_flags);
    if (!file)
        return -ENOMEM;

    if (!(open_flags & O_PATH) && file->f_op && file->f_op->open) {
        ret = file->f_op->open(file->f_inode, file);
        if (ret < 0) {
            vfs_file_put(file);
            return ret;
        }
    }

    *out = file;
    return 0;
}

static int task_execve_open_exec_file(int dirfd, const char *pathname,
                                      uint64_t flags, char *display_path,
                                      size_t display_len,
                                      struct vfs_file **out_exec_file) {
    static const uint64_t allowed_flags = AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW;
    struct vfs_open_how exec_how = {.flags = O_RDONLY};
    struct vfs_file *fd_file = NULL;
    int ret;

    if (!pathname || !display_path || !out_exec_file)
        return -EINVAL;
    if (flags & ~allowed_flags)
        return -EINVAL;

    if ((flags & AT_EMPTY_PATH) && pathname[0] == '\0') {
        fd_file = task_get_file(current_task, dirfd);
        if (!fd_file)
            return -EBADF;

        ret = task_execve_build_display_path(dirfd, pathname, flags,
                                             display_path, display_len);
        if (ret < 0) {
            vfs_file_put(fd_file);
            return ret;
        }

        ret = task_execve_open_file_from_path(&fd_file->f_path, exec_how.flags,
                                              out_exec_file);
        vfs_file_put(fd_file);
        return ret;
    }

    if (flags & AT_SYMLINK_NOFOLLOW)
        exec_how.flags |= O_NOFOLLOW;

    ret = vfs_openat(dirfd, pathname, &exec_how, out_exec_file, true);
    if (ret < 0)
        return ret;

    ret = task_execve_build_display_path(dirfd, pathname, flags, display_path,
                                         display_len);
    if (ret < 0) {
        vfs_close_file(*out_exec_file);
        *out_exec_file = NULL;
        return ret;
    }

    return 0;
}

static void task_execve_switch_mm(task_t *task, task_mm_info_t *from,
                                  task_mm_info_t *to) {
    bool irq_state = arch_interrupt_enabled();

    arch_disable_interrupt();
    spin_lock(&task_queue_lock);
    task->mm = to;
    spin_unlock(&task_queue_lock);
    set_current_page_dir(true, to->page_table_addr);
    task_mm_mark_cpu_inactive(from, task->cpu_id);
    task_mm_mark_cpu_active(to, task->cpu_id);

    if (irq_state)
        arch_enable_interrupt();
}

static uint64_t task_do_execve(int dirfd, const char *path_user,
                               const char **argv, const char **envp,
                               uint64_t flags) {
    task_t *self = current_task;
    uint64_t exec_fail_ret = (uint64_t)-ENOEXEC;
    char *interpreter_path = NULL;
    int open_ret;

    char lookup_path[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
    open_ret =
        task_execve_copy_path(path_user, lookup_path, sizeof(lookup_path));
    if (open_ret < 0) {
        return (uint64_t)open_ret;
    }

    struct vfs_file *exec_file = NULL;
    open_ret = task_execve_open_exec_file(dirfd, lookup_path, flags, path,
                                          sizeof(path), &exec_file);
    if (open_ret < 0 || !exec_file) {
        return (uint64_t)open_ret;
    }
    open_ret = vfs_file_get_exec_access(exec_file);
    if (open_ret < 0) {
        vfs_close_file(exec_file);
        return (uint64_t)open_ret;
    }
    struct vfs_open_how exec_how = {.flags = O_RDONLY};
    vfs_node_t *node = exec_file->f_inode;

    int argv_count = 0;
    int envp_count = 0;

    if (argv && (translate_address((uint64_t *)phys_to_virt(
                                       current_task->mm->page_table_addr),
                                   (uint64_t)argv) != 0)) {
        for (argv_count = 0;
             argv[argv_count] != NULL &&
             (translate_address(
                  (uint64_t *)phys_to_virt(current_task->mm->page_table_addr),
                  (uint64_t)argv[argv_count]) != 0);
             argv_count++) {
        }
    }

    if (envp && (translate_address((uint64_t *)phys_to_virt(
                                       current_task->mm->page_table_addr),
                                   (uint64_t)envp) != 0)) {
        for (envp_count = 0;
             envp[envp_count] != NULL &&
             (translate_address(
                  (uint64_t *)phys_to_virt(current_task->mm->page_table_addr),
                  (uint64_t)envp[envp_count]) != 0);
             envp_count++) {
        }
    }

    char **new_argv = (char **)malloc((argv_count + 1) * sizeof(char *));
    if (!new_argv) {
        vfs_close_file(exec_file);
        return (uint64_t)-ENOMEM;
    }
    memset(new_argv, 0, (argv_count + 1) * sizeof(char *));
    char **new_envp = (char **)malloc((envp_count + 1) * sizeof(char *));
    if (!new_envp) {
        free(new_argv);
        vfs_close_file(exec_file);
        return (uint64_t)-ENOMEM;
    }
    memset(new_envp, 0, (envp_count + 1) * sizeof(char *));

    argv_count = 0;
    envp_count = 0;

    if (argv && (translate_address((uint64_t *)phys_to_virt(
                                       current_task->mm->page_table_addr),
                                   (uint64_t)argv) != 0)) {
        for (argv_count = 0;
             argv[argv_count] != NULL &&
             (translate_address(
                  (uint64_t *)phys_to_virt(current_task->mm->page_table_addr),
                  (uint64_t)argv[argv_count]) != 0);
             argv_count++) {
            new_argv[argv_count] = strdup(argv[argv_count]);
        }
    }
    new_argv[argv_count] = NULL;

    if (envp && (translate_address((uint64_t *)phys_to_virt(
                                       current_task->mm->page_table_addr),
                                   (uint64_t)envp) != 0)) {
        for (envp_count = 0;
             envp[envp_count] != NULL &&
             (translate_address(
                  (uint64_t *)phys_to_virt(current_task->mm->page_table_addr),
                  (uint64_t)envp[envp_count]) != 0);
             envp_count++) {
            new_envp[envp_count] = strdup(envp[envp_count]);
        }
    }
    new_envp[envp_count] = NULL;

    uint8_t header_buf[512];
    ssize_t header_read;
    int shebang_depth = 0;
    bool shell_fallback_used = false;

    while (true) {
        loff_t header_pos = 0;
        header_read = vfs_read_kernel_file(exec_file, header_buf,
                                           sizeof(header_buf), &header_pos);

        if (header_read < 2 || header_buf[0] != '#' || header_buf[1] != '!') {
            break;
        }

        if (++shebang_depth > 4) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ELOOP;
        }

        size_t shebang_len = (header_read < (ssize_t)sizeof(header_buf))
                                 ? (size_t)header_read
                                 : sizeof(header_buf) - 1;
        char *line_start = (char *)header_buf + 2;
        char *line_limit = (char *)header_buf + shebang_len;
        char *line_end = line_start;
        char *interpreter_name;
        char *interpreter_end;
        char *optional_arg = NULL;
        char script_path[sizeof(path)];
        char **replaced_argv = NULL;
        int replaced_argc = 2;
        int replaced_index = 0;
        bool found_newline = false;

        header_buf[shebang_len] = '\0';

        while (line_end < line_limit && *line_end != '\n' &&
               *line_end != '\0') {
            line_end++;
        }
        if (line_end < line_limit && *line_end == '\n') {
            found_newline = true;
        }

        while (line_start < line_end &&
               (*line_start == ' ' || *line_start == '\t')) {
            line_start++;
        }
        while (line_end > line_start &&
               (line_end[-1] == ' ' || line_end[-1] == '\t' ||
                line_end[-1] == '\r')) {
            line_end--;
        }
        *line_end = '\0';

        if (line_start == line_end) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOEXEC;
        }

        interpreter_name = line_start;
        interpreter_end = interpreter_name;
        while (interpreter_end < line_end && *interpreter_end != ' ' &&
               *interpreter_end != '\t') {
            interpreter_end++;
        }

        if (!found_newline && header_read >= (ssize_t)sizeof(header_buf) &&
            interpreter_end == line_end) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOEXEC;
        }

        if (interpreter_end == interpreter_name) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOEXEC;
        }

        if (interpreter_end < line_end) {
            *interpreter_end++ = '\0';
            while (interpreter_end < line_end &&
                   (*interpreter_end == ' ' || *interpreter_end == '\t')) {
                interpreter_end++;
            }
            if (interpreter_end < line_end) {
                optional_arg = interpreter_end;
                replaced_argc++;
            }
        }

        if (argv_count > 1) {
            replaced_argc += argv_count - 1;
        }

        replaced_argv = (char **)calloc(replaced_argc + 1, sizeof(char *));
        if (!replaced_argv) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOMEM;
        }

        strncpy(script_path, path, sizeof(script_path));
        script_path[sizeof(script_path) - 1] = '\0';

        replaced_argv[replaced_index++] = strdup(interpreter_name);
        if (!replaced_argv[0]) {
            task_execve_free_string_array(replaced_argv, replaced_index);
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOMEM;
        }

        if (optional_arg) {
            replaced_argv[replaced_index++] = strdup(optional_arg);
            if (!replaced_argv[replaced_index - 1]) {
                task_execve_free_string_array(replaced_argv, replaced_index);
                task_execve_free_string_array(new_argv, argv_count);
                task_execve_free_string_array(new_envp, envp_count);
                vfs_close_file(exec_file);
                return (uint64_t)-ENOMEM;
            }
        }

        replaced_argv[replaced_index++] = strdup(script_path);
        if (!replaced_argv[replaced_index - 1]) {
            task_execve_free_string_array(replaced_argv, replaced_index);
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOMEM;
        }

        for (int i = 1; i < argv_count; i++) {
            replaced_argv[replaced_index++] = strdup(new_argv[i]);
            if (!replaced_argv[replaced_index - 1]) {
                task_execve_free_string_array(replaced_argv, replaced_index);
                task_execve_free_string_array(new_argv, argv_count);
                task_execve_free_string_array(new_envp, envp_count);
                vfs_close_file(exec_file);
                return (uint64_t)-ENOMEM;
            }
        }

        struct vfs_file *interpreter_file = NULL;
        if (vfs_openat(AT_FDCWD, interpreter_name, &exec_how, &interpreter_file,
                       true) < 0 ||
            !interpreter_file) {
            task_execve_free_string_array(replaced_argv, replaced_index);
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOENT;
        }

        task_execve_free_string_array(new_argv, argv_count);
        new_argv = replaced_argv;
        argv_count = replaced_argc;

        int exec_access_ret = vfs_file_get_exec_access(interpreter_file);
        if (exec_access_ret < 0) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(interpreter_file);
            vfs_close_file(exec_file);
            return (uint64_t)exec_access_ret;
        }

        strncpy(path, interpreter_name, sizeof(path));
        path[sizeof(path) - 1] = '\0';
        vfs_close_file(exec_file);
        exec_file = interpreter_file;
        node = interpreter_file->f_inode;
    }

    if (header_read < (ssize_t)sizeof(Elf64_Ehdr) ||
        !arch_check_elf((const Elf64_Ehdr *)header_buf)) {
        const char *shell_path = "/bin/sh";
        char script_path[sizeof(path)];
        int replaced_argc = 2;
        int replaced_index = 0;
        char **replaced_argv;
        struct vfs_file *shell_file = NULL;

        if (shell_fallback_used) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOEXEC;
        }

        if (argv_count > 1)
            replaced_argc += argv_count - 1;

        replaced_argv = (char **)calloc(replaced_argc + 1, sizeof(char *));
        if (!replaced_argv) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOMEM;
        }

        strncpy(script_path, path, sizeof(script_path));
        script_path[sizeof(script_path) - 1] = '\0';

        replaced_argv[replaced_index++] = strdup(shell_path);
        if (!replaced_argv[replaced_index - 1])
            goto shell_fallback_nomem;

        replaced_argv[replaced_index++] = strdup(script_path);
        if (!replaced_argv[replaced_index - 1])
            goto shell_fallback_nomem;

        for (int i = 1; i < argv_count; i++) {
            replaced_argv[replaced_index++] = strdup(new_argv[i]);
            if (!replaced_argv[replaced_index - 1])
                goto shell_fallback_nomem;
        }

        if (vfs_openat(AT_FDCWD, shell_path, &exec_how, &shell_file, true) <
                0 ||
            !shell_file) {
            task_execve_free_string_array(replaced_argv, replaced_index);
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOENT;
        }

        task_execve_free_string_array(new_argv, argv_count);
        new_argv = replaced_argv;
        argv_count = replaced_argc;

        int exec_access_ret = vfs_file_get_exec_access(shell_file);
        if (exec_access_ret < 0) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(shell_file);
            vfs_close_file(exec_file);
            return (uint64_t)exec_access_ret;
        }

        strncpy(path, shell_path, sizeof(path));
        path[sizeof(path) - 1] = '\0';
        vfs_close_file(exec_file);
        exec_file = shell_file;
        node = shell_file->f_inode;
        shell_fallback_used = true;

        loff_t shell_header_pos = 0;
        header_read = vfs_read_kernel_file(
            exec_file, header_buf, sizeof(header_buf), &shell_header_pos);
        if (header_read < (ssize_t)sizeof(Elf64_Ehdr) ||
            !arch_check_elf((const Elf64_Ehdr *)header_buf)) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOEXEC;
        }

        goto shell_fallback_done;

    shell_fallback_nomem:
        task_execve_free_string_array(replaced_argv, replaced_index);
        task_execve_free_string_array(new_argv, argv_count);
        task_execve_free_string_array(new_envp, envp_count);
        vfs_close_file(exec_file);
        return (uint64_t)-ENOMEM;
    }

shell_fallback_done:
    if (header_read < sizeof(Elf64_Ehdr)) {
        task_execve_free_string_array(new_argv, argv_count);
        task_execve_free_string_array(new_envp, envp_count);
        vfs_close_file(exec_file);
        return (uint64_t)-ENOEXEC;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)header_buf;

    int dethread_ret = task_execve_dethread(self);
    if (dethread_ret < 0) {
        task_execve_free_string_array(new_argv, argv_count);
        task_execve_free_string_array(new_envp, envp_count);
        vfs_close_file(exec_file);
        return (uint64_t)dethread_ret;
    }

    if (!arch_check_elf(ehdr)) {
        task_execve_free_string_array(new_argv, argv_count);
        task_execve_free_string_array(new_envp, envp_count);
        vfs_close_file(exec_file);
        return (uint64_t)-ENOEXEC;
    }

    task_execve_creds_t exec_creds = task_execve_prepare_creds(self, exec_file);

    Elf64_Phdr *phdr;
    size_t phdr_size = (size_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    bool phdr_allocated = false;
    task_signal_info_t *new_signal = NULL;
    struct vfs_file **exec_close_files = NULL;
    fd_info_t *exec_new_fd_info = NULL;
    bool exec_fd_info_put_needed = false;
    size_t exec_max_fds = 0;

    if (ehdr->e_phoff + phdr_size <= sizeof(header_buf)) {
        phdr = (Elf64_Phdr *)(header_buf + ehdr->e_phoff);
    } else {
        phdr = (Elf64_Phdr *)malloc(phdr_size);
        if (!phdr) {
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOMEM;
        }
        phdr_allocated = true;
        loff_t phdr_pos = (loff_t)ehdr->e_phoff;
        if (vfs_read_kernel_file(exec_file, phdr, phdr_size, &phdr_pos) !=
            (ssize_t)phdr_size) {
            free(phdr);
            task_execve_free_string_array(new_argv, argv_count);
            task_execve_free_string_array(new_envp, envp_count);
            vfs_close_file(exec_file);
            return (uint64_t)-ENOEXEC;
        }
    }

    task_mm_info_t *old_mm = self->mm;
    task_mm_info_t *new_mm = (task_mm_info_t *)malloc(sizeof(task_mm_info_t));
    memset(new_mm, 0, sizeof(task_mm_info_t));
    spin_init(&new_mm->lock);
    new_mm->page_table_addr = alloc_frames(1);
    arch_page_table_prepare_new(
        (uint64_t *)phys_to_virt(new_mm->page_table_addr));
    new_mm->ref_count = 1;
    vma_manager_init(&new_mm->task_vma_mgr, true);
    task_mm_init_aslr(new_mm);

    task_execve_switch_mm(self, old_mm, new_mm);

    uint64_t real_load_start = 0;
    if (ehdr->e_type == ET_DYN) {
        real_load_start = task_mm_pie_base(new_mm);
    }

    uint64_t e_entry = real_load_start + ehdr->e_entry;
    if (e_entry == 0) {
        exec_fail_ret = (uint64_t)-EINVAL;
        goto exec_fail_restore_mm;
    }

    uint64_t interpreter_base = task_mm_interpreter_base(new_mm);
    uint64_t stack_start = task_mm_stack_start(new_mm);
    uint64_t stack_end = task_mm_stack_end(new_mm);

    uint64_t load_start = UINT64_MAX;
    uint64_t load_end = 0;
    uint64_t interpreter_entry = 0;
    uint64_t interpreter_at_base = 0;
    uint64_t aux_entry = e_entry;
    uint64_t aux_phdr = 0;
    uint64_t aux_phnum = ehdr->e_phnum;

    uint64_t phdr_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type == PT_INTERP) {
            if (interpreter_path) {
                exec_fail_ret = (uint64_t)-ENOEXEC;
                goto exec_fail_restore_mm;
            }

            // Some interpreter like alpine g-compat uses a chain interpreter
            // which passes the original executable's entry and phdr info
            // through auxv, so we need to track it during execve processing.
            int interp_ret = task_execve_read_interp_path(exec_file, &phdr[i],
                                                          &interpreter_path);
            if (interp_ret < 0) {
                exec_fail_ret = (uint64_t)interp_ret;
                goto exec_fail_restore_mm;
            }

            task_execve_interp_result_t interp_result;
            interp_ret = task_execve_load_interpreter_chain(
                self, &exec_file->f_path, interpreter_path, interpreter_base, 0,
                &interp_result);
            if (interp_ret < 0) {
                exec_fail_ret = (uint64_t)interp_ret;
                goto exec_fail_restore_mm;
            }

            interpreter_entry = interp_result.entry;
            interpreter_at_base = interp_result.at_base;
            if (interp_result.aux_override) {
                aux_entry = interp_result.aux_entry;
                aux_phdr = interp_result.aux_phdr;
                aux_phnum = interp_result.aux_phnum;
            }
        } else if (phdr[i].p_type == PT_LOAD) {
            uint64_t seg_addr = real_load_start + phdr[i].p_vaddr;
            uint64_t aligned_addr = PADDING_DOWN(seg_addr, PAGE_SIZE);
            uint64_t alloc_size = PADDING_UP(
                phdr[i].p_memsz + (seg_addr - aligned_addr), PAGE_SIZE);

            if (aligned_addr < load_start)
                load_start = aligned_addr;
            if (aligned_addr + alloc_size > load_end)
                load_end = aligned_addr + alloc_size;

            if (register_elf_load_vma(self, node, path, real_load_start,
                                      &phdr[i]) != 0) {
                printk("Failed to register executable PT_LOAD VMA\n");
            }
        } else if (phdr[i].p_type == PT_PHDR) {
            phdr_vaddr = real_load_start + phdr[i].p_vaddr;
        }
    }

    if (!phdr_vaddr) {
        phdr_vaddr = (uint64_t)(load_start + ehdr->e_phoff);
    }
    if (!aux_phdr)
        aux_phdr = phdr_vaddr;

    if (phdr_allocated) {
        free(phdr);
        phdr_allocated = false;
    }

    vma_t *stack_guard_vma = vma_alloc();
    vma_t *region = vma_find_intersection(&self->mm->task_vma_mgr,
                                          stack_start - PAGE_SIZE, stack_start);
    if (stack_guard_vma) {
        stack_guard_vma->vm_start = stack_start - PAGE_SIZE;
        stack_guard_vma->vm_end = stack_start;
        stack_guard_vma->vm_flags |= VMA_ANON | VMA_STACK;
        stack_guard_vma->vm_type = VMA_TYPE_ANON;
        stack_guard_vma->vm_name = NULL;

        if (!region) {
            if (vma_insert(&self->mm->task_vma_mgr, stack_guard_vma) != 0)
                vma_free(stack_guard_vma);
        } else {
            vma_free(stack_guard_vma);
        }
    }

    vma_t *stack_vma = vma_alloc();
    region =
        vma_find_intersection(&self->mm->task_vma_mgr, stack_start, stack_end);
    if (stack_vma) {
        stack_vma->vm_start = stack_start;
        stack_vma->vm_end = stack_end;
        stack_vma->vm_flags |= VMA_ANON | VMA_READ | VMA_WRITE | VMA_STACK;
        stack_vma->vm_type = VMA_TYPE_ANON;
        stack_vma->vm_name = strdup("[stack]");

        if (!region) {
            if (vma_insert(&self->mm->task_vma_mgr, stack_vma) != 0)
                vma_free(stack_vma);
        } else {
            vma_free(stack_vma);
        }
    }

    uint64_t stack = 0;
    if (!push_infos(self, stack_end, (char **)new_argv, argv_count,
                    (char **)new_envp, envp_count, aux_entry, aux_phdr,
                    aux_phnum, interpreter_entry ? interpreter_at_base : 0,
                    path, &exec_creds, &stack)) {
        exec_fail_ret = (uint64_t)-EFAULT;
        goto exec_fail_restore_mm;
    }

    for (int i = 0; i < argv_count; i++) {
        if (new_argv[i]) {
            free(new_argv[i]);
        }
    }
    free(new_argv);
    new_argv = NULL;
    for (int i = 0; i < envp_count; i++) {
        if (new_envp[i]) {
            free(new_envp[i]);
        }
    }
    free(new_envp);
    new_envp = NULL;

    fd_info_t *exec_fd_info = task_fd_info_get(self);
    if (!exec_fd_info) {
        exec_fail_ret = (uint64_t)-EBADF;
        goto exec_fail_restore_mm;
    }

    exec_max_fds = exec_fd_info->max_fds;
    exec_close_files = calloc(exec_max_fds, sizeof(*exec_close_files));
    if (!exec_close_files) {
        task_fd_info_put(exec_fd_info, self);
        exec_fail_ret = (uint64_t)-ENOMEM;
        goto exec_fail_restore_mm;
    }
    task_fd_info_put(exec_fd_info, self);

    new_signal = task_signal_reset_after_exec(self);
    if (!new_signal) {
        exec_fail_ret = (uint64_t)-ENOMEM;
        goto exec_fail_restore_mm;
    }

    if (self->clone_flags & CLONE_FILES) {
        fd_info_t *old = task_fd_info_get(self);
        if (!old) {
            exec_fail_ret = (uint64_t)-EBADF;
            goto exec_fail_restore_mm;
        }
        exec_new_fd_info = task_fd_info_clone(old);
        if (!exec_new_fd_info) {
            task_fd_info_put(old, self);
            exec_fail_ret = (uint64_t)-ENOMEM;
            goto exec_fail_restore_mm;
        }
        task_fd_info_put(old, self);
    }

    if (exec_new_fd_info) {
        fd_info_t *replaced = task_fd_info_replace(self, exec_new_fd_info);
        exec_fd_info = exec_new_fd_info;
        exec_new_fd_info = NULL;
        task_fd_info_put(replaced, self);
        exec_fd_info_put_needed = false;
    } else {
        exec_fd_info = task_fd_info_get(self);
        if (!exec_fd_info) {
            exec_fail_ret = (uint64_t)-EBADF;
            goto exec_fail_restore_mm;
        }
        exec_fd_info_put_needed = true;
    }

    with_fd_info_lock(exec_fd_info, {
        for (size_t i = 0; i < exec_fd_info->max_fds; i++) {
            if (!exec_fd_info->fds[i].file)
                continue;

            if (exec_fd_info->fds[i].flags & FD_CLOEXEC) {
                struct vfs_file *entry =
                    task_fd_slot_take(exec_fd_info, (int)i, NULL);
                exec_close_files[i] = entry;
            }
        }
    });

    if (exec_fd_info_put_needed)
        task_fd_info_put(exec_fd_info, self);
    exec_fd_info = NULL;
    exec_fd_info_put_needed = false;

    for (size_t i = 0; i < exec_max_fds; i++) {
        if (!exec_close_files[i])
            continue;
        on_close_file_call(self, i, exec_close_files[i]);
        vfs_close_file(exec_close_files[i]);
    }
    free(exec_close_files);
    exec_close_files = NULL;

    shm_exec(self, old_mm);

    if (!self->is_kernel)
        task_enqueue_mm_free(old_mm);

    task_signal_free(self->signal);
    self->signal = new_signal;

    self->load_start = load_start;
    self->load_end = load_end;

    if (interpreter_path)
        free(interpreter_path);

    strncpy(self->name, path, TASK_NAME_MAX);
    self->name[TASK_NAME_MAX - 1] = '\0';

    task_complete_vfork(self);
    self->clone_flags = 0;
    self->is_clone = false;
    self->is_kernel = false;

    if (self->exec_file)
        vfs_close_file(self->exec_file);
    self->exec_file = exec_file;
    task_execve_commit_creds(self, &exec_creds);
    ptrace_stop_for_exec(self);
    ptrace_stop_for_exec_syscall_exit(self);

    arch_to_user_mode(self->arch_context,
                      interpreter_entry ? interpreter_entry : e_entry, stack);

    return (uint64_t)-EAGAIN;

exec_fail_restore_mm:
    task_execve_switch_mm(self, new_mm, old_mm);
    free_page_table(new_mm);
    free(exec_close_files);
    if (exec_new_fd_info)
        task_fd_info_put(exec_new_fd_info, self);
    if (new_signal)
        task_signal_free(new_signal);
    if (interpreter_path)
        free(interpreter_path);
    if (phdr_allocated)
        free(phdr);
    task_execve_free_string_array(new_argv, argv_count);
    task_execve_free_string_array(new_envp, envp_count);
    vfs_close_file(exec_file);

    return exec_fail_ret;
}

uint64_t task_execve(const char *path_user, const char **argv,
                     const char **envp) {
    return task_do_execve(AT_FDCWD, path_user, argv, envp, 0);
}

uint64_t sys_execveat(uint64_t dirfd, const char *path_user, const char **argv,
                      const char **envp, uint64_t flags) {
    return task_do_execve((int)dirfd, path_user, argv, envp, flags);
}

uint64_t sys_wait4(int pid, int *status, uint64_t options,
                   struct rusage *rusage) {
    task_t *target = NULL;
    uint64_t ret = -ECHILD;
    int64_t wait_pid = (int64_t)pid;
    uint64_t wait_parent_pid = task_effective_wait_parent_pid(current_task);

    if (status && check_user_overflow((uint64_t)status, sizeof(int))) {
        return -EFAULT;
    }
    if (rusage &&
        check_user_overflow((uint64_t)rusage, sizeof(struct rusage))) {
        return -EFAULT;
    }

    bool has_children = false;
    bool has_ptraced = false;
    spin_lock(&task_queue_lock);
    task_index_bucket_t *children =
        task_index_bucket_lookup(&task_parent_map, wait_parent_pid);
    has_children = children && children->count > 0;
    spin_unlock(&task_queue_lock);
    has_ptraced = wait_has_ptraced_target_waitpid(current_task, wait_pid);

    if (!has_children && !has_ptraced) {
        if (waitpid_has_specific_target(current_task, wait_pid))
            return -ESRCH;
        return -ECHILD;
    }

    while (1) {
        wait_queue_entry_t child_wait;
        bool child_wait_armed = !(options & WNOHANG);
        task_t *found_alive = NULL;
        task_t *found_dead = NULL;
        task_t *found_ptrace_alive = NULL;
        task_t *found_ptrace_stop = NULL;

        if (child_wait_armed)
            task_child_wait_arm(&child_wait);

        arch_disable_interrupt();

        spin_lock(&task_queue_lock);
        task_index_bucket_t *children =
            task_index_bucket_lookup(&task_parent_map, wait_parent_pid);
        struct llist_header *node = children ? children->tasks.next : NULL;
        while (children && node != &children->tasks) {
            task_t *ptr = list_entry(node, task_t, parent_node);
            node = node->next;
            if (!ptr)
                continue;

            if (task_parent_wait_key(ptr) != wait_parent_pid)
                continue;

            if (!waitpid_task_matches(ptr, wait_pid, current_task->pgid)) {
                continue;
            }

            if (ptr->state == TASK_DIED) {
                if (task_is_reaped(ptr))
                    continue;

                if (!task_try_mark_reaped(ptr))
                    continue;

                found_dead = ptr;
                break;
            } else {
                found_alive = ptr;
            }
        }

        if (!found_dead) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                task_t *ptr;

                if (!hashmap_entry_is_occupied(entry))
                    continue;

                ptr = (task_t *)entry->value;
                if (!ptrace_matches_waiter(ptr, current_task))
                    continue;
                if (!waitpid_task_matches(ptr, wait_pid, current_task->pgid))
                    continue;

                if (ptrace_has_wait_event(ptr)) {
                    found_ptrace_stop = ptr;
                    break;
                }

                found_ptrace_alive = ptr;
            }
        }
        if (!found_dead && (found_alive || found_ptrace_alive) &&
            !(options & WNOHANG)) {
            waitpid_mark_waiters_locked(current_task, wait_parent_pid,
                                        wait_pid);
        }
        spin_unlock(&task_queue_lock);

        arch_enable_interrupt();

        if (found_ptrace_stop) {
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            if (status) {
                int stop_status = 0;
                ptrace_consume_wait_event(found_ptrace_stop, &stop_status, NULL,
                                          true);
                if (copy_to_user(status, &stop_status, sizeof(stop_status))) {
                    return -EFAULT;
                }
                ptrace_consume_wait_event(found_ptrace_stop, NULL, NULL, false);
            } else {
                ptrace_consume_wait_event(found_ptrace_stop, NULL, NULL, false);
            }
            return found_ptrace_stop->pid;
        }

        if (found_dead) {
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            target = found_dead;
            break;
        }

        if ((found_alive || found_ptrace_alive) && (options & WNOHANG)) {
            return 0;
        }

        if (found_alive) {
            if (task_signal_has_deliverable(current_task)) {
                if (child_wait_armed)
                    task_child_wait_disarm(&child_wait);
                return -EINTR;
            }
            int reason = task_block(current_task, TASK_BLOCKING, -1, "waitpid");
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            if (reason < 0)
                return reason;
            if (reason != EOK && task_signal_has_deliverable(current_task))
                return -EINTR;
            continue;
        }

        if (found_ptrace_alive) {
            if (task_signal_has_deliverable(current_task)) {
                if (child_wait_armed)
                    task_child_wait_disarm(&child_wait);
                return -EINTR;
            }
            int reason =
                task_block(current_task, TASK_BLOCKING, -1, "waitpid-ptrace");
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            if (reason < 0)
                return reason;
            if (reason != EOK && task_signal_has_deliverable(current_task))
                return -EINTR;
            continue;
        }

        if (child_wait_armed)
            task_child_wait_disarm(&child_wait);
        if (waitpid_has_specific_target(current_task, wait_pid))
            return -ESRCH;
        return -ECHILD;
    }

    if (target) {
        int ret_status = 0;

        if (status) {
            if (!target->exited_by_signal) {
                ret_status = ((target->status & 0xff) << 8);
            } else {
                int sig = target->status - 128;
                ret_status = (sig & 0xff);
            }

            if (copy_to_user(status, &ret_status, sizeof(int))) {
                __atomic_store_n(&target->exit_reaped, false, __ATOMIC_RELEASE);
                return -EFAULT;
            }
        }
        if (rusage) {
            struct rusage usage;
            task_fill_rusage(target, true, &usage);
            if (copy_to_user(rusage, &usage, sizeof(usage))) {
                __atomic_store_n(&target->exit_reaped, false, __ATOMIC_RELEASE);
                return -EFAULT;
            }
        }

        ret = target->pid;
        task_aggregate_child_usage(current_task, target);

        task_enqueue_should_free(target);
    }

    return ret;
}

uint64_t sys_waitid(int idtype, uint64_t id, siginfo_t *infop, int options,
                    struct rusage *rusage) {
    task_t *target = NULL;
    uint64_t ret = 0;
    int match_idtype = idtype;
    uint64_t match_id = id;
    uint64_t wait_parent_pid = task_effective_wait_parent_pid(current_task);
    bool waiting_on_pidfd = false;

    if (idtype < P_ALL || idtype > P_PIDFD)
        return -EINVAL;

    if (!(options & (WEXITED | WSTOPPED | WCONTINUED)))
        return -EINVAL;
    if (infop && check_user_overflow((uint64_t)infop, sizeof(siginfo_t))) {
        return -EFAULT;
    }
    if (rusage &&
        check_user_overflow((uint64_t)rusage, sizeof(struct rusage))) {
        return -EFAULT;
    }
    if (idtype == P_PIDFD) {
        int pidfd_ret = pidfd_get_pid_from_fd(id, &match_id);
        if (pidfd_ret < 0) {
            return (uint64_t)pidfd_ret;
        }
        match_idtype = P_PID;
        waiting_on_pidfd = true;
    }

    bool has_children = false;
    bool has_ptraced = false;
    spin_lock(&task_queue_lock);
    task_index_bucket_t *children =
        task_index_bucket_lookup(&task_parent_map, wait_parent_pid);
    has_children = children && children->count > 0;
    spin_unlock(&task_queue_lock);
    has_ptraced =
        wait_has_ptraced_target_waitid(current_task, match_idtype, match_id);

    if (!has_children && !has_ptraced && !waiting_on_pidfd)
        return -ECHILD;

    while (1) {
        wait_queue_entry_t child_wait;
        bool child_wait_armed = !(options & WNOHANG);
        task_t *found_alive = NULL;
        task_t *found_dead = NULL;
        task_t *found_ptrace_alive = NULL;
        task_t *found_ptrace_stop = NULL;

        if (child_wait_armed)
            task_child_wait_arm(&child_wait);

        arch_disable_interrupt();

        spin_lock(&task_queue_lock);
        task_index_bucket_t *children =
            task_index_bucket_lookup(&task_parent_map, wait_parent_pid);
        struct llist_header *node = children ? children->tasks.next : NULL;
        while (children && node != &children->tasks) {
            task_t *ptr = list_entry(node, task_t, parent_node);
            node = node->next;
            if (!ptr)
                continue;

            if (task_parent_wait_key(ptr) != wait_parent_pid)
                continue;

            if (!waitid_task_matches(ptr, match_idtype, match_id)) {
                continue;
            }

            if (ptr->state == TASK_DIED) {
                if (task_is_reaped(ptr))
                    continue;

                if (options & WEXITED) {
                    if (!task_try_mark_reaped(ptr))
                        continue;

                    found_dead = ptr;
                    break;
                }
            }

            found_alive = ptr;
        }

        if (!found_dead) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                task_t *ptr;

                if (!hashmap_entry_is_occupied(entry))
                    continue;

                ptr = (task_t *)entry->value;
                if (!ptrace_matches_waiter(ptr, current_task))
                    continue;
                if (!waitid_task_matches(ptr, match_idtype, match_id))
                    continue;

                if (ptrace_has_wait_event(ptr)) {
                    found_ptrace_stop = ptr;
                    break;
                }

                found_ptrace_alive = ptr;
            }
        }
        if (!found_dead && (found_alive || found_ptrace_alive) &&
            !(options & WNOHANG)) {
            waitid_mark_waiters_locked(current_task, wait_parent_pid,
                                       match_idtype, match_id);
        }
        spin_unlock(&task_queue_lock);

        arch_enable_interrupt();

        if (found_ptrace_stop) {
            siginfo_t ptrace_info;
            bool have_ptrace_info = false;

            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            if (infop) {
                if (!ptrace_consume_wait_event(found_ptrace_stop, NULL,
                                               &ptrace_info, true)) {
                    return 0;
                }
                have_ptrace_info = true;
                if (copy_to_user(infop, &ptrace_info, sizeof(ptrace_info))) {
                    return (uint64_t)-EFAULT;
                }
            }
            if (rusage) {
                struct rusage usage;
                task_fill_rusage(found_ptrace_stop, false, &usage);
                if (copy_to_user(rusage, &usage, sizeof(usage))) {
                    return (uint64_t)-EFAULT;
                }
            }
            if (!(options & WNOWAIT)) {
                if (!have_ptrace_info)
                    ptrace_consume_wait_event(found_ptrace_stop, NULL, NULL,
                                              true);
                ptrace_consume_wait_event(found_ptrace_stop, NULL, NULL, false);
            }
            return 0;
        }

        if (found_dead) {
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            target = found_dead;
            break;
        }

        if ((found_alive || found_ptrace_alive) && (options & WNOHANG)) {
            if (infop) {
                siginfo_t empty_info;
                memset(&empty_info, 0, sizeof(empty_info));
                if (copy_to_user(infop, &empty_info, sizeof(empty_info))) {
                    return (uint64_t)-EFAULT;
                }
            }
            return 0;
        }

        if (found_alive) {
            int reason = task_block(current_task, TASK_BLOCKING, -1, "waitid");
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            if (reason < 0)
                return reason;
            if (reason != EOK && task_signal_has_deliverable(current_task))
                return -EINTR;
            continue;
        }

        if (found_ptrace_alive) {
            int reason =
                task_block(current_task, TASK_BLOCKING, -1, "waitid-ptrace");
            if (child_wait_armed)
                task_child_wait_disarm(&child_wait);
            if (reason < 0)
                return reason;
            if (reason != EOK && task_signal_has_deliverable(current_task))
                return -EINTR;
            continue;
        }

        if (child_wait_armed)
            task_child_wait_disarm(&child_wait);
        return -ECHILD;
    }

    if (target) {
        if (infop) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            info.si_signo = SIGCHLD;
            info.si_errno = 0;
            info._sifields._sigchld._pid = target->pid;
            info._sifields._sigchld._uid = target->uid;

            if (target->state == TASK_DIED) {
                if (target->exited_by_signal) {
                    info.si_code = CLD_KILLED;
                    info._sifields._sigchld._status = target->status - 128;
                } else {
                    info.si_code = CLD_EXITED;
                    info._sifields._sigchld._status = target->status;
                }
            }
            if (copy_to_user(infop, &info, sizeof(info))) {
                __atomic_store_n(&target->exit_reaped, false, __ATOMIC_RELEASE);
                return (uint64_t)-EFAULT;
            }
        }
        if (rusage) {
            struct rusage usage;
            task_fill_rusage(target, true, &usage);
            if (copy_to_user(rusage, &usage, sizeof(usage))) {
                __atomic_store_n(&target->exit_reaped, false, __ATOMIC_RELEASE);
                return (uint64_t)-EFAULT;
            }
        }

        ret = 0;

        if (!(options & WNOWAIT) && target->state == TASK_DIED) {
            task_aggregate_child_usage(current_task, target);
            task_enqueue_should_free(target);
        } else if ((options & WNOWAIT) && target->state == TASK_DIED) {
            __atomic_store_n(&target->exit_reaped, false, __ATOMIC_RELEASE);
        }
    }

    return ret;
}

uint64_t sys_getrusage(int who, struct rusage *ru) {
    if (!ru || check_user_overflow((uint64_t)ru, sizeof(struct rusage)))
        return (uint64_t)-EFAULT;

    uint64_t now_ns = nano_time();
    task_account_runtime_ns(current_task, now_ns);

    struct rusage result;

    switch (who) {
    case RUSAGE_SELF:
    case RUSAGE_THREAD:
        memset(&result, 0, sizeof(result));
        task_fill_rusage(current_task, false, &result);
        break;
    case RUSAGE_CHILDREN:
        memset(&result, 0, sizeof(result));
        result.ru_utime = task_ns_to_timeval(current_task->child_user_time_ns);
        result.ru_stime =
            task_ns_to_timeval(current_task->child_system_time_ns);
        break;
    default:
        return (uint64_t)-EINVAL;
    }

    if (copy_to_user(ru, &result, sizeof(result)))
        return (uint64_t)-EFAULT;

    return 0;
}

uint64_t sys_times(struct tms *buf) {
    struct tms value;

    if (!buf)
        return (uint64_t)-EFAULT;
    if (check_user_overflow((uint64_t)buf, sizeof(value)))
        return (uint64_t)-EFAULT;

    value.tms_utime = task_ns_to_clock_ticks(current_task->user_time_ns);
    value.tms_stime = task_ns_to_clock_ticks(current_task->system_time_ns);
    value.tms_cutime = task_ns_to_clock_ticks(current_task->child_user_time_ns);
    value.tms_cstime =
        task_ns_to_clock_ticks(current_task->child_system_time_ns);

    if (copy_to_user(buf, &value, sizeof(value)))
        return (uint64_t)-EFAULT;

    return (uint64_t)(nano_time() / 10000000ULL);
}

uint64_t sys_clone3(struct pt_regs *regs, clone_args_t *args_user,
                    uint64_t args_size) {
    if (args_size < 64)
        return (uint64_t)-EINVAL;
    clone_args_t args = {0};
    size_t copy_size = MIN((size_t)args_size, sizeof(args));
    if (copy_from_user(&args, args_user, copy_size))
        return (uint64_t)-EFAULT;

    if (args.flags & 0xFF)
        return (uint64_t)-EINVAL;
    if (args.exit_signal & ~0xFFULL)
        return (uint64_t)-EINVAL;
    if ((args.stack == 0) != (args.stack_size == 0))
        return (uint64_t)-EINVAL;

    if ((args.flags & CLONE_PIDFD) &&
        (!args.pidfd || check_user_overflow(args.pidfd, sizeof(int)) ||
         check_unmapped(args.pidfd, sizeof(int)))) {
        return (uint64_t)-EFAULT;
    }
    if ((args.flags & CLONE_INTO_CGROUP) && args.cgroup == 0)
        return (uint64_t)-EINVAL;

    uint64_t clone_flags =
        (args.flags & ~CLONE_PIDFD) | (args.exit_signal & 0xFFULL);
    uint64_t ret = sys_clone_internal(
        regs, clone_flags, args.stack ? args.stack + args.stack_size : 0,
        (int *)args.parent_tid, (int *)args.child_tid, args.tls,
        (args.flags & CLONE_INTO_CGROUP) ? (int)args.cgroup : -1);
    if ((int64_t)ret < 0) {
        return ret;
    }

    if (args.flags & CLONE_PIDFD) {
        uint64_t pidfd = pidfd_create_for_pid(ret, 0, true);
        if ((int64_t)pidfd < 0) {
            return pidfd;
        }

        int pidfd_value = (int)pidfd;
        if (copy_to_user((void *)args.pidfd, &pidfd_value,
                         sizeof(pidfd_value))) {
            sys_close(pidfd);
            return (uint64_t)-EFAULT;
        }
    }

    return ret;
}

void before_ret_from_fork() {
    task_t *self = current_task;
    if ((self->clone_flags & CLONE_CHILD_SETTID) && self->set_tidptr) {
        int value = self->pid;
        if (write_task_user_memory(self, (uint64_t)self->set_tidptr, &value,
                                   sizeof(value)) < 0) {
            task_exit(128 + SIGSEGV);
        }
    }
}

uint64_t sys_clone(struct pt_regs *regs, uint64_t flags, uint64_t newsp,
                   int *parent_tid, int *child_tid, uint64_t tls) {
    return sys_clone_internal(regs, flags, newsp, parent_tid, child_tid, tls,
                              -1);
}

static int task_fs_rebind_mount_namespace(task_fs_t *fs,
                                          struct vfs_mount *old_root,
                                          struct vfs_mount *new_root);

static struct vfs_mount *task_namespace_clone_source_root(task_t *task);

static uint64_t sys_clone_internal(struct pt_regs *regs, uint64_t flags,
                                   uint64_t newsp, int *parent_tid,
                                   int *child_tid, uint64_t tls,
                                   int cgroup_fd) {
    if ((flags & CLONE_THREAD) &&
        (!(flags & CLONE_SIGHAND) || !(flags & CLONE_VM))) {
        return (uint64_t)-EINVAL;
    }

    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) {
        return (uint64_t)-EINVAL;
    }

    if ((flags & CLONE_NEWNS) && (flags & CLONE_FS)) {
        return (uint64_t)-EINVAL;
    }

    if ((flags & CLONE_CLEAR_SIGHAND) && (flags & CLONE_SIGHAND)) {
        return (uint64_t)-EINVAL;
    }

    if ((flags & CLONE_PIDFD) && (flags & CLONE_PARENT_SETTID)) {
        return (uint64_t)-EINVAL;
    }

    if ((flags & CLONE_PARENT_SETTID) &&
        (!parent_tid ||
         check_user_overflow((uint64_t)parent_tid, sizeof(int)) ||
         check_unmapped((uint64_t)parent_tid, sizeof(int)))) {
        return (uint64_t)-EFAULT;
    }
    if ((flags & CLONE_PIDFD) &&
        (!parent_tid ||
         check_user_overflow((uint64_t)parent_tid, sizeof(int)) ||
         check_unmapped((uint64_t)parent_tid, sizeof(int)))) {
        return (uint64_t)-EFAULT;
    }

    if ((flags & CLONE_CHILD_SETTID) &&
        (!child_tid || check_user_overflow((uint64_t)child_tid, sizeof(int)) ||
         check_unmapped((uint64_t)child_tid, sizeof(int)))) {
        return (uint64_t)-EFAULT;
    }

    uint64_t ret = (uint64_t)-ENOMEM;
    task_t *child = get_free_task();
    if (child == NULL) {
        return ret;
    }

    task_t *self = current_task;

    strncpy(child->name, self->name, TASK_NAME_MAX);

    child->signal = task_signal_clone(self, flags);
    if (!child->signal) {
        goto fail;
    }

    child->nsproxy = task_ns_proxy_clone(self, flags);
    if (!child->nsproxy) {
        goto fail;
    }

    child->fs = task_fs_clone(self, flags);
    if (!child->fs) {
        goto fail;
    }

    if ((flags & CLONE_NEWNS) && child->nsproxy->mnt_ns &&
        task_namespace_clone_source_root(self) &&
        child->fs->vfs.root.mnt != child->nsproxy->mnt_ns->root) {
        int rebind_ret = task_fs_rebind_mount_namespace(
            child->fs, task_namespace_clone_source_root(self),
            child->nsproxy->mnt_ns->root);

        if (rebind_ret < 0) {
            ret = (uint64_t)rebind_ret;
            goto fail;
        }
    }

    child->cpu_id = alloc_cpu_id();

    void *kernel_stack_base = alloc_frames_bytes(STACK_SIZE);
    if (!kernel_stack_base)
        goto fail;
    child->kernel_stack_base = kernel_stack_base;
    child->kernel_stack = (uint64_t)kernel_stack_base + STACK_SIZE;

    void *syscall_stack_base = alloc_frames_bytes(STACK_SIZE);
    if (!syscall_stack_base)
        goto fail;
    child->syscall_stack_base = syscall_stack_base;
    child->syscall_stack = (uint64_t)syscall_stack_base + STACK_SIZE;

    memset(child->kernel_stack_base, 0, STACK_SIZE);
    memset(child->syscall_stack_base, 0, STACK_SIZE);

    if (!self->mm) {
        printk("src->mm == NULL!!! src = %#018lx\n", self);
        goto fail;
    }
    child->mm = clone_page_table(self->mm, flags);
    if (!child->mm) {
        printk("dst->mm == NULL!!! dst = %#018lx\n", child);
        goto fail;
    }

    child->arch_context = calloc(1, sizeof(arch_context_t));
    if (!child->arch_context)
        goto fail;
    arch_context_t orig_context;
    memcpy(&orig_context, self->arch_context, sizeof(arch_context_t));
    orig_context.ctx = regs;
    arch_context_copy(child->arch_context, &orig_context, child->kernel_stack,
                      flags);
    shm_fork(self, child);

    uint64_t user_sp = arch_regs_get_user_sp(regs);

    if (newsp) {
        user_sp = newsp;
    }

    arch_regs_set_user_sp(child->arch_context->ctx, user_sp);

    child->is_kernel = false;
    child->parent =
        (flags & (CLONE_THREAD | CLONE_PARENT)) ? self->parent : self;
    child->parent_pid = task_effective_tgid(child->parent);
    child->orphaned_to_init = false;
    child->child_subreaper = false;
    child->uid = self->uid;
    child->gid = self->gid;
    child->euid = self->euid;
    child->egid = self->egid;
    child->suid = self->suid;
    child->sgid = self->sgid;
    child->fsuid = self->fsuid;
    child->fsgid = self->fsgid;
    if (task_groups_inherit(child, self) < 0)
        goto fail;
    child->cap_effective = self->cap_effective;
    child->cap_permitted = self->cap_permitted;
    child->cap_inheritable = self->cap_inheritable;
    child->cap_bounding = self->cap_bounding;
    child->cap_ambient = self->cap_ambient;
    child->securebits = self->securebits;
    child->keep_caps = self->keep_caps;
    child->pgid = self->pgid;
    child->sid = self->sid;
    task_keyring_inherit(child, self);

    child->nice = self->nice;

    child->arg_start = self->arg_start;
    child->arg_end = self->arg_end;
    child->env_start = self->env_start;
    child->env_end = self->env_end;

    child->exec_file = self->exec_file ? vfs_file_get(self->exec_file) : NULL;

    child->load_start = self->load_start;
    child->load_end = self->load_end;

    if (flags & CLONE_FILES) {
        child->fd_info = task_fd_info_get(self);
        if (!child->fd_info)
            goto fail;
    } else {
        fd_info_t *self_fd_info = task_fd_info_get(self);
        if (!self_fd_info)
            goto fail;
        child->fd_info = task_fd_info_clone(self_fd_info);
        task_fd_info_put(self_fd_info, self);
        if (!child->fd_info)
            goto fail;
    }

    child->signal->signal = 0;
    memset(&child->signal->pending_signal, 0,
           sizeof(child->signal->pending_signal));

    if (flags & CLONE_SETTLS) {
        arch_context_set_tls(child->arch_context, tls);
    }

    child->parent_death_sig = (uint64_t)-1;

    if (flags & CLONE_THREAD) {
        child->tgid = task_effective_tgid(self);
    } else {
        child->tgid = child->pid;
    }

    int child_tid_value = (int)child->pid;

    if (flags & CLONE_PARENT_SETTID) {
        copy_to_user(parent_tid, &child_tid_value, sizeof(child_tid_value));
    }
    if (flags & CLONE_PIDFD) {
        uint64_t pidfd = pidfd_create_for_pid(child->pid, 0, true);
        if ((int64_t)pidfd < 0) {
            ret = pidfd;
            goto fail;
        }

        int pidfd_value = (int)pidfd;
        if (copy_to_user(parent_tid, &pidfd_value, sizeof(pidfd_value))) {
            sys_close(pidfd);
            ret = (uint64_t)-EFAULT;
            goto fail;
        }
    }

    if (child_tid && (flags & CLONE_CHILD_SETTID)) {
        child->set_tidptr = child_tid;
    }

    if (child_tid && (flags & CLONE_CHILD_CLEARTID)) {
        child->tidptr = child_tid;
    }

    spin_init(&child->timers_lock);

    memcpy(child->rlim, self->rlim, sizeof(child->rlim));

    child->child_vfork_done = false;
    child->vfork_parent_pid = (flags & CLONE_VFORK) ? self->pid : 0;

    child->clone_flags = flags;
    child->is_clone = !!(flags & CLONE_THREAD);

    uint32_t ptrace_fork_event = 0;
    bool ptrace_trace_fork =
        ptrace_tracees_fork_enabled(self, flags, &ptrace_fork_event);
    if (ptrace_trace_fork)
        ptrace_attach_child(self, child);

    spin_lock(&task_queue_lock);
    task_tgid_index_attach_locked(child);
    task_parent_index_attach_locked(child);
    task_pgid_index_attach_locked(child);
    spin_unlock(&task_queue_lock);

    self->child_vfork_done = false;

    if (cgroup_fd >= 0) {
        ret = (uint64_t)cgroupfs_set_task_cgroup_by_fd(child, cgroup_fd);
        if ((int64_t)ret < 0)
            goto fail;
    }

    child->state = ptrace_trace_fork ? TASK_BLOCKING : TASK_READY;
    child->current_state = ptrace_trace_fork ? TASK_BLOCKING : TASK_READY;

    child->sched_info = calloc(1, sizeof(struct sched_entity));
    if (!child->sched_info) {
        ret = (uint64_t)-ENOMEM;
        goto fail;
    }
    if (!ptrace_trace_fork) {
        add_sched_entity(child, &schedulers[child->cpu_id]);
    }

    on_new_task_call(child);

    if (ptrace_trace_fork)
        ptrace_stop_for_fork_event(self, ptrace_fork_event, child->pid);

    if (flags & CLONE_VFORK)
        task_wait_vfork_done_poll(self);

    ret = child->pid;
    return ret;

fail:
    task_cleanup_partial(child, false);
    return ret;
}

uint64_t task_fork(struct pt_regs *regs, bool vfork) {
    uint64_t flags = vfork ? (CLONE_VFORK | CLONE_VM) : 0;
    return sys_clone(regs, flags, 0, NULL, NULL, 0);
}

static int task_fs_rebind_mount_namespace(task_fs_t *fs,
                                          struct vfs_mount *old_root,
                                          struct vfs_mount *new_root) {
    struct vfs_path old_root_path = {0};
    struct vfs_path old_pwd_path = {0};
    struct vfs_path translated_root = {0};
    struct vfs_path translated_pwd = {0};

    if (!fs || !old_root || !new_root || !new_root->mnt_root)
        return -EINVAL;

    old_root_path.mnt = old_root;
    old_root_path.dentry = old_root->mnt_root;
    old_pwd_path = fs->vfs.pwd;
    if (vfs_translate_path_between_roots(&old_root_path, &fs->vfs.root,
                                         new_root, &translated_root) < 0 ||
        vfs_translate_path_between_roots(&old_root_path, &old_pwd_path,
                                         new_root, &translated_pwd) < 0) {
        vfs_path_put(&translated_root);
        vfs_path_put(&translated_pwd);
        return -ENOENT;
    }

    spin_lock(&fs->vfs.lock);
    vfs_path_put(&fs->vfs.root);
    fs->vfs.root = translated_root;

    vfs_path_put(&fs->vfs.pwd);
    fs->vfs.pwd = translated_pwd;
    fs->vfs.seq++;
    spin_unlock(&fs->vfs.lock);
    return 0;
}

static void task_wait_vfork_done_poll(task_t *parent) {
    if (!parent)
        return;

    while (!parent->child_vfork_done) {
        task_prepare_block(parent);
        if (parent->child_vfork_done) {
            task_cancel_block_prepare(parent);
            break;
        }
        task_block(parent, TASK_BLOCKING, -1, "vfork");
    }

    parent->child_vfork_done = false;
}

static struct vfs_mount *task_namespace_clone_source_root(task_t *task) {
    if (task && task->nsproxy && task->nsproxy->mnt_ns &&
        task->nsproxy->mnt_ns->root) {
        return task->nsproxy->mnt_ns->root;
    }
    {
        const struct vfs_path *fs_root = task_fs_root_path(task);

        if (fs_root && fs_root->mnt)
            return fs_root->mnt;
    }
    return vfs_root_path.mnt;
}

uint64_t sys_unshare(uint64_t unshare_flags) {
    static const uint64_t allowed_flags =
        CLONE_FS | CLONE_FILES | CLONE_SYSVSEM | CLONE_NEWNS | CLONE_NEWUTS |
        CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET |
        CLONE_NEWCGROUP;
    static const uint64_t namespace_flags =
        CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |
        CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWCGROUP;

    task_t *self = current_task;
    task_ns_proxy_t *new_nsproxy = NULL;
    task_ns_proxy_t *old_nsproxy = NULL;
    task_fs_t *new_fs = NULL;
    task_fs_t *old_fs = NULL;
    fd_info_t *new_fd_info = NULL;
    fd_info_t *old_fd_info = NULL;
    uint64_t ns_flags;
    uint64_t ret = (uint64_t)-ENOMEM;
    bool need_unshare_fs;
    bool need_unshare_files;

    if (!self)
        return (uint64_t)-EINVAL;

    if (unshare_flags & CLONE_NEWNS)
        unshare_flags |= CLONE_FS;
    if (unshare_flags & CLONE_NEWUSER)
        unshare_flags |= CLONE_FS;
    if (unshare_flags & CLONE_NEWIPC)
        unshare_flags |= CLONE_SYSVSEM;

    ns_flags = unshare_flags & namespace_flags;

    if (unshare_flags & ~allowed_flags)
        return (uint64_t)-EINVAL;

    if ((unshare_flags & CLONE_NEWUSER) &&
        task_thread_group_count(task_effective_tgid(self)) > 1) {
        return (uint64_t)-EINVAL;
    }

    need_unshare_fs = !!(unshare_flags & CLONE_FS);
    fd_info_t *cur_fd_info = task_fd_info_get(self);
    need_unshare_files = !!(unshare_flags & CLONE_FILES) && cur_fd_info &&
                         task_fd_info_ref_read(cur_fd_info) > 2;
    if (cur_fd_info)
        task_fd_info_put(cur_fd_info, self);

    if (ns_flags) {
        new_nsproxy = task_ns_proxy_clone(self, ns_flags);
        if (!new_nsproxy)
            return (uint64_t)-ENOMEM;
    }

    if (need_unshare_fs && self->fs) {
        new_fs = task_fs_create(&self->fs->vfs.root, &self->fs->vfs.pwd);
        if (!new_fs)
            goto fail;

        if ((ns_flags & CLONE_NEWNS) && new_nsproxy && new_nsproxy->mnt_ns &&
            task_namespace_clone_source_root(self) &&
            new_fs->vfs.root.mnt != new_nsproxy->mnt_ns->root) {
            int rebind_ret = task_fs_rebind_mount_namespace(
                new_fs, task_namespace_clone_source_root(self),
                new_nsproxy->mnt_ns->root);

            if (rebind_ret < 0) {
                ret = (uint64_t)rebind_ret;
                goto fail;
            }
        }
    }

    if (need_unshare_files) {
        fd_info_t *clone_source = task_fd_info_get(self);
        if (!clone_source)
            goto fail;
        new_fd_info = task_fd_info_clone(clone_source);
        task_fd_info_put(clone_source, self);
        if (!new_fd_info)
            goto fail;
    }

    if (new_nsproxy) {
        old_nsproxy = self->nsproxy;
        self->nsproxy = new_nsproxy;
    }

    if (new_fs) {
        old_fs = self->fs;
        self->fs = new_fs;
    }

    if (new_fd_info) {
        old_fd_info = task_fd_info_replace(self, new_fd_info);
    }

    if (unshare_flags & CLONE_FS)
        self->clone_flags &= ~CLONE_FS;
    if (unshare_flags & CLONE_FILES)
        self->clone_flags &= ~CLONE_FILES;
    if (unshare_flags & CLONE_SYSVSEM)
        self->clone_flags &= ~CLONE_SYSVSEM;

    if (old_nsproxy)
        task_ns_proxy_put(old_nsproxy);
    if (old_fs)
        task_fs_put(old_fs);
    if (old_fd_info) {
        task_fd_info_put(old_fd_info, self);
    }

    return 0;

fail:
    if (new_fd_info)
        task_fd_info_put(new_fd_info, self);
    if (new_fs)
        task_fs_put(new_fs);
    if (new_nsproxy)
        task_ns_proxy_put(new_nsproxy);
    return ret;
}

uint64_t sys_setns(int fd, uint64_t nstype) {
    static const uint64_t supported_types =
        CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWNET;
    struct vfs_file *file = NULL;
    task_mount_namespace_t *target_mnt_ns = NULL;
    task_user_namespace_t *target_user_ns = NULL;
    task_simple_namespace_t *target_net_ns = NULL;
    uint64_t fd_nstype = 0;
    int ret;

    if (nstype & ~supported_types)
        return (uint64_t)-EINVAL;
    if (nstype != 0 && (nstype & (nstype - 1)) != 0)
        return (uint64_t)-EINVAL;

    file = task_get_file(current_task, fd);
    if (!file)
        return (uint64_t)-EBADF;

    ret = procfs_nsfd_identify(file, &fd_nstype, &target_mnt_ns,
                               &target_user_ns, &target_net_ns);
    vfs_file_put(file);
    if (ret < 0)
        return (uint64_t)ret;

    if (nstype != 0 && nstype != fd_nstype)
        return (uint64_t)-EINVAL;

    switch (fd_nstype) {
    case CLONE_NEWNS:
        ret = task_setns_mount(current_task, target_mnt_ns);
        break;
    case CLONE_NEWUSER:
        ret = task_setns_user(current_task, target_user_ns);
        break;
    case CLONE_NEWNET:
        ret = task_setns_net(current_task, target_net_ns);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    return ret < 0 ? (uint64_t)ret : 0;
}

uint64_t sys_nanosleep(struct timespec *req, struct timespec *rem) {
    if (check_user_overflow((uint64_t)req, sizeof(struct timespec)) ||
        check_unmapped((uint64_t)req, sizeof(struct timespec))) {
        return -EFAULT;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        return (uint64_t)-EINVAL;
    }

    uint64_t request_ns = task_timespec_to_ns_saturate(req);
    uint64_t target = task_ns_add_saturate(nano_time(), request_ns);

    struct timespec remain;
    memset(&remain, 0, sizeof(remain));
    uint64_t ret = task_sleep_until_ns(target, CLOCK_MONOTONIC, true, &remain);

    if ((int64_t)ret < 0 && rem &&
        !check_user_overflow((uint64_t)rem, sizeof(struct timespec)) &&
        !check_unmapped((uint64_t)rem, sizeof(struct timespec))) {
        copy_to_user(rem, &remain, sizeof(remain));
    }

    return ret;
}

uint64_t get_nanotime_by_clockid(int clock_id) {
    if (clock_id == CLOCK_REALTIME) {
        return realtime_now_ns();
    } else if (clock_id == CLOCK_MONOTONIC) {
        return nano_time();
    } else {
        return (uint64_t)-EINVAL;
    }
}

static uint64_t task_sleep_until_ns(uint64_t target_ns, int clock_id,
                                    bool interruptible,
                                    struct timespec *remain) {
    while (true) {
        uint64_t now = get_nanotime_by_clockid(clock_id);

        if ((int64_t)now < 0)
            return (uint64_t)-EINVAL;

        if (target_ns <= now)
            break;

        uint64_t remaining = target_ns - now;
        int64_t block_ns =
            remaining > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)remaining;
        int reason =
            task_block(current_task, TASK_BLOCKING, block_ns, "nanosleep");

        if (reason < 0)
            return (uint64_t)reason;
        if (interruptible && reason >= 128) {
            if (remain) {
                uint64_t end = get_nanotime_by_clockid(clock_id);
                uint64_t left = target_ns > end ? target_ns - end : 0;
                remain->tv_sec = left / 1000000000ULL;
                remain->tv_nsec = left % 1000000000ULL;
            }
            return (uint64_t)-EINTR;
        }
    }

    if (remain) {
        remain->tv_sec = 0;
        remain->tv_nsec = 0;
    }
    return 0;
}

static inline uint64_t task_ns_add_saturate(uint64_t left, uint64_t right) {
    if (left > UINT64_MAX - right)
        return UINT64_MAX;
    return left + right;
}

static inline uint64_t task_timespec_to_ns_saturate(const struct timespec *ts) {
    uint64_t sec = (uint64_t)ts->tv_sec;

    if (sec > UINT64_MAX / 1000000000ULL)
        return UINT64_MAX;

    return task_ns_add_saturate(sec * 1000000000ULL, (uint64_t)ts->tv_nsec);
}

uint64_t sys_clock_nanosleep(int clock_id, int flags,
                             const struct timespec *request_user,
                             struct timespec *remain_user) {
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
        return (uint64_t)-EINVAL;
    }
    if (flags & ~TIMER_ABSTIME) {
        return (uint64_t)-EINVAL;
    }
    if (check_user_overflow((uint64_t)request_user, sizeof(struct timespec)) ||
        check_unmapped((uint64_t)request_user, sizeof(struct timespec))) {
        return -EFAULT;
    }
    struct timespec request;
    if (copy_from_user(&request, request_user, sizeof(request)))
        return (uint64_t)-EFAULT;
    struct timespec remain;
    memset(&remain, 0, sizeof(remain));
    if (remain_user && copy_to_user(remain_user, &remain, sizeof(remain)))
        return (uint64_t)-EFAULT;

    if (request.tv_sec < 0 || request.tv_nsec < 0 ||
        request.tv_nsec >= 1000000000L) {
        return (uint64_t)-EINVAL;
    }

    uint64_t request_ns = task_timespec_to_ns_saturate(&request);
    uint64_t target = (flags & TIMER_ABSTIME)
                          ? request_ns
                          : task_ns_add_saturate(
                                get_nanotime_by_clockid(clock_id), request_ns);

    uint64_t ret = task_sleep_until_ns(target, clock_id, true, &remain);
    if (remain_user && copy_to_user(remain_user, &remain, sizeof(remain)))
        return (uint64_t)-EFAULT;

    return ret;
}

uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4,
                   uint64_t arg5) {
    switch (option) {
    case PR_SET_NAME: // 设置进程名 (PR_SET_NAME=15)
    {
        char pr_name[16] = {0};
        if (!arg2 ||
            copy_from_user_str(pr_name, (const char *)arg2, sizeof(pr_name))) {
            return (uint64_t)-EFAULT;
        }
        memset(current_task->name, 0, sizeof(current_task->name));
        strncpy(current_task->name, pr_name, sizeof(pr_name) - 1);
        return 0;
    }

    case PR_GET_NAME: // 获取进程名 (PR_GET_NAME=16)
    {
        char pr_name[16] = {0};
        strncpy(pr_name, current_task->name, sizeof(pr_name) - 1);
        if (!arg2 || copy_to_user((void *)arg2, pr_name, sizeof(pr_name))) {
            return (uint64_t)-EFAULT;
        }
        return 0;
    }

    case PR_SET_TIMERSLACK:
        return 0;

    case PR_SET_PDEATHSIG:
        current_task->parent_death_sig = arg2;
        return 0;

    case PR_GET_KEEPCAPS:
        return current_task->keep_caps ? 1 : 0;

    case PR_SET_KEEPCAPS:
        if (arg2 != 0 && arg2 != 1)
            return (uint64_t)-EINVAL;
        current_task->keep_caps = arg2 != 0;
        return 0;

    case PR_CAPBSET_READ:
        if (!task_capability_valid(arg2))
            return (uint64_t)-EINVAL;
        return (current_task->cap_bounding & task_capability_bit(arg2)) ? 1 : 0;

    case PR_CAPBSET_DROP:
        if (!task_capability_valid(arg2))
            return (uint64_t)-EINVAL;
        current_task->cap_bounding &= ~task_capability_bit(arg2);
        current_task->cap_ambient &= current_task->cap_bounding;
        return 0;

    case PR_GET_SECUREBITS:
        return current_task->securebits;

    case PR_SET_SECUREBITS:
        current_task->securebits = (uint32_t)arg2;
        return 0;

    case PR_SET_NO_NEW_PRIVS:
        if (arg2 != 1)
            return (uint64_t)-EINVAL;
        current_task->no_new_privs = true;
        return 0;

    case PR_GET_NO_NEW_PRIVS:
        return current_task->no_new_privs ? 1 : 0;

    case PR_SET_CHILD_SUBREAPER:
        current_task->child_subreaper = arg2 != 0;
        return 0;

    case PR_GET_CHILD_SUBREAPER:
        if (!arg2)
            return (uint64_t)-EINVAL;
        {
            int value = current_task->child_subreaper ? 1 : 0;
            if (copy_to_user((void *)arg2, &value, sizeof(value)))
                return (uint64_t)-EFAULT;
        }
        return 0;

    case PR_CAP_AMBIENT:
        switch (arg2) {
        case PR_CAP_AMBIENT_IS_SET:
            if (!task_capability_valid(arg3))
                return (uint64_t)-EINVAL;
            return (current_task->cap_ambient & task_capability_bit(arg3)) ? 1
                                                                           : 0;
        case PR_CAP_AMBIENT_RAISE:
            if (!task_capability_valid(arg3))
                return (uint64_t)-EINVAL;
            if (current_task->securebits & SECBIT_NO_CAP_AMBIENT_RAISE)
                return (uint64_t)-EPERM;
            if (!(current_task->cap_permitted & task_capability_bit(arg3)) ||
                !(current_task->cap_inheritable & task_capability_bit(arg3)))
                return (uint64_t)-EPERM;
            current_task->cap_ambient |= task_capability_bit(arg3);
            return 0;
        case PR_CAP_AMBIENT_LOWER:
            if (!task_capability_valid(arg3))
                return (uint64_t)-EINVAL;
            current_task->cap_ambient &= ~task_capability_bit(arg3);
            return 0;
        case PR_CAP_AMBIENT_CLEAR_ALL:
            if (arg3 != 0)
                return (uint64_t)-EINVAL;
            task_capability_clear_root_ambient(current_task);
            return 0;
        default:
            return (uint64_t)-EINVAL;
        }

    case PR_GET_TIMERSLACK:
        return 0;

    default:
        return -EINVAL; // 未实现的功能返回不支持
    }
}

uint64_t sys_set_robust_list(void *head, size_t len) {
    struct robust_list_head_abi {
        void *list_next;
        long futex_offset;
        void *list_op_pending;
    };

    if (!current_task)
        return (uint64_t)-EINVAL;
    if (len != sizeof(struct robust_list_head_abi))
        return (uint64_t)-EINVAL;

    if ((uint64_t)head != 0 &&
        (check_user_overflow((uint64_t)head, len ? len : 1) ||
         check_unmapped((uint64_t)head, len ? len : 1))) {
        return (uint64_t)-EFAULT;
    }

    current_task->robust_list_head = head;
    current_task->robust_list_len = len;
    return 0;
}

uint64_t sys_get_robust_list(int pid, void **head_ptr, size_t *len_ptr) {
    task_t *target = NULL;

    if (!head_ptr || !len_ptr)
        return (uint64_t)-EFAULT;

    if (pid == 0) {
        target = current_task;
    } else {
        target = task_find_by_pid((uint64_t)pid);
        if (!target)
            return (uint64_t)-ESRCH;
    }

    if (copy_to_user(head_ptr, &target->robust_list_head,
                     sizeof(target->robust_list_head)) ||
        copy_to_user(len_ptr, &target->robust_list_len,
                     sizeof(target->robust_list_len))) {
        return (uint64_t)-EFAULT;
    }

    return 0;
}

uint64_t sys_rseq(void *rseq, uint32_t rseq_len, int flags, uint32_t sig) {
    (void)rseq;
    (void)rseq_len;
    (void)flags;
    (void)sig;
    return (uint64_t)-ENOSYS;
}

size_t sys_setitimer(int which, struct itimerval *value,
                     struct itimerval *old) {
    if (which != 0)
        return (size_t)-ENOSYS;

    uint64_t rt_at = current_task->itimer_real.at;
    uint64_t rt_reset = current_task->itimer_real.reset;

    uint64_t now = nano_time() / 1000000;

    if (old) {
        uint64_t remaining = rt_at > now ? rt_at - now : 0;
        ms_to_timeval(remaining, &old->it_value);
        ms_to_timeval(rt_reset, &old->it_interval);
    }

    if (value) {
        uint64_t targValue =
            value->it_value.tv_sec * 1000 + value->it_value.tv_usec / 1000;
        uint64_t targInterval = value->it_interval.tv_sec * 1000 +
                                value->it_interval.tv_usec / 1000;

        current_task->itimer_real.at = targValue ? (now + targValue) : 0ULL;
        current_task->itimer_real.reset = targInterval;
        task_refresh_tick_work_state(current_task);
    }

    return 0;
}

uint64_t sys_timer_create(clockid_t clockid, struct sigevent *sevp,
                          timer_t *timerid) {
    kernel_timer_t *kt = NULL;
    struct sigevent ksev;
    timer_t created_timerid;
    uint64_t i;

    if (!timerid)
        return -EFAULT;
    if (!timer_clockid_supported(clockid))
        return -EINVAL;

    spin_lock(&current_task->timers_lock);
    for (i = 0; i < MAX_TIMERS_NUM; i++) {
        if (current_task->timers[i] == NULL) {
            kt = malloc(sizeof(kernel_timer_t));
            current_task->timers[i] = kt;
            break;
        }
    }
    spin_unlock(&current_task->timers_lock);

    if (!kt)
        return -ENOMEM;

    memset(kt, 0, sizeof(kernel_timer_t));

    kt->clock_type = clockid;
    kt->sigev_notify = SIGEV_SIGNAL;
    kt->sigev_signo = SIGALRM;

    created_timerid = (timer_t)(uintptr_t)i;
    kt->sigev_value.sival_ptr = created_timerid;

    if (sevp) {
        if (copy_from_user(&ksev, sevp, sizeof(ksev))) {
            current_task->timers[i] = NULL;
            free(kt);
            return -EFAULT;
        }

        if (ksev.sigev_notify != SIGEV_SIGNAL &&
            ksev.sigev_notify != SIGEV_NONE) {
            current_task->timers[i] = NULL;
            free(kt);
            return -EINVAL;
        }

        if (ksev.sigev_notify == SIGEV_SIGNAL &&
            !signal_sig_in_range(ksev.sigev_signo)) {
            current_task->timers[i] = NULL;
            free(kt);
            return -EINVAL;
        }

        kt->sigev_signo = ksev.sigev_signo;
        kt->sigev_value = ksev.sigev_value;
        kt->sigev_notify = ksev.sigev_notify;
    }

    if (copy_to_user(timerid, &created_timerid, sizeof(created_timerid))) {
        current_task->timers[i] = NULL;
        free(kt);
        return -EFAULT;
    }

    return 0;
}

uint64_t sys_timer_settime(timer_t timerid, int flags,
                           const struct itimerspec *new_value,
                           struct itimerspec *old_value) {
    uint64_t idx = (uint64_t)timerid;
    struct itimerspec kts;
    uint64_t interval;
    uint64_t value;
    uint64_t now;
    uint64_t expires;

    if (idx >= MAX_TIMERS_NUM)
        return -EINVAL;
    if (!new_value)
        return -EINVAL;
    if (flags & ~TIMER_ABSTIME)
        return -EINVAL;

    kernel_timer_t *kt = current_task->timers[idx];
    if (!kt)
        return -EINVAL;

    if (copy_from_user(&kts, new_value, sizeof(kts)))
        return -EFAULT;
    if (timer_validate_spec(&kts))
        return -EINVAL;

    interval = timer_spec_to_ns(&kts.it_interval);
    value = timer_spec_to_ns(&kts.it_value);
    now = timer_current_time_ns(kt->clock_type);

    if (old_value) {
        struct itimerspec old = {0};
        uint64_t remaining = kt->expires > now ? kt->expires - now : 0;

        timer_ns_to_spec(kt->interval, &old.it_interval);
        timer_ns_to_spec(remaining, &old.it_value);
        if (copy_to_user(old_value, &old, sizeof(old)))
            return -EFAULT;
    }

    if (flags & TIMER_ABSTIME) {
        expires = value;
    } else {
        expires = value ? now + value : 0;
    }

    kt->interval = interval;
    kt->expires = expires;
    task_refresh_tick_work_state(current_task);

    return 0;
}

uint64_t sys_alarm(uint64_t seconds) {
    struct itimerval old, new;
    new.it_value.tv_sec = seconds;
    new.it_value.tv_usec = 0;
    new.it_interval.tv_sec = 0;
    new.it_interval.tv_usec = 0;
    size_t ret = sys_setitimer(0, &new, &old);
    if ((int64_t)ret < 0)
        return ret;
    return old.it_value.tv_sec + !!old.it_value.tv_usec;
}

#define LINUX_REBOOT_MAGIC1 0xfee1dead
#define LINUX_REBOOT_MAGIC2 672274793
#define LINUX_REBOOT_MAGIC2A 85072278
#define LINUX_REBOOT_MAGIC2B 369367448
#define LINUX_REBOOT_MAGIC2C 537993216

#define LINUX_REBOOT_CMD_RESTART 0x01234567
#define LINUX_REBOOT_CMD_HALT 0xCDEF0123
#define LINUX_REBOOT_CMD_CAD_ON 0x89ABCDEF
#define LINUX_REBOOT_CMD_CAD_OFF 0x00000000
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC
#define LINUX_REBOOT_CMD_RESTART2 0xA1B2C3D4
#define LINUX_REBOOT_CMD_SW_SUSPEND 0xD000FCE2
#define LINUX_REBOOT_CMD_KEXEC 0x45584543

bool cad_enabled = true;

uint64_t sys_reboot(int magic1, int magic2, uint32_t cmd, void *arg) {
    if (magic1 != LINUX_REBOOT_MAGIC1 || magic2 != LINUX_REBOOT_MAGIC2)
        return (uint64_t)-EINVAL;

    switch (cmd) {
    case LINUX_REBOOT_CMD_CAD_OFF:
        cad_enabled = false;
        return 0;
    case LINUX_REBOOT_CMD_CAD_ON:
        cad_enabled = true;
        return 0;
    case LINUX_REBOOT_CMD_RESTART:
    case LINUX_REBOOT_CMD_RESTART2:
        return 0;
    case LINUX_REBOOT_CMD_POWER_OFF:
        return 0;
    default:
        return (uint64_t)-EINVAL;
        break;
    }
}

uint64_t sys_getpgid(uint64_t pid) {
    if (pid) {
        task_t *task = task_find_by_pid(pid);
        return task ? task->pgid : -ESRCH;
    } else
        return current_task->pgid;
}

uint64_t sys_getsid(uint64_t pid) {
    if (pid == 0)
        return current_task->sid;

    task_t *task = task_find_by_pid(pid);
    return task ? task->sid : (uint64_t)-ESRCH;
}

uint64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    spin_lock(&task_queue_lock);
    task_t *task = pid ? task_lookup_by_pid_nolock(pid) : current_task;
    if (!task) {
        spin_unlock(&task_queue_lock);
        return -ESRCH;
    }

    int64_t new_pgid =
        pgid ? (int64_t)pgid : (int64_t)task_effective_tgid(task);
    task_set_thread_group_pgid_locked(task, new_pgid);
    spin_unlock(&task_queue_lock);
    return 0;
}

uint64_t sys_setsid() {
    task_t *self = current_task;
    if (!self)
        return (uint64_t)-EINVAL;
    if (self->pid != task_effective_tgid(self))
        return (uint64_t)-EPERM;
    if (self->pgid == (int64_t)task_effective_tgid(self))
        return (uint64_t)-EPERM;

    spin_lock(&task_queue_lock);
    task_set_thread_group_sid_locked(self, (int64_t)task_effective_tgid(self));
    task_set_thread_group_pgid_locked(self, (int64_t)task_effective_tgid(self));
    spin_unlock(&task_queue_lock);
    return task_effective_tgid(self);
}

static int task_sched_normalize_policy(int policy, int *base_policy) {
    const int allowed_mask = SCHED_RESET_ON_FORK;
    if (policy & ~((int)allowed_mask | 0x7)) {
        return -EINVAL;
    }

    int normalized = policy & ~SCHED_RESET_ON_FORK;

    switch (normalized) {
    case SCHED_OTHER:
    case SCHED_FIFO:
    case SCHED_RR:
    case SCHED_BATCH:
    case SCHED_IDLE:
    case SCHED_DEADLINE:
        if (base_policy) {
            *base_policy = normalized;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

static int task_sched_priority_bounds(int policy, int *min_priority,
                                      int *max_priority) {
    int base_policy = 0;
    int ret = task_sched_normalize_policy(policy, &base_policy);
    if (ret < 0) {
        return ret;
    }

    switch (base_policy) {
    case SCHED_FIFO:
    case SCHED_RR:
        if (min_priority) {
            *min_priority = 1;
        }
        if (max_priority) {
            *max_priority = 99;
        }
        return 0;
    case SCHED_OTHER:
    case SCHED_BATCH:
    case SCHED_IDLE:
    case SCHED_DEADLINE:
        if (min_priority) {
            *min_priority = 0;
        }
        if (max_priority) {
            *max_priority = 0;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

static task_t *task_sched_lookup_target(int pid) {
    if (pid < 0) {
        return NULL;
    }

    if (pid == 0) {
        return current_task;
    }

    return task_find_by_pid((uint64_t)pid);
}

static inline int task_clamp_nice(int niceval) {
    if (niceval < -20)
        return -20;
    if (niceval > 19)
        return 19;
    return niceval;
}

static void task_set_nice_locked(task_t *task, int niceval) {
    if (!task)
        return;
    sched_set_task_nice(task, niceval);
}

static void task_sched_store_params(task_t *task, int policy,
                                    int sched_priority) {
    if (!task)
        return;
    task->sched_policy = policy;
    task->sched_priority = sched_priority;
}

static int task_setpriority_apply_to_process(task_t *task, int niceval) {
    if (!task)
        return -ESRCH;
    task_set_nice_locked(task, niceval);
    return 0;
}

static int task_setpriority_apply_to_pgrp(int64_t pgid, int niceval) {
    bool found = false;

    while (true) {
        uint64_t batch[32];
        size_t batch_count = 0;
        bool has_more = false;

        spin_lock(&task_queue_lock);
        if (task_pid_map.buckets) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                if (!hashmap_entry_is_occupied(entry))
                    continue;
                task_t *task = (task_t *)entry->value;
                if (!task || task->state == TASK_DIED)
                    continue;
                if (task->pgid != pgid)
                    continue;
                found = true;
                if (task->nice == niceval)
                    continue;
                if (batch_count == sizeof(batch) / sizeof(batch[0])) {
                    has_more = true;
                    break;
                }
                batch[batch_count++] = task->pid;
            }
        }
        spin_unlock(&task_queue_lock);

        if (!batch_count)
            break;

        for (size_t i = 0; i < batch_count; i++) {
            task_t *task = task_find_by_pid(batch[i]);
            if (task)
                task_set_nice_locked(task, niceval);
        }

        if (!has_more)
            break;
    }

    return found ? 0 : -ESRCH;
}

static int task_setpriority_apply_to_user(int64_t uid, int niceval) {
    bool found = false;

    while (true) {
        uint64_t batch[32];
        size_t batch_count = 0;
        bool has_more = false;

        spin_lock(&task_queue_lock);
        if (task_pid_map.buckets) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                if (!hashmap_entry_is_occupied(entry))
                    continue;
                task_t *task = (task_t *)entry->value;
                if (!task || task->state == TASK_DIED)
                    continue;
                if (task->uid != uid)
                    continue;
                found = true;
                if (task->nice == niceval)
                    continue;
                if (batch_count == sizeof(batch) / sizeof(batch[0])) {
                    has_more = true;
                    break;
                }
                batch[batch_count++] = task->pid;
            }
        }
        spin_unlock(&task_queue_lock);

        if (!batch_count)
            break;

        for (size_t i = 0; i < batch_count; i++) {
            task_t *task = task_find_by_pid(batch[i]);
            if (task)
                task_set_nice_locked(task, niceval);
        }

        if (!has_more)
            break;
    }

    return found ? 0 : -ESRCH;
}

static int task_getpriority_collect(int which, int who, int *out_nice) {
    int best = 19;
    bool found = false;

    if (!out_nice)
        return -EINVAL;

    switch (which) {
    case PRIO_PROCESS: {
        task_t *task = who == 0 ? current_task : task_find_by_pid(who);
        if (!task)
            return -ESRCH;
        *out_nice = task->nice;
        return 0;
    }
    case PRIO_PGRP: {
        int64_t pgid = who == 0 ? current_task->pgid : who;
        spin_lock(&task_queue_lock);
        if (task_pid_map.buckets) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                if (!hashmap_entry_is_occupied(entry))
                    continue;
                task_t *task = (task_t *)entry->value;
                if (!task || task->state == TASK_DIED || task->pgid != pgid)
                    continue;
                if (!found || task->nice < best)
                    best = task->nice;
                found = true;
            }
        }
        spin_unlock(&task_queue_lock);
        if (!found)
            return -ESRCH;
        *out_nice = best;
        return 0;
    }
    case PRIO_USER: {
        int64_t uid = who == 0 ? current_task->uid : who;
        spin_lock(&task_queue_lock);
        if (task_pid_map.buckets) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                if (!hashmap_entry_is_occupied(entry))
                    continue;
                task_t *task = (task_t *)entry->value;
                if (!task || task->state == TASK_DIED || task->uid != uid)
                    continue;
                if (!found || task->nice < best)
                    best = task->nice;
                found = true;
            }
        }
        spin_unlock(&task_queue_lock);
        if (!found)
            return -ESRCH;
        *out_nice = best;
        return 0;
    }
    default:
        return -EINVAL;
    }
}

uint64_t sys_sched_setparam(int pid, const struct sched_param *param) {
    if (pid < 0) {
        return (uint64_t)-EINVAL;
    }

    struct sched_param kparam = {0};
    if (!param || copy_from_user(&kparam, param, sizeof(kparam))) {
        return (uint64_t)-EFAULT;
    }

    task_t *task = task_sched_lookup_target(pid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }

    int min_priority = 0;
    int max_priority = 0;
    int ret = task_sched_priority_bounds(task->sched_policy, &min_priority,
                                         &max_priority);
    if (ret < 0)
        return (uint64_t)ret;
    if (kparam.sched_priority < min_priority ||
        kparam.sched_priority > max_priority)
        return (uint64_t)-EINVAL;

    task->sched_priority = kparam.sched_priority;
    return 0;
}

uint64_t sys_sched_getparam(int pid, struct sched_param *param) {
    if (pid < 0) {
        return (uint64_t)-EINVAL;
    }

    if (!param) {
        return (uint64_t)-EFAULT;
    }

    task_t *task = task_sched_lookup_target(pid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }

    struct sched_param kparam = {
        .sched_priority = task->sched_priority,
    };

    if (copy_to_user(param, &kparam, sizeof(kparam))) {
        return (uint64_t)-EFAULT;
    }

    return 0;
}

uint64_t sys_sched_setscheduler(int pid, int policy,
                                const struct sched_param *param) {
    if (pid < 0) {
        return (uint64_t)-EINVAL;
    }

    struct sched_param kparam = {0};
    if (!param || copy_from_user(&kparam, param, sizeof(kparam))) {
        return (uint64_t)-EFAULT;
    }

    task_t *task = task_sched_lookup_target(pid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }

    int min_priority = 0;
    int max_priority = 0;
    int ret = task_sched_priority_bounds(policy, &min_priority, &max_priority);
    if (ret < 0) {
        return (uint64_t)ret;
    }

    if (kparam.sched_priority < min_priority ||
        kparam.sched_priority > max_priority) {
        return (uint64_t)-EINVAL;
    }

    task_sched_store_params(task, policy, kparam.sched_priority);
    return 0;
}

uint64_t sys_sched_getscheduler(int pid) {
    if (pid < 0) {
        return (uint64_t)-EINVAL;
    }

    task_t *task = task_sched_lookup_target(pid);
    if (!task) {
        return (uint64_t)-ESRCH;
    }

    return task->sched_policy;
}

uint64_t sys_sched_get_priority_max(int policy) {
    int max_priority = 0;
    int ret = task_sched_priority_bounds(policy, NULL, &max_priority);
    if (ret < 0) {
        return (uint64_t)ret;
    }

    return (uint64_t)max_priority;
}

uint64_t sys_sched_get_priority_min(int policy) {
    int min_priority = 0;
    int ret = task_sched_priority_bounds(policy, &min_priority, NULL);
    if (ret < 0) {
        return (uint64_t)ret;
    }

    return (uint64_t)min_priority;
}

uint64_t sys_sched_rr_get_interval(int pid, struct timespec *interval) {
    if (pid < 0) {
        return (uint64_t)-EINVAL;
    }

    if (!interval) {
        return (uint64_t)-EFAULT;
    }

    if (!task_sched_lookup_target(pid)) {
        return (uint64_t)-ESRCH;
    }

    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 100000000,
    };

    if (copy_to_user(interval, &ts, sizeof(ts))) {
        return (uint64_t)-EFAULT;
    }

    return 0;
}

static size_t task_sched_affinity_bytes(void) {
    size_t bits_per_word = sizeof(unsigned long) * 8;
    size_t words = (cpu_count + bits_per_word - 1) / bits_per_word;
    if (words == 0)
        words = 1;
    return words * sizeof(unsigned long);
}

uint64_t sys_sched_setaffinity(int pid, size_t len,
                               const unsigned long *user_mask_ptr) {
    if (pid < 0)
        return (uint64_t)-EINVAL;
    if (!user_mask_ptr)
        return (uint64_t)-EFAULT;
    if (len < task_sched_affinity_bytes())
        return (uint64_t)-EINVAL;
    if (!task_sched_lookup_target(pid))
        return (uint64_t)-ESRCH;

    unsigned long mask[(MAX_CPU_NUM + sizeof(unsigned long) * 8 - 1) /
                       (sizeof(unsigned long) * 8)];
    memset(mask, 0, sizeof(mask));

    if (copy_from_user(mask, user_mask_ptr, task_sched_affinity_bytes()))
        return (uint64_t)-EFAULT;

    bool any_cpu = false;
    for (uint64_t cpu = 0; cpu < cpu_count; cpu++) {
        size_t word = cpu / (sizeof(unsigned long) * 8);
        size_t bit = cpu % (sizeof(unsigned long) * 8);
        if (mask[word] & (1UL << bit)) {
            any_cpu = true;
            break;
        }
    }

    return any_cpu ? 0 : (uint64_t)-EINVAL;
}

uint64_t sys_sched_getaffinity(int pid, size_t len,
                               unsigned long *user_mask_ptr) {
    if (pid < 0)
        return (uint64_t)-EINVAL;
    if (!user_mask_ptr)
        return (uint64_t)-EFAULT;
    if (len < task_sched_affinity_bytes())
        return (uint64_t)-EINVAL;
    if (!task_sched_lookup_target(pid))
        return (uint64_t)-ESRCH;

    unsigned long mask[(MAX_CPU_NUM + sizeof(unsigned long) * 8 - 1) /
                       (sizeof(unsigned long) * 8)];
    memset(mask, 0, sizeof(mask));

    for (uint64_t cpu = 0; cpu < cpu_count && cpu < MAX_CPU_NUM; cpu++) {
        size_t word = cpu / (sizeof(unsigned long) * 8);
        size_t bit = cpu % (sizeof(unsigned long) * 8);
        mask[word] |= (1UL << bit);
    }

    if (copy_to_user(user_mask_ptr, mask, task_sched_affinity_bytes()))
        return (uint64_t)-EFAULT;

    return task_sched_affinity_bytes();
}

uint64_t sys_getpriority(int which, int who) {
    int niceval = 0;
    int ret = task_getpriority_collect(which, who, &niceval);
    if (ret < 0)
        return (uint64_t)ret;
    return (uint64_t)(20 - niceval);
}

uint64_t sys_setpriority(int which, int who, int niceval) {
    niceval = task_clamp_nice(niceval);

    switch (which) {
    case PRIO_PROCESS:
        return (uint64_t)task_setpriority_apply_to_process(
            who == 0 ? current_task : task_find_by_pid(who), niceval);
    case PRIO_PGRP:
        return (uint64_t)task_setpriority_apply_to_pgrp(
            who == 0 ? current_task->pgid : who, niceval);
    case PRIO_USER:
        return (uint64_t)task_setpriority_apply_to_user(
            who == 0 ? current_task->uid : who, niceval);
    default:
        return (uint64_t)-EINVAL;
    }
}

#define LINUX_KCMP_FILE 0
#define LINUX_KCMP_VM 1
#define LINUX_KCMP_FILES 2
#define LINUX_KCMP_FS 3
#define LINUX_KCMP_SIGHAND 4
#define LINUX_KCMP_IO 5
#define LINUX_KCMP_SYSVSEM 6
#define LINUX_KCMP_EPOLL_TFD 7

static int linux_kcmp_ptrs(const void *left, const void *right) {
    uintptr_t lhs = (uintptr_t)left;
    uintptr_t rhs = (uintptr_t)right;

    if (lhs == rhs)
        return 0;
    return lhs < rhs ? -1 : 1;
}

uint64_t sys_kcmp(uint64_t pid1, uint64_t pid2, int type, uint64_t idx1,
                  uint64_t idx2) {
    task_t *task1;
    task_t *task2;

    task1 = task_find_by_pid(pid1);
    task2 = task_find_by_pid(pid2);
    if (!task1 || !task2)
        return (uint64_t)-ESRCH;

    switch (type) {
    case LINUX_KCMP_FILE: {
        fd_t *file1 = task_get_file(task1, (int)idx1);
        fd_t *file2 = task_get_file(task2, (int)idx2);
        int ret;

        if (!file1 || !file2) {
            if (file1)
                vfs_file_put(file1);
            if (file2)
                vfs_file_put(file2);
            return (uint64_t)-EBADF;
        }

        ret = linux_kcmp_ptrs(file1, file2);
        vfs_file_put(file1);
        vfs_file_put(file2);
        return (uint64_t)ret;
    }
    case LINUX_KCMP_VM:
        return (uint64_t)linux_kcmp_ptrs(task1->mm, task2->mm);
    case LINUX_KCMP_FILES: {
        fd_info_t *fd_info1 = task_fd_info_get(task1);
        fd_info_t *fd_info2 = task_fd_info_get(task2);
        int ret = linux_kcmp_ptrs(fd_info1, fd_info2);
        task_fd_info_put(fd_info1, task1);
        task_fd_info_put(fd_info2, task2);
        return (uint64_t)ret;
    }
    case LINUX_KCMP_FS:
        return (uint64_t)linux_kcmp_ptrs(task1->fs, task2->fs);
    case LINUX_KCMP_SIGHAND:
        return (uint64_t)linux_kcmp_ptrs(
            task1->signal ? task1->signal->sighand : NULL,
            task2->signal ? task2->signal->sighand : NULL);
    case LINUX_KCMP_IO:
    case LINUX_KCMP_SYSVSEM:
    case LINUX_KCMP_EPOLL_TFD:
        printk("sys_kcmp: unsupported type=%d pid1=%lu pid2=%lu idx1=%lu "
               "idx2=%lu\n",
               type, pid1, pid2, idx1, idx2);
        return (uint64_t)-EOPNOTSUPP;
    default:
        printk(
            "sys_kcmp: unknown type=%d pid1=%lu pid2=%lu idx1=%lu idx2=%lu\n",
            type, pid1, pid2, idx1, idx2);
        return (uint64_t)-EINVAL;
    }
}

uint32_t sys_personality(uint32_t personality) {
    uint32_t old = current_task->personality;

    if (personality != UINT32_MAX)
        current_task->personality = personality;

    return old;
}

int sys_yield() {
    schedule(SCHED_FLAG_YIELD);
    return 0;
}
