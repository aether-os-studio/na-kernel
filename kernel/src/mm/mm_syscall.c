#include <mm/fault.h>
#include <mm/mm_syscall.h>
#include <mm/cache.h>
#include <fs/fs_syscall.h>
#include <fs/vfs/vfs.h>
#include <irq/irq_manager.h>
#include <task/task.h>

#define NUMA_NODE_COUNT 1
#define MM_PTE_ATTR_BATCH_PAGES 65536
#define MADVISE_SHARED_FILE_BATCH_PAGES 64

static bool mempolicy_mode_valid(int mode) {
    return mode >= MPOL_DEFAULT && mode <= MPOL_PREFERRED_MANY;
}

static uint64_t mempolicy_copy_nodemask_to_user(unsigned long *nmask,
                                                uint64_t maxnode) {
    if (!nmask)
        return 0;
    if (maxnode == 0)
        return (uint64_t)-EINVAL;

    uint64_t bits_per_long = sizeof(unsigned long) * 8;
    uint64_t words = (maxnode + bits_per_long - 1) / bits_per_long;

    for (uint64_t i = 0; i < words; i++) {
        unsigned long value = 0;
        if (i == 0)
            value = 1UL;
        if (copy_to_user(nmask + i, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }

    return 0;
}

static uint64_t do_munmap_locked(uint64_t addr, uint64_t size);
static uint64_t msync_writeback_file_range(vfs_node_t *node, uint64_t vm_start,
                                           int64_t vm_offset,
                                           uint64_t sync_start,
                                           uint64_t sync_end);
void *general_map(fd_t *file, uint64_t addr, uint64_t len, uint64_t prot,
                  uint64_t flags, uint64_t offset);

static inline uint64_t *mm_pgdir(task_mm_info_t *mm) {
    return mm ? (uint64_t *)phys_to_virt(mm->page_table_addr) : NULL;
}

static void mmap_put_fd_ref(fd_t *fd_ref) {
    if (!fd_ref)
        return;
    vfs_file_put(fd_ref);
}

static bool rlimit_is_infinite(size_t value) { return value == (size_t)-1; }

static bool user_range_valid(uint64_t addr, uint64_t len) {
    if (len == 0)
        return false;
    if (check_user_overflow(addr, len))
        return false;
    return true;
}

static bool user_mmap_range_valid(task_mm_info_t *mm, uint64_t addr,
                                  uint64_t len) {
    if (!user_range_valid(addr, len))
        return false;
    if (addr < USER_MMAP_START)
        return false;
    if (addr + len > task_mm_mmap_top(mm))
        return false;
    return true;
}

static uint64_t vm_flags_access_bits(uint64_t vm_flags) {
    return vm_flags & (VMA_READ | VMA_WRITE | VMA_EXEC);
}

static uint64_t vma_guard_saved_access(uint64_t vm_flags) {
    return (vm_flags & VMA_GUARD_SAVED_MASK) >> VMA_GUARD_SAVED_SHIFT;
}

static void vma_guard_set_saved_access(vma_t *vma, uint64_t access) {
    if (!vma)
        return;

    vma->vm_flags &= ~VMA_GUARD_SAVED_MASK;
    vma->vm_flags |= (access & (VMA_READ | VMA_WRITE | VMA_EXEC))
                     << VMA_GUARD_SAVED_SHIFT;
}

static uint64_t prot_to_vma_access_flags(uint64_t prot) {
    uint64_t vm_flags = 0;

    if (prot & PROT_READ)
        vm_flags |= VMA_READ;
    if (prot & PROT_WRITE)
        vm_flags |= VMA_WRITE;
    if (prot & PROT_EXEC)
        vm_flags |= VMA_EXEC;

    return vm_flags;
}

static uint64_t vm_flags_to_prot(uint64_t vm_flags) {
    uint64_t prot = 0;

    if (vm_flags & VMA_READ)
        prot |= PROT_READ;
    if (vm_flags & VMA_WRITE)
        prot |= PROT_WRITE;
    if (vm_flags & VMA_EXEC)
        prot |= PROT_EXEC;

    return prot;
}

static bool vm_flags_has_access(uint64_t vm_flags) {
    return (vm_flags & (VMA_READ | VMA_WRITE | VMA_EXEC)) != 0;
}

static bool vma_needs_file_writeback(vma_t *vma) {
    return vma && vma->vm_type == VMA_TYPE_FILE &&
           (vma->vm_flags & VMA_SHARED) && (vma->vm_flags & VMA_WRITE) &&
           !(vma->vm_flags & VMA_DEVICE) && vma->node;
}

static uint64_t writeback_vma_file_range(vma_t *vma, uint64_t start,
                                         uint64_t end) {
    if (!vma_needs_file_writeback(vma) || start >= end)
        return 0;

    uint64_t delta = start - vma->vm_start;
    uint64_t file_start = (uint64_t)vma->vm_offset + delta;
    uint64_t file_end = file_start + (end - start);
    int ret = page_cache_writeback_range(&vma->node->i_mapping, file_start,
                                         file_end, false);
    return ret < 0 ? (uint64_t)ret : 0;
}

static bool vma_uses_page_cache_mapping(vma_t *vma) {
    return vma && vma->vm_type == VMA_TYPE_FILE && vma->node &&
           (vma->vm_flags & VMA_SHARED) && !(vma->vm_flags & VMA_DEVICE) &&
           vma->vm_offset >= 0;
}

static uint64_t mm_pte_attr_batch_end(uint64_t cursor, uint64_t limit) {
    uint64_t chunk_end = cursor + MM_PTE_ATTR_BATCH_PAGES * PAGE_SIZE;
    if (chunk_end < cursor || chunk_end > limit)
        chunk_end = limit;
    return chunk_end;
}

static void unmap_vma_resident_range(task_mm_info_t *mm, vma_t *vma,
                                     uint64_t start, uint64_t end) {
    if (!mm || !vma || start >= end)
        return;

    if (vma_uses_page_cache_mapping(vma)) {
        uint64_t file_start =
            (uint64_t)vma->vm_offset + (start - vma->vm_start);
        page_cache_unmap_shared_range(&vma->node->i_mapping, mm, start,
                                      file_start, end - start);
        return;
    }

    unmap_page_range_mm_batched(mm, start, end - start);
}

static uint64_t unmap_vma_resident_range_once(task_mm_info_t *mm, vma_t *vma,
                                              uint64_t start, uint64_t end,
                                              unmap_release_batch_t *batch,
                                              uint64_t *unmapped) {
    if (!mm || !vma || start >= end)
        return end;

    if (vma_uses_page_cache_mapping(vma)) {
        uint64_t chunk_end =
            start + MADVISE_SHARED_FILE_BATCH_PAGES * PAGE_SIZE;
        if (chunk_end < start || chunk_end > end)
            chunk_end = end;
        uint64_t file_start =
            (uint64_t)vma->vm_offset + (start - vma->vm_start);
        page_cache_unmap_shared_range(&vma->node->i_mapping, mm, start,
                                      file_start, chunk_end - start);
        return chunk_end;
    }

    if (!batch)
        return start;

    spin_lock(&mm->lock);
    uint64_t next = unmap_page_range_mm_locked(mm, start, end, batch, unmapped);
    spin_unlock(&mm->lock);
    return next;
}

static uint64_t vm_flags_to_pt_flags(uint64_t vm_flags) {
    uint64_t pt_flags = 0;

    if (vm_flags_has_access(vm_flags))
        pt_flags |= PT_FLAG_U;

    if (vm_flags & VMA_READ)
        pt_flags |= PT_FLAG_R;
    if (vm_flags & VMA_WRITE)
        pt_flags |= PT_FLAG_W;
    if (vm_flags & VMA_EXEC)
        pt_flags |= PT_FLAG_X;

    return pt_flags;
}

static bool ranges_overlap(uint64_t lhs_start, uint64_t lhs_end,
                           uint64_t rhs_start, uint64_t rhs_end) {
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static const char *madvise_behavior_name(int behavior) {
    switch (behavior) {
    case MADV_NORMAL:
        return "MADV_NORMAL";
    case MADV_RANDOM:
        return "MADV_RANDOM";
    case MADV_SEQUENTIAL:
        return "MADV_SEQUENTIAL";
    case MADV_WILLNEED:
        return "MADV_WILLNEED";
    case MADV_DONTNEED:
        return "MADV_DONTNEED";
    case MADV_FREE:
        return "MADV_FREE";
    case MADV_REMOVE:
        return "MADV_REMOVE";
    case MADV_DONTFORK:
        return "MADV_DONTFORK";
    case MADV_DOFORK:
        return "MADV_DOFORK";
    case MADV_MERGEABLE:
        return "MADV_MERGEABLE";
    case MADV_UNMERGEABLE:
        return "MADV_UNMERGEABLE";
    case MADV_HUGEPAGE:
        return "MADV_HUGEPAGE";
    case MADV_NOHUGEPAGE:
        return "MADV_NOHUGEPAGE";
    case MADV_DONTDUMP:
        return "MADV_DONTDUMP";
    case MADV_DODUMP:
        return "MADV_DODUMP";
    case MADV_WIPEONFORK:
        return "MADV_WIPEONFORK";
    case MADV_KEEPONFORK:
        return "MADV_KEEPONFORK";
    case MADV_COLD:
        return "MADV_COLD";
    case MADV_PAGEOUT:
        return "MADV_PAGEOUT";
    case MADV_POPULATE_READ:
        return "MADV_POPULATE_READ";
    case MADV_POPULATE_WRITE:
        return "MADV_POPULATE_WRITE";
    case MADV_GUARD_INSTALL:
        return "MADV_GUARD_INSTALL";
    case MADV_GUARD_REMOVE:
        return "MADV_GUARD_REMOVE";
    default:
        return "MADV_UNKNOWN";
    }
}

static uint64_t membarrier_supported_mask(void) {
    return MEMBARRIER_CMD_PRIVATE_EXPEDITED |
           MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
}

static void membarrier_collect_target_cpus(task_mm_info_t *mm,
                                           uint32_t self_cpu,
                                           bool cpu_targets[MAX_CPU_NUM]) {
    memset(cpu_targets, 0, sizeof(bool) * MAX_CPU_NUM);

    for (uint32_t cpu = 0; cpu < cpu_count && cpu < MAX_CPU_NUM; cpu++)
        cpu_targets[cpu] = cpu != self_cpu && task_mm_cpu_active(mm, cpu);
}

static bool membarrier_private_expedited_wait(task_mm_info_t *mm,
                                              uint64_t seq) {
    bool cpu_targets[MAX_CPU_NUM];
    membarrier_collect_target_cpus(mm, current_cpu_id, cpu_targets);

    for (uint32_t cpu = 0; cpu < cpu_count && cpu < MAX_CPU_NUM; cpu++) {
        if (cpu_targets[cpu])
            irq_trigger_sched_ipi(cpu);
    }

    uint64_t spin_count = 0;
    while (true) {
        bool pending = false;

        for (uint32_t cpu = 0; cpu < cpu_count && cpu < MAX_CPU_NUM; cpu++) {
            if (!cpu_targets[cpu])
                continue;
            if (!task_mm_cpu_active(mm, cpu)) {
                cpu_targets[cpu] = false;
                continue;
            }

            uint64_t seen = __atomic_load_n(&mm->membarrier_cpu_seen_seq[cpu],
                                            __ATOMIC_ACQUIRE);
            if (seen < seq) {
                pending = true;
                break;
            }
        }

        if (!pending)
            return true;

        if ((++spin_count & 0xfff) == 0) {
            for (uint32_t cpu = 0; cpu < cpu_count && cpu < MAX_CPU_NUM;
                 cpu++) {
                if (cpu_targets[cpu] && task_mm_cpu_active(mm, cpu) &&
                    __atomic_load_n(&mm->membarrier_cpu_seen_seq[cpu],
                                    __ATOMIC_ACQUIRE) < seq)
                    irq_trigger_sched_ipi(cpu);
            }
        }

        arch_pause();
    }
}

static int mmap_check_flags_linux(uint64_t flags, uint64_t map_type) {
    const uint64_t supported_flags =
        MAP_TYPE | MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE | MAP_GROWSDOWN |
        MAP_DENYWRITE | MAP_EXECUTABLE | MAP_LOCKED | MAP_POPULATE |
        MAP_NONBLOCK | MAP_STACK | MAP_HUGETLB | MAP_SYNC | MAP_FIXED_NOREPLACE;

    uint64_t unknown_flags = flags & ~supported_flags;
    if (unknown_flags && map_type == MAP_SHARED_VALIDATE)
        return -EOPNOTSUPP;

    if ((flags & MAP_SYNC) && map_type != MAP_SHARED_VALIDATE)
        return -EINVAL;

    return 0;
}

static int mmap_check_file_access(fd_t *file, uint64_t prot,
                                  uint64_t map_type) {
    uint64_t flags = fd_get_flags(file);
    if (flags & O_PATH)
        return -EBADF;
    uint64_t accmode = flags & O_ACCMODE_FLAGS;

    if (accmode == O_WRONLY)
        return -EACCES;

    if ((prot & PROT_WRITE) && (flags & O_APPEND))
        return -EACCES;

    if (prot & PROT_WRITE) {
        if (map_type == MAP_SHARED || map_type == MAP_SHARED_VALIDATE) {
            if (accmode != O_RDWR)
                return -EACCES;
        }
    }

    return 0;
}

static int check_address_space_limit(vma_manager_t *mgr, uint64_t grow) {
    size_t limit = current_task->rlim[RLIMIT_AS].rlim_cur;

    if (rlimit_is_infinite(limit))
        return 0;
    if (mgr->vm_used > limit)
        return -ENOMEM;
    if (grow > limit - mgr->vm_used)
        return -ENOMEM;

    return 0;
}

static int check_data_limit(task_mm_info_t *mm, uint64_t new_brk) {
    size_t limit = current_task->rlim[RLIMIT_DATA].rlim_cur;

    if (rlimit_is_infinite(limit))
        return 0;
    if (new_brk < mm->brk_start)
        return -ENOMEM;
    if (new_brk - mm->brk_start > limit)
        return -ENOMEM;

    return 0;
}

static uint64_t find_unmapped_area_in_window(vma_manager_t *mgr,
                                             uint64_t window_start,
                                             uint64_t window_end,
                                             uint64_t len) {
    window_start = PADDING_UP(window_start, PAGE_SIZE);
    window_end = PADDING_DOWN(window_end, PAGE_SIZE);

    if (window_start >= window_end)
        return (uint64_t)-ENOMEM;
    if (len == 0 || len > window_end - window_start)
        return (uint64_t)-ENOMEM;

    uint64_t cursor = window_start;
    uint64_t best = (uint64_t)-ENOMEM;
    rb_node_t *node = rb_first(&mgr->vma_tree);

    while (node) {
        vma_t *vma = rb_entry(node, vma_t, vm_rb);
        node = rb_next(node);

        if (vma->vm_end <= cursor)
            continue;
        if (vma->vm_start >= window_end)
            break;

        if (vma->vm_start > cursor) {
            uint64_t gap_end = vma->vm_start;
            if (gap_end > window_end)
                gap_end = window_end;
            if (gap_end > cursor && gap_end - cursor >= len)
                best = gap_end - len;
        }

        cursor = PADDING_UP(vma->vm_end, PAGE_SIZE);
        if (cursor < vma->vm_end || cursor >= window_end)
            return best;
    }

    if (cursor < window_end && window_end - cursor >= len)
        best = window_end - len;

    return best;
}

uint64_t find_unmapped_area(task_mm_info_t *mm, uint64_t hint, uint64_t len) {
    /*
     * This helper just needs to find a valid gap without colliding with an
     * existing VMA. Use the hint when it fits, otherwise prefer a stable free
     * region over forcing the mapping near the hinted address.
     */
    if (!mm)
        return (uint64_t)-ENOMEM;

    vma_manager_t *mgr = &mm->task_vma_mgr;
    uint64_t mmap_top = task_mm_mmap_top(mm);
    if (len == 0)
        return (uint64_t)-ENOMEM;

    len = PADDING_UP(len, PAGE_SIZE);
    if (len == 0)
        return (uint64_t)-ENOMEM;
    if (mmap_top <= USER_MMAP_START || len > mmap_top - USER_MMAP_START)
        return (uint64_t)-ENOMEM;

    if (hint) {
        hint = PADDING_DOWN(hint, PAGE_SIZE);
        if (hint >= USER_MMAP_START && hint <= mmap_top - len) {
            if (!vma_find_intersection(mgr, hint, hint + len))
                return hint;

            if (hint + len < mmap_top) {
                uint64_t wrapped = find_unmapped_area_in_window(mgr, hint + len,
                                                                mmap_top, len);
                if ((int64_t)wrapped >= 0)
                    return wrapped;
            }

            uint64_t downward =
                find_unmapped_area_in_window(mgr, USER_MMAP_START, hint, len);
            if ((int64_t)downward >= 0)
                return downward;
        }
    }

    return find_unmapped_area_in_window(mgr, USER_MMAP_START, mmap_top, len);
}

static bool range_fully_covered_locked(vma_manager_t *mgr, uint64_t start,
                                       uint64_t end) {
    uint64_t cursor = start;

    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma || vma->vm_end <= cursor)
            return false;
        cursor = vma->vm_end < end ? vma->vm_end : end;
    }

    return true;
}

static int split_vma_boundaries_locked(vma_manager_t *mgr, uint64_t start,
                                       uint64_t end) {
    if (start >= end)
        return 0;

    while (true) {
        vma_t *vma = vma_find(mgr, start);
        if (!vma)
            break;
        if (vma->vm_start < start && start < vma->vm_end) {
            if (vma_split(mgr, vma, start) != 0)
                return -ENOMEM;
            continue;
        }
        break;
    }

    while (true) {
        /*
         * The end boundary must be split against the VMA that actually covers
         * the last byte in [start, end). Looking up the first intersection in
         * the whole range is wrong when the range spans multiple VMAs: it can
         * stop at an earlier VMA and leave the trailing VMA unsplit, after
         * which do_munmap_locked() trips over vm_end > end and returns
         * -ENOMEM.
         */
        vma_t *vma = vma_find(mgr, end - 1);
        if (!vma)
            break;
        if (vma->vm_start < end && end < vma->vm_end) {
            if (vma_split(mgr, vma, end) != 0)
                return -ENOMEM;
            continue;
        }
        break;
    }

    return 0;
}

static uint64_t update_vma_pte_attrs_locked(task_mm_info_t *mm, uint64_t start,
                                            uint64_t end, bool *ptes_changed) {
    if (!mm || start >= end)
        return 0;

    vma_manager_t *mgr = &mm->task_vma_mgr;
    uint64_t cursor = start;

    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma)
            return (uint64_t)-ENOMEM;

        uint64_t next_cursor =
            mm_pte_attr_batch_end(cursor, MIN(vma->vm_end, end));
        uint64_t effective_access = (vma->vm_flags & VMA_GUARD)
                                        ? 0
                                        : vm_flags_access_bits(vma->vm_flags);

        spin_lock(&mm->lock);
        uint64_t *pgdir = task_mm_pgdir(mm);
        if (!pgdir) {
            spin_unlock(&mm->lock);
            return (uint64_t)-1;
        }

        uint64_t ret = map_change_attribute_range(
            pgdir, cursor, next_cursor - cursor,
            vm_flags_to_pt_flags(effective_access), false);
        spin_unlock(&mm->lock);

        if ((int64_t)ret < 0)
            return ret;
        if (ptes_changed)
            *ptes_changed = true;
        cursor = next_cursor;
    }

    return 0;
}

static uint64_t merge_vma_range_locked(vma_manager_t *mgr, uint64_t start,
                                       uint64_t end) {
    uint64_t cursor = start;

    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma)
            return (uint64_t)-ENOMEM;

        uint64_t next_cursor = MIN(vma->vm_end, end);
        vma_try_merge_around(mgr, &vma);
        if (vma->vm_end > next_cursor)
            next_cursor = vma->vm_end;
        cursor = next_cursor;
    }

    return 0;
}

static vma_t *alloc_mapping_vma(uint64_t start, uint64_t len, uint64_t prot,
                                uint64_t map_type, uint64_t file_flags,
                                bool anonymous, vfs_node_t *node,
                                uint64_t offset) {
    vma_t *vma = vma_alloc();
    if (!vma)
        return NULL;

    vma->vm_start = start;
    vma->vm_end = start + len;
    vma->vm_flags = prot_to_vma_access_flags(prot);
    if (map_type == MAP_SHARED || map_type == MAP_SHARED_VALIDATE)
        vma->vm_flags |= VMA_SHARED;

    if (anonymous) {
        vma->vm_type = VMA_TYPE_ANON;
        vma->vm_flags |= VMA_ANON;
        return vma;
    }

    vma->vm_type = VMA_TYPE_FILE;
    vma->node = node;
    vma->vm_offset = offset;
    vma->vm_file_len = len;
    vma->vm_file_flags = file_flags;
    if (node)
        vfs_igrab(node);

    if (node && (S_ISCHR(node->i_mode) || S_ISBLK(node->i_mode)))
        vma->vm_flags |= VMA_DEVICE;

    return vma;
}

static uint64_t map_file_vma(fd_t *file, uint64_t offset, uint64_t addr,
                             uint64_t len, uint64_t prot, uint64_t flags) {
    if (!file || !file->f_op || !file->f_op->mmap)
        return (uint64_t)-ENOSYS;
    uint64_t ret = (uint64_t)file->f_op->mmap(file, (void *)addr, offset, len,
                                              prot, flags);
    if ((int64_t)ret < 0)
        return ret;
    return addr;
}

static uint64_t populate_anon_vma(uint64_t vm_flags, uint64_t addr,
                                  uint64_t len, bool writable_override) {
    uint64_t pt_flags = vm_flags_to_pt_flags(vm_flags);
    task_mm_info_t *mm = current_task->mm;

    if (writable_override)
        pt_flags |= PT_FLAG_W;

    uint64_t ret =
        map_page_range_mm_batched(mm, addr, (uint64_t)-1, len, pt_flags);
    if (ret != 0)
        unmap_page_range_mm_batched(mm, addr, len);
    return ret == 0 ? addr : (uint64_t)-ENOMEM;
}

static bool should_eager_map_file(vma_t *vma, uint64_t flags) {
    if (!vma)
        return false;
    if (!vm_flags_has_access(vma->vm_flags))
        return false;
    if ((flags & MAP_FIXED) && (vma->vm_flags & VMA_WRITE))
        return true;
    if (flags & (MAP_POPULATE | MAP_LOCKED))
        return true;
    if (vma->vm_flags & VMA_SHARED)
        return true;
    if (vma->vm_flags & VMA_DEVICE)
        return true;
    return false;
}

static bool should_eager_map_anon(uint64_t flags) {
    return (flags & (MAP_POPULATE | MAP_LOCKED)) != 0;
}

static char *mmap_resolve_vm_name(fd_t *file) {
    const struct vfs_path *root;
    const char *fs_name = NULL;
    char buf[256];

    if (!file || !file->f_inode)
        return NULL;

    root = task_fs_root_path(current_task);
    if (root && root->mnt && root->dentry && file->f_path.mnt &&
        file->f_path.dentry && vfs_path_is_ancestor(root, &file->f_path)) {
        char *fullpath = vfs_path_to_string(&file->f_path, root);
        if (fullpath)
            return fullpath;
    }

    if (file->f_path.dentry && file->f_path.dentry->d_name.name &&
        file->f_path.dentry->d_name.name[0]) {
        return strdup(file->f_path.dentry->d_name.name);
    }

    if (file->f_inode->i_sb && file->f_inode->i_sb->s_type)
        fs_name = file->f_inode->i_sb->s_type->name;
    if (fs_name) {
        snprintf(buf, sizeof(buf), "[%s]", fs_name);
        return strdup(buf);
    }

    return NULL;
}

uint64_t sys_brk(uint64_t brk) {
    task_mm_info_t *mm = current_task->mm;
    vma_manager_t *mgr = &mm->task_vma_mgr;
    uint64_t old_brk = mm->brk_current;
    uint64_t heap_base = PADDING_DOWN(mm->brk_start, PAGE_SIZE);

    /*
     * brk has a simple userspace contract: return the new break on success and
     * the old break on failure. Keep partial address-space changes from leaking
     * across failure paths.
     */
    if (brk == 0)
        return old_brk;
    if (brk < mm->brk_start || brk > mm->brk_end)
        return old_brk;

    uint64_t old_map_end = PADDING_UP(old_brk, PAGE_SIZE);
    uint64_t new_map_end = PADDING_UP(brk, PAGE_SIZE);

    spin_lock(&mgr->lock);

    if (new_map_end > old_map_end) {
        vma_t *heap_vma = NULL;
        if (old_map_end > heap_base)
            heap_vma = vma_find(mgr, old_map_end - 1);

        bool extend_tail =
            heap_vma && heap_vma->vm_end == old_map_end &&
            heap_vma->vm_type == VMA_TYPE_ANON && !heap_vma->node &&
            !heap_vma->shm &&
            !(heap_vma->vm_flags & (VMA_SHARED | VMA_SHM | VMA_DEVICE)) &&
            (heap_vma->vm_flags & (VMA_READ | VMA_WRITE | VMA_EXEC)) ==
                (VMA_READ | VMA_WRITE);

        uint64_t map_start = old_map_end;
        if (!extend_tail && !heap_vma && heap_base < old_map_end)
            map_start = heap_base;

        uint64_t map_len = new_map_end - map_start;
        if (check_data_limit(mm, brk) != 0 ||
            check_address_space_limit(mgr, map_len) != 0)
            goto fail;
        if (!user_range_valid(map_start, map_len))
            goto fail;
        if (vma_find_intersection(mgr, map_start, new_map_end))
            goto fail;

        if (extend_tail) {
            uint64_t grow = new_map_end - old_map_end;
            heap_vma->vm_end = new_map_end;
            mgr->vm_used += grow;
        } else if (map_len > 0) {
            vma_t *new_vma = vma_alloc();
            if (!new_vma)
                goto fail;

            new_vma->vm_start = map_start;
            new_vma->vm_end = new_map_end;
            new_vma->vm_flags = VMA_READ | VMA_WRITE | VMA_ANON;
            new_vma->vm_type = VMA_TYPE_ANON;
            new_vma->vm_name = strdup("[heap]");

            if (vma_insert(mgr, new_vma) != 0) {
                vma_free(new_vma);
                goto fail;
            }
        }
    } else if (new_map_end < old_map_end) {
        uint64_t ret = do_munmap_locked(new_map_end, old_map_end - new_map_end);
        if ((int64_t)ret < 0)
            goto fail;
    }

    mm->brk_current = brk;
    spin_unlock(&mgr->lock);
    return mm->brk_current;

fail:
    spin_unlock(&mgr->lock);
    return old_brk;
}

uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                  uint64_t fd, uint64_t offset) {
    const uint64_t page_mask = PAGE_SIZE - 1;
    uint64_t map_type = flags & MAP_TYPE;
    uint64_t aligned_len = PADDING_UP(len, PAGE_SIZE);
    bool anonymous = (flags & MAP_ANONYMOUS) != 0;
    bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    bool no_replace = (flags & MAP_FIXED_NOREPLACE) != 0;

    /*
     * mmap is intentionally strict here. MAP_FIXED and MAP_FIXED_NOREPLACE
     * have different replacement rules, anonymous and file-backed mappings do
     * not share the same setup path, and the VMA is recorded before optional
     * eager population so failure handling stays predictable.
     */
    if (len == 0 || aligned_len == 0 || aligned_len < len)
        return (uint64_t)-EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return (uint64_t)-EINVAL;
    if (map_type != MAP_PRIVATE && map_type != MAP_SHARED &&
        map_type != MAP_SHARED_VALIDATE)
        return (uint64_t)-EINVAL;
    int check_ret = mmap_check_flags_linux(flags, map_type);
    if (check_ret != 0)
        return (uint64_t)check_ret;
    if (offset & page_mask)
        return (uint64_t)-EINVAL;
    if (anonymous && offset != 0)
        return (uint64_t)-EINVAL;
    if ((flags & MAP_SYNC) && anonymous)
        return (uint64_t)-EINVAL;
    task_mm_info_t *mm_info = current_task->mm;
    uint64_t mmap_top = task_mm_mmap_top(mm_info);
    if (mmap_top <= USER_MMAP_START ||
        aligned_len > mmap_top - USER_MMAP_START) {
        return (uint64_t)-ENOMEM;
    }

    fd_t *map_fd_ref = NULL;
    vfs_node_t *map_node = NULL;
    int access_ret = 0;
    if (!anonymous) {
        if (fd >= MAX_FD_NUM)
            return (uint64_t)-EBADF;

        map_fd_ref = task_get_file(current_task, fd);
        if (!map_fd_ref || !map_fd_ref->node) {
            vfs_file_put(map_fd_ref);
            return (uint64_t)-EBADF;
        }

        map_node = map_fd_ref->node;
        if (S_ISDIR(map_node->i_mode))
            goto out_map_fd_eisdir;
        if (flags & MAP_SYNC)
            goto out_map_fd_eopnotsupp;

        access_ret = mmap_check_file_access(map_fd_ref, prot, map_type);
        if (access_ret < 0)
            goto out_map_fd_error;
    }

    vma_manager_t *mgr = &mm_info->task_vma_mgr;
    uint64_t start_addr = 0;
    uint64_t eager_vm_flags = 0;
    uint64_t eager_vm_offset = 0;
    uint64_t accounted_grow = aligned_len;
    bool eager_map_anon = false;
    bool eager_map_file_now = false;

    spin_lock(&mgr->lock);

    if (fixed) {
        if (addr & page_mask) {
            spin_unlock(&mgr->lock);
            mmap_put_fd_ref(map_fd_ref);
            return (uint64_t)-EINVAL;
        }
        if (!user_mmap_range_valid(mm_info, addr, aligned_len)) {
            spin_unlock(&mgr->lock);
            mmap_put_fd_ref(map_fd_ref);
            return (uint64_t)-ENOMEM;
        }

        start_addr = addr;
        if (vma_find_intersection(mgr, start_addr, start_addr + aligned_len)) {
            if (no_replace) {
                spin_unlock(&mgr->lock);
                mmap_put_fd_ref(map_fd_ref);
                return (uint64_t)-EEXIST;
            }

            unsigned long vm_used_before_replace = mgr->vm_used;
            uint64_t ret = do_munmap_locked(start_addr, aligned_len);
            if ((int64_t)ret < 0) {
                spin_unlock(&mgr->lock);
                mmap_put_fd_ref(map_fd_ref);
                return ret;
            }

            /*
             * Linux charges MAP_FIXED replacements only for the net new
             * address-space growth. The overlapped part has already been
             * removed by do_munmap_locked(), so charging the full new mapping
             * length can incorrectly raise ENOMEM for in-place replacements.
             */
            unsigned long unmapped = vm_used_before_replace > mgr->vm_used
                                         ? vm_used_before_replace - mgr->vm_used
                                         : 0;
            accounted_grow =
                unmapped >= aligned_len ? 0 : aligned_len - unmapped;
        }
    } else {
        uint64_t hint = 0;
        if (addr >= USER_MMAP_START && addr < mmap_top)
            hint = PADDING_DOWN(addr, PAGE_SIZE);

        start_addr = find_unmapped_area(mm_info, hint, aligned_len);
        if ((int64_t)start_addr < 0) {
            spin_unlock(&mgr->lock);
            mmap_put_fd_ref(map_fd_ref);
            return start_addr;
        }
    }

    if (check_address_space_limit(mgr, accounted_grow) != 0) {
        spin_unlock(&mgr->lock);
        mmap_put_fd_ref(map_fd_ref);
        return (uint64_t)-ENOMEM;
    }

    vma_t *vma = alloc_mapping_vma(start_addr, aligned_len, prot, map_type,
                                   map_fd_ref ? fd_get_flags(map_fd_ref) : 0,
                                   anonymous, map_node, offset);
    if (!vma) {
        spin_unlock(&mgr->lock);
        goto out_map_fd_nomem;
    }

    if (!anonymous)
        vma->vm_name = mmap_resolve_vm_name(map_fd_ref);

    if (vma_insert(mgr, vma) != 0) {
        spin_unlock(&mgr->lock);
        vma_free(vma);
        goto out_map_fd_nomem;
    }

    eager_vm_flags = vma->vm_flags;
    eager_vm_offset = (uint64_t)vma->vm_offset;
    eager_map_anon = anonymous && should_eager_map_anon(flags);
    eager_map_file_now = !anonymous && should_eager_map_file(vma, flags);

    spin_unlock(&mgr->lock);

    uint64_t ret = start_addr;
    if (eager_map_anon) {
        ret = populate_anon_vma(eager_vm_flags, start_addr, aligned_len, false);
    } else if (eager_map_file_now) {
        ret = map_file_vma(map_fd_ref, eager_vm_offset, start_addr, aligned_len,
                           prot, flags);
    }

    mmap_put_fd_ref(map_fd_ref);
    map_fd_ref = NULL;

    if ((int64_t)ret < 0) {
        unmap_page_range_mm_batched(mm_info, start_addr, aligned_len);
        spin_lock(&mgr->lock);
        vma_remove(mgr, vma);
        spin_unlock(&mgr->lock);
        vma_free(vma);
        return ret;
    }

    spin_lock(&mgr->lock);
    vma_try_merge_around(mgr, &vma);
    spin_unlock(&mgr->lock);

    return start_addr;

out_map_fd_nomem:
    mmap_put_fd_ref(map_fd_ref);
    return (uint64_t)-ENOMEM;

out_map_fd_eisdir:
    mmap_put_fd_ref(map_fd_ref);
    return (uint64_t)-EISDIR;

out_map_fd_eopnotsupp:
    mmap_put_fd_ref(map_fd_ref);
    return (uint64_t)-EOPNOTSUPP;

out_map_fd_error:
    mmap_put_fd_ref(map_fd_ref);
    return (uint64_t)access_ret;
}

static uint64_t do_munmap_locked(uint64_t addr, uint64_t size) {
    addr = PADDING_DOWN(addr, PAGE_SIZE);
    size = PADDING_UP(size, PAGE_SIZE);

    if (size == 0)
        return (uint64_t)-EINVAL;
    if (!user_range_valid(addr, size))
        return (uint64_t)-EFAULT;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    task_mm_info_t *mm = current_task->mm;
    uint64_t end = addr + size;

    if (split_vma_boundaries_locked(mgr, addr, end) != 0)
        return (uint64_t)-ENOMEM;

    while (true) {
        vma_t *vma = vma_find_intersection(mgr, addr, end);
        if (!vma)
            break;
        if (vma->vm_start < addr || vma->vm_end > end)
            return (uint64_t)-ENOMEM;

        uint64_t unmap_start = vma->vm_start;
        uint64_t unmap_len = vma->vm_end - vma->vm_start;

        {
            uint64_t ret = writeback_vma_file_range(vma, unmap_start,
                                                    unmap_start + unmap_len);
            if ((int64_t)ret < 0)
                return ret;
        }

        if (vma_remove(mgr, vma) != 0)
            return (uint64_t)-ENOMEM;
        unmap_vma_resident_range(mm, vma, unmap_start, unmap_start + unmap_len);
        vma_free(vma);
    }

    return 0;
}

static uint64_t do_munmap_detach_locked(uint64_t addr, uint64_t size,
                                        vma_t **detached) {
    if (detached)
        *detached = NULL;

    addr = PADDING_DOWN(addr, PAGE_SIZE);
    size = PADDING_UP(size, PAGE_SIZE);

    if (size == 0)
        return (uint64_t)-EINVAL;
    if (!user_range_valid(addr, size))
        return (uint64_t)-EFAULT;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    uint64_t end = addr + size;
    vma_t *head = NULL;

    if (split_vma_boundaries_locked(mgr, addr, end) != 0)
        return (uint64_t)-ENOMEM;

    for (uint64_t cursor = addr; cursor < end;) {
        vma_t *vma = vma_find_intersection(mgr, cursor, end);
        if (!vma)
            break;
        if (vma_needs_file_writeback(vma))
            return (uint64_t)-EAGAIN;
        cursor = vma->vm_end;
    }

    while (true) {
        vma_t *vma = vma_find_intersection(mgr, addr, end);
        if (!vma)
            break;
        if (vma->vm_start < addr || vma->vm_end > end) {
            for (vma_t *cur = head; cur;) {
                vma_t *next = cur->vm_rb.rb_left
                                  ? rb_entry(cur->vm_rb.rb_left, vma_t, vm_rb)
                                  : NULL;
                vma_insert(mgr, cur);
                cur = next;
            }
            return (uint64_t)-ENOMEM;
        }

        if (vma_remove(mgr, vma) != 0) {
            for (vma_t *cur = head; cur;) {
                vma_t *next = cur->vm_rb.rb_left
                                  ? rb_entry(cur->vm_rb.rb_left, vma_t, vm_rb)
                                  : NULL;
                vma_insert(mgr, cur);
                cur = next;
            }
            return (uint64_t)-ENOMEM;
        }

        vma->vm_rb.rb_left = head ? &head->vm_rb : NULL;
        head = vma;
    }

    if (detached)
        *detached = head;
    return 0;
}

static uint64_t munmap_release_detached(task_mm_info_t *mm, vma_t *list) {
    while (list) {
        vma_t *vma = list;
        list = vma->vm_rb.rb_left ? rb_entry(vma->vm_rb.rb_left, vma_t, vm_rb)
                                  : NULL;
        vma->vm_rb.rb_left = NULL;

        uint64_t start = vma->vm_start;
        uint64_t end = vma->vm_end;

        unmap_vma_resident_range(mm, vma, start, end);
        vma_free(vma);
    }

    return 0;
}

uint64_t sys_munmap(uint64_t addr, uint64_t size) {
    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    task_mm_info_t *mm = current_task->mm;
    vma_t *detached = NULL;
    bool single_owner = __atomic_load_n(&mm->ref_count, __ATOMIC_ACQUIRE) == 1;

    spin_lock(&mgr->lock);
    uint64_t ret;
    if (single_owner) {
        ret = do_munmap_detach_locked(addr, size, &detached);
        if ((int64_t)ret == -EAGAIN)
            ret = do_munmap_locked(addr, size);
    } else {
        ret = do_munmap_locked(addr, size);
    }
    spin_unlock(&mgr->lock);

    if (ret == 0)
        ret = munmap_release_detached(mm, detached);
    return ret;
}

uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return (uint64_t)-EINVAL;

    len = PADDING_UP(len, PAGE_SIZE);
    if (len == 0)
        return (uint64_t)-EINVAL;
    if (!user_range_valid(addr, len))
        return (uint64_t)-EFAULT;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    task_mm_info_t *mm = current_task->mm;
    uint64_t end = addr + len;
    uint64_t new_vm_access = prot_to_vma_access_flags(prot);
    bool ptes_changed = false;

    spin_lock(&mgr->lock);

    if (!range_fully_covered_locked(mgr, addr, end)) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-ENOMEM;
    }

    if (split_vma_boundaries_locked(mgr, addr, end) != 0) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-ENOMEM;
    }

    uint64_t cursor = addr;
    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-ENOMEM;
        }

        if (vma->vm_flags & VMA_GUARD) {
            vma_guard_set_saved_access(vma, new_vm_access);
            vma->vm_flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC);
        } else {
            vma->vm_flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC);
            vma->vm_flags |= new_vm_access;
        }
        cursor = vma->vm_end;
    }

    uint64_t ret = update_vma_pte_attrs_locked(mm, addr, end, &ptes_changed);
    if (ret == 0)
        ret = merge_vma_range_locked(mgr, addr, end);
    spin_unlock(&mgr->lock);

    if (ptes_changed)
        task_mm_flush_tlb_all(mm);
    return ret;
}

static int madvise_guard_validate_vmas(uint64_t addr, uint64_t end) {
    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    spin_lock(&mgr->lock);

    for (uint64_t cursor = addr; cursor < end;) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return -ENOMEM;
        }
        if (vma->vm_type != VMA_TYPE_ANON ||
            (vma->vm_flags & (VMA_SHARED | VMA_SHM | VMA_DEVICE))) {
            spin_unlock(&mgr->lock);
            return -EINVAL;
        }
        cursor = MIN(vma->vm_end, end);
    }

    spin_unlock(&mgr->lock);
    return 0;
}

static uint64_t madvise_guard_install_range(uint64_t addr, uint64_t len) {
    int check_ret = madvise_guard_validate_vmas(addr, addr + len);
    if (check_ret < 0)
        return (uint64_t)check_ret;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    task_mm_info_t *mm = current_task->mm;
    uint64_t end = addr + len;
    uint64_t cursor = addr;
    bool ptes_changed = false;

    spin_lock(&mgr->lock);
    if (split_vma_boundaries_locked(mgr, addr, end) != 0) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-ENOMEM;
    }

    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-ENOMEM;
        }

        if (!(vma->vm_flags & VMA_GUARD)) {
            vma_guard_set_saved_access(vma,
                                       vm_flags_access_bits(vma->vm_flags));
            vma->vm_flags |= VMA_GUARD;
        }
        vma->vm_flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC);
        cursor = MIN(vma->vm_end, end);
    }

    uint64_t ret = update_vma_pte_attrs_locked(mm, addr, end, &ptes_changed);
    if (ret == 0)
        ret = merge_vma_range_locked(mgr, addr, end);
    spin_unlock(&mgr->lock);

    if (ptes_changed)
        task_mm_flush_tlb_all(mm);
    return ret;
}

static uint64_t madvise_guard_remove_range(uint64_t addr, uint64_t len) {
    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    task_mm_info_t *mm = current_task->mm;
    uint64_t end = addr + len;
    uint64_t cursor = addr;
    uint64_t ret = 0;
    bool ptes_changed = false;

    spin_lock(&mgr->lock);
    if (split_vma_boundaries_locked(mgr, addr, end) != 0) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-ENOMEM;
    }

    while (cursor < end) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-ENOMEM;
        }

        if (vma->vm_flags & VMA_GUARD) {
            uint64_t saved_access = vma_guard_saved_access(vma->vm_flags);
            vma->vm_flags &= ~(VMA_READ | VMA_WRITE | VMA_EXEC | VMA_GUARD |
                               VMA_GUARD_SAVED_MASK);
            vma->vm_flags |= saved_access;
        }
        cursor = MIN(vma->vm_end, end);
    }

    ret = update_vma_pte_attrs_locked(mm, addr, end, &ptes_changed);
    if (ret == 0)
        ret = merge_vma_range_locked(mgr, addr, end);
    spin_unlock(&mgr->lock);

    if (ptes_changed)
        task_mm_flush_tlb_all(mm);
    return ret;
}

static vma_t *duplicate_vma(vma_t *src, uint64_t source_addr, uint64_t start,
                            uint64_t size) {
    vma_t *dst = vma_alloc();
    if (!dst)
        return NULL;

    uint64_t source_delta =
        source_addr > src->vm_start ? source_addr - src->vm_start : 0;

    dst->vm_start = start;
    dst->vm_end = start + size;
    dst->vm_flags = src->vm_flags;
    dst->vm_type = src->vm_type;
    dst->node = src->node;
    dst->shm = src->shm;
    dst->shm_id = src->shm_id;
    dst->vm_offset = src->vm_offset;
    dst->vm_file_len = src->vm_file_len;
    dst->vm_file_flags = src->vm_file_flags;

    if (src->vm_type == VMA_TYPE_FILE) {
        uint64_t src_len = src->vm_end - source_addr;
        dst->vm_offset += source_delta;
        if (src->vm_file_len <= source_delta) {
            dst->vm_file_len = 0;
        } else if (src->vm_file_len == src_len + source_delta) {
            dst->vm_file_len = size;
        } else {
            dst->vm_file_len = MIN(src->vm_file_len - source_delta, size);
        }
    }

    if (dst->node)
        vfs_igrab(dst->node);
    if (src->vm_name)
        dst->vm_name = strdup(src->vm_name);

    return dst;
}

static uint64_t mremap_map_new_region(vma_t *new_vma, uint64_t addr,
                                      uint64_t size) {
    if (new_vma->vm_type == VMA_TYPE_ANON)
        return addr;

    if (new_vma->vm_type != VMA_TYPE_FILE)
        return (uint64_t)-EINVAL;

    fd_t fd = {
        .f_op = new_vma->node ? new_vma->node->i_fop : NULL,
        .f_inode = new_vma->node,
        .node = new_vma->node,
        .f_flags = (unsigned int)new_vma->vm_file_flags,
    };

    uint64_t map_flags =
        (new_vma->vm_flags & VMA_SHARED) ? MAP_SHARED : MAP_PRIVATE;
    return map_file_vma(&fd, (uint64_t)new_vma->vm_offset, addr, size,
                        vm_flags_to_prot(new_vma->vm_flags), map_flags);
}

static bool mremap_get_mapped_page(uint64_t *pgdir, uint64_t vaddr,
                                   uint64_t *paddr, uint64_t *flags) {
    uint64_t levels = arch_page_table_levels();
    if (!pgdir || levels == 0 || levels > ARCH_MAX_PT_LEVEL)
        return false;

    uint64_t indexs[ARCH_MAX_PT_LEVEL];
    for (uint64_t i = 0; i < levels; i++)
        indexs[i] = PAGE_TABLE_LEVEL_INDEX(vaddr, i + 1, levels);

    for (uint64_t i = 0; i < levels - 1; i++) {
        uint64_t entry = pgdir[indexs[i]];
        if (ARCH_PT_IS_LARGE(entry))
            return false;
        if (!ARCH_PT_IS_TABLE(entry))
            return false;
        pgdir = (uint64_t *)phys_to_virt(ARCH_READ_PTE(entry));
    }

    uint64_t pte = pgdir[indexs[levels - 1]];
    if (!(pte & ARCH_PT_FLAG_VALID))
        return false;

    if (paddr)
        *paddr = ARCH_READ_PTE(pte);
    if (flags)
        *flags = ARCH_READ_PTE_FLAG(pte);
    return true;
}

static uint64_t mremap_target_pt_flags(uint64_t old_flags,
                                       uint64_t target_vm_flags) {
    uint64_t flags =
        get_arch_page_table_flags(vm_flags_to_pt_flags(target_vm_flags));

    if (old_flags & ARCH_PT_FLAG_COW)
        flags = arch_page_table_flags_make_cow(flags);

    return flags;
}

static uint64_t mremap_map_resident_pages(task_mm_info_t *mm, uint64_t old_addr,
                                          uint64_t new_addr, uint64_t size,
                                          uint64_t target_vm_flags,
                                          uint64_t *mapped_out) {
    if (mapped_out)
        *mapped_out = 0;
    if (size == 0)
        return 0;

    uint64_t *pgdir = mm_pgdir(mm);
    if (!pgdir)
        return (uint64_t)-ENOMEM;

    uint64_t mapped = 0;
    spin_lock(&mm->lock);

    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
        uint64_t paddr = 0;
        uint64_t flags = 0;
        uint64_t src = old_addr + offset;
        uint64_t dst = new_addr + offset;

        if (!mremap_get_mapped_page(pgdir, src, &paddr, &flags))
            continue;

        uint64_t target_flags = mremap_target_pt_flags(flags, target_vm_flags);
        bool new_mapping = false;
        if (map_page(pgdir, dst, paddr, target_flags, true, false,
                     &new_mapping) != 0) {
            if (mapped)
                __atomic_add_fetch(&mm->resident_pages, mapped,
                                   __ATOMIC_RELAXED);
            spin_unlock(&mm->lock);
            if (mapped)
                task_mm_flush_tlb_all(mm);
            return (uint64_t)-ENOMEM;
        }

        if (new_mapping)
            mapped++;
    }

    if (mapped)
        __atomic_add_fetch(&mm->resident_pages, mapped, __ATOMIC_RELAXED);

    spin_unlock(&mm->lock);
    if (mapped)
        task_mm_flush_tlb_all(mm);
    if (mapped_out)
        *mapped_out = mapped;
    return 0;
}

static void mremap_account_shared_file_pages(vma_t *vma, uint64_t old_addr,
                                             uint64_t new_addr, uint64_t size,
                                             uint64_t max_pages) {
    if (!vma || vma->vm_type != VMA_TYPE_FILE || !vma->node ||
        !(vma->vm_flags & VMA_SHARED) || vma->vm_offset < 0 || size == 0 ||
        max_pages == 0)
        return;

    task_mm_info_t *mm = current_task ? current_task->mm : NULL;
    uint64_t *pgdir = task_mm_pgdir(mm);
    uint64_t accounted = 0;
    if (!pgdir)
        return;

    spin_lock(&mm->lock);
    for (uint64_t off = 0; off < size && accounted < max_pages;
         off += PAGE_SIZE) {
        if (!translate_address(pgdir, new_addr + off))
            continue;
        uint64_t source_delta =
            old_addr + off > vma->vm_start ? old_addr + off - vma->vm_start : 0;
        uint64_t file_off = (uint64_t)vma->vm_offset + source_delta;
        page_cache_mmap_inc_page(&vma->node->i_mapping, file_off / PAGE_SIZE);
        accounted++;
    }
    spin_unlock(&mm->lock);
}

static void mremap_unmap_resident_pages(task_mm_info_t *mm, uint64_t addr,
                                        uint64_t size) {
    if (!mm || size == 0)
        return;

    unmap_page_range_mm_batched(mm, addr, size);
}

static vma_t *mremap_find_single_vma_locked(vma_manager_t *mgr, uint64_t addr,
                                            uint64_t size) {
    vma_t *vma = vma_find(mgr, addr);

    if (!vma || addr + size > vma->vm_end)
        return NULL;

    return vma;
}

static int mremap_isolate_source_vma_locked(vma_manager_t *mgr, uint64_t addr,
                                            uint64_t size, vma_t **out_vma) {
    if (!out_vma)
        return -EINVAL;

    uint64_t end = addr + size;

    if (split_vma_boundaries_locked(mgr, addr, addr + size) != 0)
        return -ENOMEM;

    vma_t *vma = vma_find(mgr, addr);
    if (!vma || vma->vm_start != addr)
        return -EFAULT;

    while (vma->vm_end < end) {
        vma_t *next = vma_find(mgr, vma->vm_end);
        if (!next || next->vm_start != vma->vm_end)
            return -EFAULT;
        if (vma_merge(mgr, vma, next) != 0)
            return -EFAULT;
    }

    if (vma->vm_end != end)
        return -EFAULT;

    *out_vma = vma;
    return 0;
}

static uint64_t mremap_shrink_locked(uint64_t old_addr, uint64_t old_size,
                                     uint64_t new_size) {
    uint64_t shrink_start = old_addr + new_size;
    uint64_t shrink_size = old_size - new_size;

    if (shrink_size == 0)
        return old_addr;

    uint64_t ret = do_munmap_locked(shrink_start, shrink_size);
    return (int64_t)ret < 0 ? ret : old_addr;
}

static uint64_t mremap_expand_inplace_locked(task_mm_info_t *mm,
                                             vma_manager_t *mgr, vma_t *vma,
                                             uint64_t old_addr,
                                             uint64_t old_size,
                                             uint64_t new_size) {
    uint64_t old_end = old_addr + old_size;
    uint64_t new_end = old_addr + new_size;
    uint64_t grow = new_size - old_size;

    if (grow == 0)
        return old_addr;
    if (!user_mmap_range_valid(mm, old_addr, new_size))
        return (uint64_t)-ENOMEM;
    if (check_address_space_limit(mgr, grow) != 0)
        return (uint64_t)-ENOMEM;
    if (vma_find_intersection(mgr, old_end, new_end))
        return (uint64_t)-ENOMEM;

    if (vma->vm_type == VMA_TYPE_FILE &&
        ((vma->vm_flags & VMA_SHARED) || (vma->vm_flags & VMA_DEVICE))) {
        fd_t fd = {
            .f_op = vma->node ? vma->node->i_fop : NULL,
            .f_inode = vma->node,
            .node = vma->node,
            .f_flags = (unsigned int)vma->vm_file_flags,
        };
        uint64_t map_flags =
            (vma->vm_flags & VMA_SHARED) ? MAP_SHARED : MAP_PRIVATE;

        if (!fd.f_op || !fd.f_op->mmap)
            return (uint64_t)-ENOSYS;
        uint64_t ret = (uint64_t)fd.f_op->mmap(
            &fd, (void *)old_end, (uint64_t)vma->vm_offset + old_size, grow,
            vm_flags_to_prot(vma->vm_flags), map_flags);
        if ((int64_t)ret < 0)
            return ret;
    }

    vma->vm_end = new_end;
    if (vma->vm_type == VMA_TYPE_FILE && vma->vm_file_len == old_size)
        vma->vm_file_len = new_size;
    if (!(vma->vm_flags & VMA_STACK))
        mgr->vm_used += grow;
    return old_addr;
}

static uint64_t mremap_move_locked(task_mm_info_t *mm, vma_manager_t *mgr,
                                   vma_t *old_vma, uint64_t old_addr,
                                   uint64_t old_size, uint64_t new_size,
                                   uint64_t flags, uint64_t new_addr) {
    uint64_t target = new_addr;

    if (flags & MREMAP_FIXED) {
        if (!user_mmap_range_valid(mm, target, new_size))
            return (uint64_t)-ENOMEM;
        if (ranges_overlap(old_addr, old_addr + old_size, target,
                           target + new_size))
            return (uint64_t)-EINVAL;

        uint64_t ret = do_munmap_locked(target, new_size);
        if ((int64_t)ret < 0)
            return ret;
    } else {
        target = find_unmapped_area(mm, 0, new_size);
        if ((int64_t)target < 0)
            return target;
    }

    uint64_t extra_space =
        (flags & MREMAP_DONTUNMAP || old_size == 0)
            ? new_size
            : (new_size > old_size ? new_size - old_size : 0);
    if (check_address_space_limit(mgr, extra_space) != 0)
        return (uint64_t)-ENOMEM;

    vma_t *new_vma = duplicate_vma(old_vma, old_addr, target, new_size);
    if (!new_vma)
        return (uint64_t)-ENOMEM;

    if (vma_insert(mgr, new_vma) != 0) {
        vma_free(new_vma);
        return (uint64_t)-ENOMEM;
    }

    uint64_t map_ret = mremap_map_new_region(new_vma, target, new_size);
    if ((int64_t)map_ret < 0) {
        unmap_page_range_mm_batched(mm, target, new_size);
        vma_remove(mgr, new_vma);
        vma_free(new_vma);
        return map_ret;
    }

    bool duplicate_mapping = old_size == 0;
    uint64_t copy_len = duplicate_mapping
                            ? new_size
                            : (old_size < new_size ? old_size : new_size);
    uint64_t mapped_pages = 0;
    uint64_t resident_ret = mremap_map_resident_pages(
        mm, old_addr, target, copy_len, new_vma->vm_flags, &mapped_pages);
    if ((int64_t)resident_ret < 0) {
        unmap_page_range_mm_batched(mm, target, new_size);
        vma_remove(mgr, new_vma);
        vma_free(new_vma);
        return resident_ret;
    }
    mremap_account_shared_file_pages(old_vma, old_addr, target, copy_len,
                                     mapped_pages);

    if (duplicate_mapping) {
        /* old_size == 0 duplicates a shareable mapping and leaves source live.
         */
    } else if (flags & MREMAP_DONTUNMAP) {
        mremap_unmap_resident_pages(mm, old_addr, old_size);
    } else {
        uint64_t ret = do_munmap_locked(old_addr, old_size);
        if ((int64_t)ret < 0)
            return ret;
    }

    vma_try_merge_around(mgr, &new_vma);
    return target;
}

uint64_t sys_mremap(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
                    uint64_t flags, uint64_t new_addr) {
    const uint64_t supported_flags =
        MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP;

    uint64_t old_addr_aligned = PADDING_DOWN(old_addr, PAGE_SIZE);
    uint64_t new_addr_aligned = PADDING_DOWN(new_addr, PAGE_SIZE);
    uint64_t old_size_aligned = PADDING_UP(old_size, PAGE_SIZE);
    uint64_t new_size_aligned = PADDING_UP(new_size, PAGE_SIZE);

    if (flags & ~supported_flags)
        return (uint64_t)-EINVAL;
    if (new_size == 0)
        return (uint64_t)-EINVAL;
    if (old_addr != old_addr_aligned)
        return (uint64_t)-EINVAL;
    if (old_size_aligned &&
        !user_range_valid(old_addr_aligned, old_size_aligned))
        return (uint64_t)-EFAULT;
    task_mm_info_t *mm = current_task->mm;
    uint64_t mmap_top = task_mm_mmap_top(mm);
    if (mmap_top <= USER_MMAP_START ||
        new_size_aligned > mmap_top - USER_MMAP_START)
        return (uint64_t)-ENOMEM;
    if ((flags & MREMAP_FIXED) &&
        (!(flags & MREMAP_MAYMOVE) || new_addr != new_addr_aligned))
        return (uint64_t)-EINVAL;
    if ((flags & MREMAP_FIXED) &&
        !user_mmap_range_valid(mm, new_addr_aligned, new_size_aligned))
        return (uint64_t)-ENOMEM;
    if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
        return (uint64_t)-EINVAL;
    if ((flags & MREMAP_DONTUNMAP) && old_size_aligned != new_size_aligned)
        return (uint64_t)-EINVAL;

    if (old_size_aligned == 0) {
        if (!(flags & MREMAP_MAYMOVE) || (flags & MREMAP_DONTUNMAP))
            return (uint64_t)-EINVAL;

        vma_manager_t *mgr = &mm->task_vma_mgr;
        spin_lock(&mgr->lock);

        vma_t *vma = vma_find(mgr, old_addr_aligned);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-EFAULT;
        }
        if (!(vma->vm_flags & VMA_SHARED)) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-EINVAL;
        }
        if (old_addr_aligned + new_size_aligned > vma->vm_end) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-EFAULT;
        }
        if ((flags & MREMAP_FIXED) &&
            ranges_overlap(
                old_addr_aligned, old_addr_aligned + new_size_aligned,
                new_addr_aligned, new_addr_aligned + new_size_aligned)) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-EINVAL;
        }

        uint64_t ret =
            mremap_move_locked(mm, mgr, vma, old_addr_aligned, 0,
                               new_size_aligned, flags, new_addr_aligned);
        spin_unlock(&mgr->lock);
        return ret;
    }

    vma_manager_t *mgr = &mm->task_vma_mgr;
    bool shrink_inplace = !(flags & MREMAP_FIXED) &&
                          !(flags & MREMAP_DONTUNMAP) &&
                          new_size_aligned < old_size_aligned;
    spin_lock(&mgr->lock);

    vma_t *vma = vma_find(mgr, old_addr_aligned);
    if (!vma) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-EFAULT;
    }
    if (vma->vm_type == VMA_TYPE_SHM) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-EINVAL;
    }

    if ((flags & MREMAP_DONTUNMAP) &&
        (vma->vm_type != VMA_TYPE_ANON || (vma->vm_flags & VMA_SHARED))) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-EINVAL;
    }

    uint64_t ret;
    if (new_size_aligned == old_size_aligned && !(flags & MREMAP_FIXED) &&
        !(flags & MREMAP_DONTUNMAP)) {
        if (!mremap_find_single_vma_locked(mgr, old_addr_aligned,
                                           old_size_aligned)) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-EFAULT;
        }
        ret = old_addr_aligned;
    } else if (shrink_inplace) {
        if (!mremap_find_single_vma_locked(mgr, old_addr_aligned,
                                           new_size_aligned)) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-EFAULT;
        }
        ret = mremap_shrink_locked(old_addr_aligned, old_size_aligned,
                                   new_size_aligned);
    } else {
        int prep_ret = mremap_isolate_source_vma_locked(mgr, old_addr_aligned,
                                                        old_size_aligned, &vma);
        if (prep_ret != 0) {
            spin_unlock(&mgr->lock);
            return (uint64_t)prep_ret;
        }

        if (!(flags & MREMAP_FIXED) && new_size_aligned > old_size_aligned &&
            !(flags & MREMAP_DONTUNMAP)) {
            ret = mremap_expand_inplace_locked(mm, mgr, vma, old_addr_aligned,
                                               old_size_aligned,
                                               new_size_aligned);
            if ((int64_t)ret == -ENOMEM && (flags & MREMAP_MAYMOVE)) {
                ret = mremap_move_locked(mm, mgr, vma, old_addr_aligned,
                                         old_size_aligned, new_size_aligned,
                                         flags, new_addr_aligned);
            }
        } else {
            if (!(flags & MREMAP_MAYMOVE)) {
                spin_unlock(&mgr->lock);
                return (uint64_t)-ENOMEM;
            }
            ret = mremap_move_locked(mm, mgr, vma, old_addr_aligned,
                                     old_size_aligned, new_size_aligned, flags,
                                     new_addr_aligned);
        }
    }

    spin_unlock(&mgr->lock);
    return ret;
}

void *general_map(fd_t *file, uint64_t addr, uint64_t len, uint64_t prot,
                  uint64_t flags, uint64_t offset) {
    if (!file || !file->node)
        return (void *)(uint64_t)-EBADF;
    if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0)
        return (void *)addr;

    uint64_t final_pt_flags = PT_FLAG_U;
    if (prot & PROT_READ)
        final_pt_flags |= PT_FLAG_R;
    if (prot & PROT_WRITE)
        final_pt_flags |= PT_FLAG_W;
    if (prot & PROT_EXEC)
        final_pt_flags |= PT_FLAG_X;

    uint64_t map_addr = PADDING_DOWN(addr, PAGE_SIZE);
    uint64_t page_off = addr - map_addr;
    uint64_t map_len = PADDING_UP(len + page_off, PAGE_SIZE);
    uint64_t load_pt_flags = final_pt_flags | PT_FLAG_W;
    task_mm_info_t *mm = current_task->mm;

    uint64_t map_ret = map_page_range_mm_batched(mm, map_addr, (uint64_t)-1,
                                                 map_len, load_pt_flags);
    if (map_ret != 0)
        unmap_page_range_mm_batched(mm, map_addr, map_len);
    if (map_ret != 0)
        return (void *)(uint64_t)-ENOMEM;

    uint64_t remaining = len;
    uint64_t file_off = offset;

    while (remaining > 0) {
        loff_t pos = (loff_t)file_off;
        ssize_t ret = vfs_read_kernel_file(
            file, (void *)(addr + (len - remaining)), remaining, &pos);
        if (ret < 0) {
            unmap_page_range_mm_batched(mm, map_addr, map_len);
            printk("Failed read file for mmap: file_off=%lu req=%lu ret=%ld\n",
                   file_off, remaining, ret);
            return (void *)ret;
        }
        if (ret == 0)
            break;

        file_off += (uint64_t)ret;
        remaining -= (uint64_t)ret;
    }

    if (load_pt_flags != final_pt_flags) {
        map_change_attribute_range_mm_batched(mm, map_addr, map_len,
                                              final_pt_flags, false);
        task_mm_flush_tlb_all(mm);
    }

    return (void *)addr;
}

static uint64_t msync_writeback_file_range(vfs_node_t *node, uint64_t vm_start,
                                           int64_t vm_offset,
                                           uint64_t sync_start,
                                           uint64_t sync_end) {
    if (!node || sync_start >= sync_end || vm_offset < 0)
        return 0;

    if (!node || !node->i_fop)
        return 0;

    uint64_t delta = sync_start - vm_start;
    if ((uint64_t)vm_offset > UINT64_MAX - delta)
        return (uint64_t)-EINVAL;

    uint64_t file_start = (uint64_t)vm_offset + delta;
    uint64_t file_end = file_start + (sync_end - sync_start);
    int ret = page_cache_writeback_range(&node->i_mapping, file_start, file_end,
                                         false);
    if (ret < 0)
        return (uint64_t)ret;

    int sync_ret = vfs_fsync_file_range(
        &(fd_t){
            .f_op = node->i_fop,
            .f_inode = node,
            .node = node,
        },
        (loff_t)file_start, (loff_t)file_end, 0);
    return sync_ret < 0 ? (sync_ret == -ENOSYS ? 0 : (uint64_t)sync_ret) : 0;
}

uint64_t sys_msync(uint64_t addr, uint64_t size, uint64_t flags) {
    const uint64_t supported_flags = 0x1 | 0x2 | 0x4;

    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;
    if (size == 0)
        return 0;
    if (!user_range_valid(addr, size))
        return (uint64_t)-ENOMEM;
    if (flags & ~supported_flags)
        return (uint64_t)-EINVAL;

    uint64_t end = addr + size;
    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;

    spin_lock(&mgr->lock);
    bool covered = range_fully_covered_locked(mgr, addr, end);
    spin_unlock(&mgr->lock);
    if (!covered)
        return (uint64_t)-ENOMEM;

    uint64_t cursor = addr;
    while (cursor < end) {
        vfs_node_t *node = NULL;
        uint64_t vm_start = 0;
        uint64_t vm_end = 0;
        uint64_t vm_flags = 0;
        vma_type_t vm_type = VMA_TYPE_ANON;
        int64_t vm_offset = 0;

        spin_lock(&mgr->lock);
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-ENOMEM;
        }

        vm_start = vma->vm_start;
        vm_end = MIN(vma->vm_end, end);
        vm_flags = vma->vm_flags;
        vm_type = vma->vm_type;
        vm_offset = vma->vm_offset;
        node = vma->node;
        if (node)
            vfs_igrab(node);
        spin_unlock(&mgr->lock);

        if (vm_type == VMA_TYPE_FILE && (vm_flags & VMA_SHARED) &&
            !(vm_flags & VMA_DEVICE) && node) {
            uint64_t ret = msync_writeback_file_range(node, vm_start, vm_offset,
                                                      cursor, vm_end);
            vfs_iput(node);
            if ((int64_t)ret < 0)
                return ret;
        } else if (node) {
            vfs_iput(node);
        }

        cursor = vm_end;
    }

    return 0;
}

uint64_t sys_mincore(uint64_t addr, uint64_t size, uint64_t vec) {
    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;
    if (size == 0)
        return 0;
    if (!user_range_valid(addr, size))
        return (uint64_t)-ENOMEM;

    uint64_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (check_user_overflow(vec, num_pages))
        return (uint64_t)-EFAULT;

    uint64_t end = addr + size;
    uint64_t *page_dir = mm_pgdir(current_task->mm);
    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;

    spin_lock(&mgr->lock);

    if (!range_fully_covered_locked(mgr, addr, end)) {
        spin_unlock(&mgr->lock);
        return (uint64_t)-ENOMEM;
    }

    spin_unlock(&mgr->lock);

    for (uint64_t index = 0, cursor = addr; index < num_pages;
         index++, cursor += PAGE_SIZE) {
        uint8_t value = translate_address(page_dir, cursor) ? 1 : 0;
        if (copy_to_user((void *)vec + index, &value, 1)) {
            return (uint64_t)-EFAULT;
        }
    }

    return 0;
}

static uint64_t madvise_validate_range(uint64_t addr, uint64_t len,
                                       uint64_t *len_out, uint64_t *end_out) {
    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;
    if (len == 0) {
        if (len_out)
            *len_out = 0;
        if (end_out)
            *end_out = addr;
        return 0;
    }
    if (check_user_overflow(addr, len))
        return (uint64_t)-ENOMEM;

    uint64_t aligned_len = PADDING_UP(len, PAGE_SIZE);
    if (aligned_len == 0 || !user_range_valid(addr, aligned_len))
        return (uint64_t)-ENOMEM;

    uint64_t end = addr + aligned_len;
    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;

    spin_lock(&mgr->lock);
    bool covered = range_fully_covered_locked(mgr, addr, end);
    spin_unlock(&mgr->lock);
    if (!covered)
        return (uint64_t)-ENOMEM;

    if (len_out)
        *len_out = aligned_len;
    if (end_out)
        *end_out = end;
    return 0;
}

static int madvise_check_vmas(uint64_t addr, uint64_t end) {
    if (addr >= end)
        return 0;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    spin_lock(&mgr->lock);

    for (uint64_t cursor = addr; cursor < end;) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return -ENOMEM;
        }
        if (vma->vm_type == VMA_TYPE_SHM || (vma->vm_flags & VMA_DEVICE)) {
            spin_unlock(&mgr->lock);
            return -EINVAL;
        }
        cursor = MIN(vma->vm_end, end);
    }

    spin_unlock(&mgr->lock);
    return 0;
}

static int madvise_check_vmas_madv_free(uint64_t addr, uint64_t end) {
    if (addr >= end)
        return 0;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    spin_lock(&mgr->lock);

    for (uint64_t cursor = addr; cursor < end;) {
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return -ENOMEM;
        }

        if (vma->vm_type != VMA_TYPE_ANON ||
            (vma->vm_flags & (VMA_SHARED | VMA_SHM | VMA_DEVICE))) {
            spin_unlock(&mgr->lock);
            return -EINVAL;
        }

        cursor = MIN(vma->vm_end, end);
    }

    spin_unlock(&mgr->lock);
    return 0;
}

static uint64_t madvise_prefault_range(uint64_t addr, uint64_t len,
                                       uint64_t fault_flags) {
    for (uint64_t cursor = addr; cursor < addr + len; cursor += PAGE_SIZE) {
        page_fault_result_t fault =
            handle_page_fault_flags(current_task, cursor, fault_flags);
        if (fault == PF_RES_NOMEM)
            return (uint64_t)-ENOMEM;
    }
    return 0;
}

static uint64_t madvise_dontneed_range(uint64_t addr, uint64_t len) {
    int check_ret = madvise_check_vmas(addr, addr + len);
    if (check_ret < 0)
        return (uint64_t)check_ret;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;
    task_mm_info_t *mm = current_task->mm;
    uint64_t end = addr + len;
    uint64_t cursor = addr;

    while (cursor < end) {
        spin_lock(&mgr->lock);
        vma_t *vma = vma_find(mgr, cursor);
        if (!vma) {
            spin_unlock(&mgr->lock);
            return (uint64_t)-ENOMEM;
        }

        uint64_t range_end = MIN(vma->vm_end, end);
        uint64_t chunk_end = range_end;
        if (vma_needs_file_writeback(vma)) {
            chunk_end = cursor + MADVISE_SHARED_FILE_BATCH_PAGES * PAGE_SIZE;
            if (chunk_end < cursor || chunk_end > range_end)
                chunk_end = range_end;
        }

        uint64_t ret = writeback_vma_file_range(vma, cursor, chunk_end);
        if ((int64_t)ret < 0) {
            spin_unlock(&mgr->lock);
            return ret;
        }

        unmap_release_batch_t batch = {
            .mm = mm,
            .flush_start = addr,
            .flush_end = end,
        };
        uint64_t unmapped = 0;
        uint64_t next = unmap_vma_resident_range_once(
            mm, vma, cursor, chunk_end, &batch, &unmapped);
        spin_unlock(&mgr->lock);

        unmap_release_batch_commit(&batch);
        task_mm_account_unmapped_pages(mm, unmapped);

        if (next <= cursor)
            next = chunk_end;
        cursor = next;
    }

    return 0;
}

static uint64_t madvise_free_range(uint64_t addr, uint64_t len) {
    int check_ret = madvise_check_vmas_madv_free(addr, addr + len);
    if (check_ret < 0)
        return (uint64_t)check_ret;

    /*
     * Linux MADV_FREE only applies to private anonymous memory and permits the
     * kernel to reclaim pages lazily. This kernel does not yet track lazy-free
     * anonymous pages separately, so use the stricter MADV_DONTNEED behavior as
     * a compatibility fallback after enforcing the same VMA constraints.
     */
    return madvise_dontneed_range(addr, len);
}

uint64_t sys_madvise(uint64_t addr, uint64_t len, int behavior) {
    uint64_t aligned_len = 0;
    uint64_t end = 0;
    uint64_t ret = madvise_validate_range(addr, len, &aligned_len, &end);
    if ((int64_t)ret < 0)
        return ret;
    if (aligned_len == 0)
        return 0;

    switch (behavior) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_DONTFORK:
    case MADV_DOFORK:
    case MADV_MERGEABLE:
    case MADV_UNMERGEABLE:
    case MADV_HUGEPAGE:
    case MADV_NOHUGEPAGE:
    case MADV_DONTDUMP:
    case MADV_DODUMP:
    case MADV_WIPEONFORK:
    case MADV_KEEPONFORK:
    case MADV_COLD:
    case MADV_PAGEOUT:
        return 0;
    case MADV_WILLNEED:
    case MADV_POPULATE_READ:
        return madvise_prefault_range(addr, aligned_len, PF_ACCESS_READ);
    case MADV_POPULATE_WRITE:
        return madvise_prefault_range(addr, aligned_len, PF_ACCESS_WRITE);
    case MADV_DONTNEED:
        return madvise_dontneed_range(addr, aligned_len);
    case MADV_FREE:
        return madvise_free_range(addr, aligned_len);
    case MADV_GUARD_INSTALL:
        return madvise_guard_install_range(addr, aligned_len);
    case MADV_GUARD_REMOVE:
        return madvise_guard_remove_range(addr, aligned_len);
    case MADV_REMOVE:
        printk("madvise unsupported behavior=%s(%d) addr=%#018lx "
               "len=%#018lx\n",
               madvise_behavior_name(behavior), behavior, addr, aligned_len);
        return (uint64_t)-EINVAL;
    default:
        printk("madvise unknown behavior=%d addr=%#018lx "
               "len=%#018lx\n",
               behavior, addr, aligned_len);
        return (uint64_t)-EINVAL;
    }
}

static uint64_t mlock_validate_range(uint64_t addr, uint64_t len,
                                     uint64_t *start_out, uint64_t *len_out) {
    if (len == 0)
        return 0;
    if (check_user_overflow(addr, len))
        return (uint64_t)-ENOMEM;

    uint64_t start = PADDING_DOWN(addr, PAGE_SIZE);
    uint64_t end = PADDING_UP(addr + len, PAGE_SIZE);
    if (end <= start)
        return (uint64_t)-ENOMEM;
    if (!user_range_valid(start, end - start))
        return (uint64_t)-ENOMEM;

    vma_manager_t *mgr = &current_task->mm->task_vma_mgr;

    spin_lock(&mgr->lock);
    bool covered = range_fully_covered_locked(mgr, start, end);
    spin_unlock(&mgr->lock);

    if (!covered)
        return (uint64_t)-ENOMEM;

    if (start_out)
        *start_out = start;
    if (len_out)
        *len_out = end - start;

    return 0;
}

uint64_t sys_mlock(uint64_t addr, uint64_t len) {
    uint64_t start = 0;
    uint64_t aligned_len = 0;
    uint64_t ret = mlock_validate_range(addr, len, &start, &aligned_len);
    if ((int64_t)ret < 0 || aligned_len == 0)
        return ret;

    uint64_t *pgdir = mm_pgdir(current_task->mm);
    for (uint64_t cursor = start; cursor < start + aligned_len;
         cursor += PAGE_SIZE) {
        if (translate_address(pgdir, cursor))
            continue;

        page_fault_result_t fault =
            handle_page_fault_flags(current_task, cursor, PF_ACCESS_READ);
        if (fault == PF_RES_NOMEM)
            return (uint64_t)-ENOMEM;
        if (fault != PF_RES_OK)
            return (uint64_t)-ENOMEM;

        pgdir = mm_pgdir(current_task->mm);
    }

    return 0;
}

uint64_t sys_munlock(uint64_t addr, uint64_t len) {
    return mlock_validate_range(addr, len, NULL, NULL);
}

uint64_t sys_mlockall(int flags) {
    const int supported_flags = MCL_CURRENT | MCL_FUTURE;

    if (flags & ~supported_flags)
        return (uint64_t)-EINVAL;
    if ((flags & supported_flags) == 0)
        return (uint64_t)-EINVAL;
    return 0;
}

uint64_t sys_munlockall(void) { return 0; }

uint64_t sys_membarrier(int cmd, unsigned int flags, int cpu_id) {
    (void)cpu_id;

    if (flags != 0)
        return (uint64_t)-EINVAL;

    if (cmd == MEMBARRIER_CMD_QUERY)
        return membarrier_supported_mask();

    task_t *self = current_task;
    if (!self || !self->mm)
        return (uint64_t)-EINVAL;

    task_mm_info_t *mm = self->mm;

    switch (cmd) {
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
        __atomic_store_n(&mm->membarrier_private_expedited_registered, true,
                         __ATOMIC_RELEASE);
        return 0;

    case MEMBARRIER_CMD_PRIVATE_EXPEDITED: {
        if (!__atomic_load_n(&mm->membarrier_private_expedited_registered,
                             __ATOMIC_ACQUIRE))
            return (uint64_t)-EPERM;

        uint64_t seq = __atomic_add_fetch(&mm->membarrier_private_expedited_seq,
                                          1, __ATOMIC_ACQ_REL);
        task_membarrier_checkpoint(self);
        return membarrier_private_expedited_wait(mm, seq) ? 0 : (uint64_t)-EIO;
    }

    default:
        return (uint64_t)-EINVAL;
    }
}

uint64_t sys_mbind(uint64_t start, uint64_t len, int mode,
                   const unsigned long *nmask, uint64_t maxnode,
                   uint64_t flags) {
    (void)start;
    (void)len;
    (void)nmask;
    (void)maxnode;

    const uint64_t supported_flags = 0;
    if (!mempolicy_mode_valid(mode))
        return (uint64_t)-EINVAL;
    if (flags & ~supported_flags)
        return (uint64_t)-EINVAL;

    return 0;
}

uint64_t sys_set_mempolicy(int mode, const unsigned long *nmask,
                           uint64_t maxnode) {
    (void)nmask;
    (void)maxnode;

    if (!mempolicy_mode_valid(mode))
        return (uint64_t)-EINVAL;

    return 0;
}

uint64_t sys_get_mempolicy(int *policy, unsigned long *nmask, uint64_t maxnode,
                           uint64_t addr, uint64_t flags) {
    (void)addr;

    const uint64_t supported_flags =
        MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED;

    if (flags & ~supported_flags)
        return (uint64_t)-EINVAL;
    if ((flags & MPOL_F_MEMS_ALLOWED) && (flags & (MPOL_F_NODE | MPOL_F_ADDR)))
        return (uint64_t)-EINVAL;

    uint64_t ret = mempolicy_copy_nodemask_to_user(nmask, maxnode);
    if ((int64_t)ret < 0)
        return ret;

    if (policy) {
        int value = MPOL_DEFAULT;
        if (flags & MPOL_F_NODE)
            value = 0;
        else if (flags & MPOL_F_MEMS_ALLOWED)
            value = MPOL_DEFAULT;

        if (copy_to_user(policy, &value, sizeof(value)))
            return (uint64_t)-EFAULT;
    }

    return 0;
}
