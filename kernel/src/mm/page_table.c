#include <arch/arch.h>
#include <mm/cache.h>
#include <mm/mm.h>
#include <mm/page.h>
#include <task/task.h>

static inline bool page_table_levels_valid(uint64_t levels) {
    return levels > 0 && levels <= ARCH_MAX_PT_LEVEL;
}

uint64_t translate_address(uint64_t *pgdir, uint64_t vaddr) {
    if (!vaddr)
        return 0;

    uint64_t levels = arch_page_table_levels();
    if (!page_table_levels_valid(levels))
        return 0;
    uint64_t indexs[ARCH_MAX_PT_LEVEL];
    for (uint64_t i = 0; i < levels; i++) {
        indexs[i] = PAGE_TABLE_LEVEL_INDEX(vaddr, i + 1, levels);
    }

    for (uint64_t i = 0; i < levels - 1; i++) {
        uint64_t index = indexs[i];
        uint64_t addr = pgdir[index];
        if (ARCH_PT_IS_LARGE(addr)) {
            uint64_t mask = PAGE_TABLE_LEVEL_MASK(i + 1, levels);
            return (ARCH_READ_PTE(pgdir[index]) & ~mask) + (vaddr & mask);
        }
        if (!ARCH_PT_IS_TABLE(addr)) {
            return 0;
        }
        pgdir = (uint64_t *)phys_to_virt(ARCH_READ_PTE(addr));
    }

    uint64_t index = indexs[levels - 1];
    uint64_t pte = pgdir[index];
    if (!(pte & ARCH_PT_FLAG_VALID))
        return 0;

    return ARCH_READ_PTE(pte) + (vaddr & PAGE_TABLE_LEVEL_MASK(levels, levels));
}

uint64_t *kernel_page_dir = NULL;

uint64_t *get_kernel_page_dir() { return kernel_page_dir; }

typedef struct deferred_unmap_release {
    struct deferred_unmap_release *next;
    uint64_t page_addrs[UNMAP_RELEASE_BATCH_MAX];
    size_t page_count;
    uint64_t table_addrs[UNMAP_RELEASE_TABLE_BATCH_MAX];
    size_t table_count;
    task_mm_info_t *mm;
} deferred_unmap_release_t;

static spinlock_t deferred_unmap_lock = SPIN_INIT;
static deferred_unmap_release_t *deferred_unmap_head;

static inline void unmap_release_page(uint64_t paddr) {
    if (paddr)
        address_release(paddr);
}

static inline void unmap_release_table(uint64_t table_phys_addr) {
    if (table_phys_addr)
        address_release(table_phys_addr);
}

static inline void unmap_batch_queue_page(unmap_release_batch_t *batch,
                                          uint64_t paddr) {
    if (!batch || !paddr)
        return;

    ASSERT(batch->page_count < UNMAP_RELEASE_BATCH_MAX);
    batch->page_addrs[batch->page_count++] = paddr;
}

static inline void unmap_batch_queue_table(unmap_release_batch_t *batch,
                                           uint64_t table_phys_addr) {
    if (!batch || !table_phys_addr)
        return;

    ASSERT(batch->table_count < UNMAP_RELEASE_TABLE_BATCH_MAX);
    batch->table_addrs[batch->table_count++] = table_phys_addr;
}

static void unmap_release_addrs(uint64_t *pages, size_t page_count,
                                uint64_t *tables, size_t table_count) {
    for (size_t i = 0; i < page_count; i++)
        unmap_release_page(pages[i]);

    for (size_t i = 0; i < table_count; i++)
        unmap_release_table(tables[i]);
}

static bool unmap_release_can_wait(void) {
    task_t *self = current_task;
    return !self || self->preempt_count == 0;
}

static void unmap_defer_release(unmap_release_batch_t *batch) {
    if (!batch || (!batch->page_count && !batch->table_count))
        return;

    deferred_unmap_release_t *entry = malloc(sizeof(*entry));
    ASSERT(entry != NULL);

    entry->next = NULL;
    entry->page_count = batch->page_count;
    entry->table_count = batch->table_count;
    entry->mm = batch->mm;
    if (entry->mm)
        __atomic_add_fetch(&entry->mm->ref_count, 1, __ATOMIC_ACQ_REL);
    memcpy(entry->page_addrs, batch->page_addrs,
           batch->page_count * sizeof(batch->page_addrs[0]));
    memcpy(entry->table_addrs, batch->table_addrs,
           batch->table_count * sizeof(batch->table_addrs[0]));

    spin_lock(&deferred_unmap_lock);
    entry->next = deferred_unmap_head;
    deferred_unmap_head = entry;
    spin_unlock(&deferred_unmap_lock);
}

void unmap_release_deferred_drain(void) {
    if (!unmap_release_can_wait())
        return;

    while (true) {
        spin_lock(&deferred_unmap_lock);
        deferred_unmap_release_t *entry = deferred_unmap_head;
        if (entry)
            deferred_unmap_head = entry->next;
        spin_unlock(&deferred_unmap_lock);

        if (!entry)
            break;

        if (!entry->mm || task_mm_flush_tlb_all(entry->mm)) {
            unmap_release_addrs(entry->page_addrs, entry->page_count,
                                entry->table_addrs, entry->table_count);
            task_mm_info_t *mm = entry->mm;
            free(entry);
            if (mm)
                free_page_table(mm);
            continue;
        }

        spin_lock(&deferred_unmap_lock);
        entry->next = deferred_unmap_head;
        deferred_unmap_head = entry;
        spin_unlock(&deferred_unmap_lock);
        break;
    }
}

typedef struct page_map_path {
    uint64_t *leaf;
    uint64_t *created_parent_tables[ARCH_MAX_PT_LEVEL - 1];
    uint64_t created_parent_indices[ARCH_MAX_PT_LEVEL - 1];
    uint64_t created_table_addrs[ARCH_MAX_PT_LEVEL - 1];
    size_t created_tables;
    // Large pages split during the walk, restored on rollback.
    uint64_t *split_parent_tables[ARCH_MAX_PT_LEVEL - 1];
    uint64_t split_parent_indices[ARCH_MAX_PT_LEVEL - 1];
    uint64_t split_old_entries[ARCH_MAX_PT_LEVEL - 1];
    size_t split_tables;
} page_map_path_t;

static void page_map_path_rollback(page_map_path_t *path) {
    while (path->created_tables > 0) {
        path->created_tables--;
        path->created_parent_tables
            [path->created_tables]
            [path->created_parent_indices[path->created_tables]] = 0;
        unmap_release_table(path->created_table_addrs[path->created_tables]);
    }
    while (path->split_tables > 0) {
        path->split_tables--;
        uint64_t current = path->split_parent_tables
                               [path->split_tables]
                               [path->split_parent_indices[path->split_tables]];
        unmap_release_table(ARCH_READ_PTE(current));
        path->split_parent_tables
            [path->split_tables]
            [path->split_parent_indices[path->split_tables]] =
            path->split_old_entries[path->split_tables];
    }
}

static bool page_map_split_large(uint64_t *pgdir, uint64_t index,
                                 uint64_t level, uint64_t levels,
                                 uint64_t vaddr, uint64_t flags, bool flush,
                                 page_map_path_t *path) {
    if (path->split_tables >= ARCH_MAX_PT_LEVEL - 1)
        return false;

    uint64_t base = ARCH_READ_PTE(pgdir[index]);
    uint64_t old_flags = ARCH_READ_PTE_FLAG(pgdir[index]);

    uint64_t a = alloc_frames(1);
    if (a == 0)
        return false;
    uint64_t *table = (uint64_t *)phys_to_virt(a);
    memset(table, 0, PAGE_SIZE);

    uint64_t sub_shift =
        ARCH_PT_OFFSET_BASE + ARCH_PT_OFFSET_PER_LEVEL * (levels - 2 - level);
    uint64_t sub_size = 1UL << sub_shift;
    bool sub_is_huge = (level + 2 < levels);

    for (uint64_t j = 0; j < (1UL << ARCH_PT_OFFSET_PER_LEVEL); j++) {
        uint64_t sub_paddr = base + j * sub_size;
        if (sub_is_huge)
            table[j] = ARCH_MAKE_HUGE_PTE(sub_paddr, old_flags);
        else
            table[j] = ARCH_MAKE_PTE(sub_paddr, old_flags);
    }

    path->split_parent_tables[path->split_tables] = pgdir;
    path->split_parent_indices[path->split_tables] = index;
    path->split_old_entries[path->split_tables] = pgdir[index];
    path->split_tables++;

    pgdir[index] = arch_make_page_table_entry(a, flags);
    if (flush)
        arch_flush_tlb(vaddr);
    return true;
}

static bool page_map_find_leaf(uint64_t *pgdir, uint64_t vaddr, uint64_t flags,
                               bool flush, uint64_t levels,
                               page_map_path_t *path) {
    memset(path, 0, sizeof(*path));

    for (uint64_t i = 0; i < levels - 1; i++) {
        uint64_t index = PAGE_TABLE_LEVEL_INDEX(vaddr, i + 1, levels);
        uint64_t addr = pgdir[index];
        if (ARCH_PT_IS_LARGE(addr)) {
            if (!page_map_split_large(pgdir, index, i, levels, vaddr, flags,
                                      flush, path))
                goto fail;
            addr = pgdir[index];
        }

        if (!ARCH_PT_IS_TABLE(addr)) {
            uint64_t a = alloc_frames(1);
            if (a == 0)
                goto fail;
            memset((uint64_t *)phys_to_virt(a), 0, PAGE_SIZE);
            pgdir[index] = arch_make_page_table_entry(a, flags);
            path->created_parent_tables[path->created_tables] = pgdir;
            path->created_parent_indices[path->created_tables] = index;
            path->created_table_addrs[path->created_tables] = a;
            path->created_tables++;
        } else if ((flags & ARCH_PT_FLAG_USER) && !(addr & ARCH_PT_FLAG_USER)) {
            uint64_t pa = ARCH_READ_PTE(addr);
            uint64_t old_flags = ARCH_READ_PTE_FLAG(addr);
            pgdir[index] = arch_make_page_table_entry(pa, old_flags | flags);
            if (flush)
                arch_flush_tlb(vaddr);
        }

        pgdir = (uint64_t *)phys_to_virt(ARCH_READ_PTE(pgdir[index]));
    }

    path->leaf = pgdir;
    return true;

fail:
    page_map_path_rollback(path);
    return false;
}

static uint64_t page_map_leaf(page_map_path_t *path, uint64_t index,
                              uint64_t vaddr, uint64_t paddr, uint64_t flags,
                              bool force, bool flush, bool *new_mapping) {
    if (new_mapping)
        *new_mapping = false;

    bool had_old_mapping = (path->leaf[index] & ARCH_PT_FLAG_VALID) != 0;
    uint64_t old_paddr = 0;
    if (had_old_mapping) {
        if (!force) {
            path->created_tables = 0;
            return 0;
        }
        old_paddr = ARCH_READ_PTE(path->leaf[index]);
    }

    if (paddr == (uint64_t)-1) {
        uint64_t phys = alloc_frames(1);
        if (phys == 0) {
            printk("Cannot allocate frame\n");
            goto fail;
        }
        memset((void *)phys_to_virt(phys), 0, PAGE_SIZE);
        paddr = phys;
    } else if (paddr && (!had_old_mapping || old_paddr != paddr) &&
               !address_ref(paddr)) {
        goto fail;
    }

    path->leaf[index] = ARCH_MAKE_PTE(paddr, flags);
    if (new_mapping)
        *new_mapping = !had_old_mapping;

    if (flush || (had_old_mapping && old_paddr && old_paddr != paddr))
        arch_flush_tlb(vaddr);

    if (had_old_mapping && old_paddr && old_paddr != paddr)
        address_release(old_paddr);

    path->created_tables = 0;
    return 0;

fail:
    page_map_path_rollback(path);
    return (uint64_t)-1;
}

uint64_t map_page(uint64_t *pgdir, uint64_t vaddr, uint64_t paddr,
                  uint64_t flags, bool force, bool flush, bool *new_mapping) {
    ASSERT((vaddr & 0xfff) == 0);
    ASSERT(paddr == (uint64_t)-1 || (paddr & 0xfff) == 0);

    if (new_mapping)
        *new_mapping = false;

    uint64_t levels = arch_page_table_levels();
    if (!pgdir || !page_table_levels_valid(levels))
        return (uint64_t)-1;

    page_map_path_t path;
    if (!page_map_find_leaf(pgdir, vaddr, flags, flush, levels, &path))
        return (uint64_t)-1;

    uint64_t index = PAGE_TABLE_LEVEL_INDEX(vaddr, levels, levels);
    return page_map_leaf(&path, index, vaddr, paddr, flags, force, flush,
                         new_mapping);
}

uint64_t map_pages(uint64_t *pgdir, uint64_t vaddr, uint64_t paddr,
                   uint64_t size, uint64_t flags, bool force,
                   uint64_t *new_mappings) {
    ASSERT((vaddr & 0xfff) == 0);
    ASSERT(paddr == (uint64_t)-1 || (paddr & 0xfff) == 0);

    if (new_mappings)
        *new_mappings = 0;
    if (!size)
        return 0;

    uint64_t levels = arch_page_table_levels();
    if (!pgdir || !page_table_levels_valid(levels))
        return (uint64_t)-1;

    uint64_t page_count = size / PAGE_SIZE + ((size % PAGE_SIZE) != 0);
    uint64_t entries = (uint64_t)1 << ARCH_PT_OFFSET_PER_LEVEL;

    for (uint64_t page = 0; page < page_count;) {
        uint64_t offset = page * PAGE_SIZE;
        uint64_t va = vaddr + offset;
        if (va < vaddr)
            return (uint64_t)-1;

        page_map_path_t path;
        if (!page_map_find_leaf(pgdir, va, flags, false, levels, &path))
            return (uint64_t)-1;

        uint64_t index = PAGE_TABLE_LEVEL_INDEX(va, levels, levels);
        uint64_t chunk_pages = MIN(entries - index, page_count - page);
        for (uint64_t i = 0; i < chunk_pages; i++, page++) {
            uint64_t map_paddr =
                paddr == (uint64_t)-1 ? (uint64_t)-1 : paddr + page * PAGE_SIZE;
            bool new_mapping = false;
            uint64_t ret =
                page_map_leaf(&path, index + i, vaddr + page * PAGE_SIZE,
                              map_paddr, flags, force, false, &new_mapping);
            if (ret != 0)
                return ret;
            if (new_mapping && new_mappings)
                (*new_mappings)++;
        }
    }

    return 0;
}

uint64_t unmap_page_defer_release(uint64_t *pgdir, uint64_t vaddr,
                                  unmap_release_batch_t *batch, bool flush,
                                  bool reclaim_tables) {
    uint64_t levels = arch_page_table_levels();
    if (!page_table_levels_valid(levels))
        return 0;
    uint64_t indexs[ARCH_MAX_PT_LEVEL];
    uint64_t *table_ptrs[ARCH_MAX_PT_LEVEL];
    uint64_t table_indices[ARCH_MAX_PT_LEVEL];

    for (uint64_t i = 0; i < levels; i++) {
        indexs[i] = PAGE_TABLE_LEVEL_INDEX(vaddr, i + 1, levels);
    }

    // 保存每一级页表的指针和索引
    table_ptrs[0] = pgdir;
    table_indices[0] = indexs[0];

    // 遍历页表层级
    for (uint64_t i = 0; i < levels - 1; i++) {
        uint64_t index = table_indices[i];
        uint64_t addr = table_ptrs[i][index];
        if (ARCH_PT_IS_LARGE(addr)) {
            return 0; // 大页映射，不支持部分释放
        }
        if (!ARCH_PT_IS_TABLE(addr)) {
            return 0; // 页表不存在
        }
        table_ptrs[i + 1] = (uint64_t *)phys_to_virt(ARCH_READ_PTE(addr));
        table_indices[i + 1] = indexs[i + 1];
    }

    // 处理最底层页表
    uint64_t index = table_indices[levels - 1];
    uint64_t pte = table_ptrs[levels - 1][index];
    uint64_t paddr = ARCH_READ_PTE(pte);

    if (paddr != 0) {
        table_ptrs[levels - 1][index] = 0;
        if (flush)
            arch_flush_tlb(vaddr);
        if (batch) {
            unmap_batch_queue_page(batch, paddr);
        } else {
            unmap_release_page(paddr);
        }

        if (reclaim_tables) {
            // 从最底层页表开始向上检查并释放空页表
            for (int level = (int)levels - 1; level > 0; level--) {
                uint64_t *current_table = table_ptrs[level];
                bool table_empty = true;

                for (uint64_t i = 0; i < 512; i++) {
                    if (current_table[i] != 0) {
                        table_empty = false;
                        break;
                    }
                }

                if (table_empty) {
                    // 释放空页表
                    uint64_t table_phys_addr = virt_to_phys(current_table);
                    if (batch) {
                        unmap_batch_queue_table(batch, table_phys_addr);
                    } else {
                        unmap_release_table(table_phys_addr);
                    }

                    // 清除上级页表中的对应条目
                    uint64_t *parent_table = table_ptrs[level - 1];
                    uint64_t parent_index = table_indices[level - 1];
                    parent_table[parent_index] = 0;
                } else {
                    // 页表不为空，停止向上检查
                    break;
                }
            }
        }
    }

    return paddr;
}

uint64_t unmap_page(uint64_t *pgdir, uint64_t vaddr) {
    return unmap_page_defer_release(pgdir, vaddr, NULL, true, true);
}

static uint64_t page_table_entries_per_level(void) {
    return (uint64_t)1 << ARCH_PT_OFFSET_PER_LEVEL;
}

static uint64_t page_table_region_size(uint64_t level, uint64_t levels) {
    uint64_t span = PAGE_TABLE_LEVEL_SIZE(level, levels);
    uint64_t entries = page_table_entries_per_level();

    if (!span || span > UINT64_MAX / entries)
        return UINT64_MAX;
    return span * entries;
}

static uint64_t pt_range_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

static uint64_t pt_range_max(uint64_t a, uint64_t b) { return a > b ? a : b; }

static uint64_t unmap_present_range(uint64_t *table, uint64_t level,
                                    uint64_t table_base, uint64_t start,
                                    uint64_t end, unmap_release_batch_t *batch,
                                    uint64_t max_scan, uint64_t *scanned,
                                    uint64_t *unmapped, uint64_t levels) {
    if (!table || start >= end || level == 0 || level > levels)
        return end;

    uint64_t span = PAGE_TABLE_LEVEL_SIZE(level, levels);
    if (!span)
        return end;

    uint64_t entries = page_table_entries_per_level();
    uint64_t first = PAGE_TABLE_LEVEL_INDEX(start, level, levels);
    uint64_t last = PAGE_TABLE_LEVEL_INDEX(end - 1, level, levels);
    if (first >= entries)
        first = entries - 1;
    if (last >= entries)
        last = entries - 1;

    for (uint64_t index = first; index <= last; index++) {
        uint64_t entry_base = table_base + index * span;
        uint64_t entry_end = entry_base + span;
        if (entry_end < entry_base)
            entry_end = UINT64_MAX;
        if (entry_end <= start || entry_base >= end)
            continue;

        if (level == levels) {
            if (batch && batch->page_count >= UNMAP_RELEASE_BATCH_MAX)
                return entry_base;
            if (max_scan && *scanned >= max_scan)
                return entry_base;
            (*scanned)++;

            uint64_t entry = table[index];
            uint64_t paddr = ARCH_READ_PTE(entry);
            if (!paddr)
                continue;

            table[index] = 0;
            if (batch) {
                unmap_batch_queue_page(batch, paddr);
            } else {
                unmap_release_page(paddr);
            }
            if (unmapped)
                (*unmapped)++;
            continue;
        }

        uint64_t entry = table[index];
        if (ARCH_PT_IS_LARGE(entry)) {
            /*
             * Existing range unmap does not partially release huge mappings.
             * Keep that behavior here and only skip the covered span.
             */
            continue;
        }
        if (!ARCH_PT_IS_TABLE(entry))
            continue;

        uint64_t *child = (uint64_t *)phys_to_virt(ARCH_READ_PTE(entry));
        uint64_t ret = unmap_present_range(child, level + 1, entry_base,
                                           pt_range_max(start, entry_base),
                                           pt_range_min(end, entry_end), batch,
                                           max_scan, scanned, unmapped, levels);
        if (ret < pt_range_min(end, entry_end))
            return ret;
    }

    return end;
}

uint64_t unmap_page_range_defer_release(uint64_t *pgdir, uint64_t vaddr,
                                        uint64_t end,
                                        unmap_release_batch_t *batch,
                                        uint64_t max_scan, uint64_t *unmapped) {
    if (!pgdir || vaddr >= end)
        return end;

    uint64_t levels = arch_page_table_levels();
    if (!page_table_levels_valid(levels))
        return end;

    uint64_t region_size = page_table_region_size(1, levels);
    uint64_t cursor = vaddr;
    uint64_t scanned = 0;

    while (cursor < end && (!max_scan || scanned < max_scan)) {
        uint64_t table_base =
            region_size == UINT64_MAX ? 0 : (cursor & ~(region_size - 1));
        uint64_t table_end = table_base + region_size;
        if (table_end < table_base)
            table_end = UINT64_MAX;
        uint64_t chunk_end = pt_range_min(end, table_end);
        if (chunk_end <= cursor)
            chunk_end = end;

        uint64_t next =
            unmap_present_range(pgdir, 1, table_base, cursor, chunk_end, batch,
                                max_scan, &scanned, unmapped, levels);
        if (next < chunk_end)
            return next;
        cursor = chunk_end;
    }

    return cursor;
}

uint64_t map_change_attribute(uint64_t *pgdir, uint64_t vaddr, uint64_t flags,
                              bool flush) {
    uint64_t levels = arch_page_table_levels();
    if (!page_table_levels_valid(levels))
        return 0;
    uint64_t indexs[ARCH_MAX_PT_LEVEL];
    for (uint64_t i = 0; i < levels; i++) {
        indexs[i] = PAGE_TABLE_LEVEL_INDEX(vaddr, i + 1, levels);
    }

    for (uint64_t i = 0; i < levels - 1; i++) {
        uint64_t index = indexs[i];
        uint64_t addr = pgdir[index];
        if (ARCH_PT_IS_LARGE(addr)) {
            uint64_t old_flags = ARCH_READ_PTE_FLAG(pgdir[index]);
            uint64_t keep_flags = old_flags & ARCH_PT_SOFT_FLAGS;
            uint64_t old_paddr = ARCH_READ_PTE(pgdir[index]);
            uint64_t new_flags = flags | keep_flags;
            pgdir[index] = ARCH_MAKE_HUGE_PTE(old_paddr, new_flags);
            if (flush)
                arch_flush_tlb(vaddr);
            return 0;
        }
        if (!ARCH_PT_IS_TABLE(addr)) {
            return 0;
        }
        pgdir = (uint64_t *)phys_to_virt(ARCH_READ_PTE(addr));
    }

    uint64_t index = indexs[levels - 1];
    if (!(pgdir[index] & ARCH_PT_FLAG_VALID)) {
        return 0;
    }

    uint64_t old_paddr = ARCH_READ_PTE(pgdir[index]);
    uint64_t old_flags = ARCH_READ_PTE_FLAG(pgdir[index]);
    uint64_t keep_flags = old_flags & ARCH_PT_SOFT_FLAGS;
    uint64_t new_flags = flags | keep_flags;
    pgdir[index] = ARCH_MAKE_PTE(old_paddr, new_flags);

    if (flush)
        arch_flush_tlb(vaddr);

    return 0;
}

static void free_page_table_recursive(uint64_t *table, int level);

static uint64_t page_table_entry_span(int level) {
    uint64_t levels = arch_page_table_levels();
    if (!page_table_levels_valid(levels) || level <= 0 ||
        (uint64_t)level > levels)
        return 0;
    return PAGE_TABLE_LEVEL_SIZE(levels - level + 1, levels);
}

static bool vma_is_private_mapping(vma_t *vma) {
    return vma && !(vma->vm_flags & (VMA_SHARED | VMA_SHM | VMA_DEVICE));
}

static bool vma_is_shared_file_mapping(vma_t *vma) {
    return vma && vma->vm_type == VMA_TYPE_FILE && vma->node &&
           (vma->vm_flags & VMA_SHARED) && !(vma->vm_flags & VMA_DEVICE) &&
           vma->vm_offset >= 0;
}

static void page_table_account_shared_file_mappings(task_mm_info_t *mm,
                                                    bool add) {
    if (!mm)
        return;

    uint64_t *pgdir = task_mm_pgdir(mm);
    if (!pgdir)
        return;

    vma_manager_t *mgr = &mm->task_vma_mgr;
    rb_node_t *node = rb_first(&mgr->vma_tree);
    while (node) {
        vma_t *vma = rb_entry(node, vma_t, vm_rb);
        node = rb_next(node);
        if (!vma_is_shared_file_mapping(vma))
            continue;

        for (uint64_t va = vma->vm_start; va < vma->vm_end; va += PAGE_SIZE) {
            if (!translate_address(pgdir, va))
                continue;
            uint64_t file_off = (uint64_t)vma->vm_offset + (va - vma->vm_start);
            if (add)
                page_cache_mmap_inc_page(&vma->node->i_mapping,
                                         file_off / PAGE_SIZE);
            else
                page_cache_mmap_dec_page(&vma->node->i_mapping,
                                         file_off / PAGE_SIZE);
        }
    }
}

static uint64_t *copy_page_table_recursive(uint64_t *source_table, int level,
                                           uint64_t base_vaddr,
                                           vma_manager_t *mgr,
                                           bool *source_changed) {
    if (!source_table)
        return NULL;

    uint64_t frame = alloc_frames(1);
    if (!frame)
        return NULL;

    uint64_t *new_table = (uint64_t *)phys_to_virt(frame);
    memset(new_table, 0, PAGE_SIZE);

    uint64_t entries = arch_page_table_root_entries(level);

    for (uint64_t i = 0; i < entries; i++) {
        uint64_t entry = source_table[i];
        uint64_t entry_vaddr = base_vaddr + i * page_table_entry_span(level);
        if (!entry)
            continue;

        if (level == 1) {
            uint64_t flags = ARCH_READ_PTE_FLAG(entry);
            if (!(flags & ARCH_PT_FLAG_VALID)) {
                new_table[i] = entry;
                continue;
            }

            uint64_t paddr = ARCH_READ_PTE(entry);
            bool managed = paddr && address_is_managed(paddr);

            if (managed && !address_ref(paddr)) {
                free_page_table_recursive(new_table, level);
                return NULL;
            }

            if (managed && vma_is_private_mapping(vma_find(mgr, entry_vaddr)) &&
                arch_page_table_flags_writable(flags)) {
                flags = arch_page_table_flags_make_cow(flags);
                source_table[i] = ARCH_MAKE_PTE(paddr, flags);
                if (source_changed)
                    *source_changed = true;
            }

            new_table[i] = ARCH_MAKE_PTE(paddr, flags);
            continue;
        }

        if (ARCH_PT_IS_TABLE(entry)) {
            uint64_t *child_src =
                (uint64_t *)phys_to_virt(ARCH_READ_PTE(entry));
            uint64_t *child_new = copy_page_table_recursive(
                child_src, level - 1, entry_vaddr, mgr, source_changed);
            if (!child_new) {
                free_page_table_recursive(new_table, level);
                return NULL;
            }

            new_table[i] = ARCH_MAKE_PDE(virt_to_phys(child_new),
                                         ARCH_READ_PTE_FLAG(entry));
            continue;
        }

        new_table[i] = entry;
    }

    return new_table;
}

static void free_page_table_recursive(uint64_t *table, int level) {
    if (!table)
        return;

    uint64_t table_phys = virt_to_phys(table);
    if (!table_phys)
        return;

    uint64_t entries = arch_page_table_root_entries(level);

    if (level == 1) {
        for (uint64_t i = 0; i < entries; i++) {
            uint64_t pte = table[i];

            uint64_t paddr = ARCH_READ_PTE(pte);
            uint64_t flags = ARCH_READ_PTE_FLAG(pte);
            if (!(flags & ARCH_PT_FLAG_VALID))
                continue;
            if (paddr) {
                address_release(paddr);
            }
        }
    } else {
        for (uint64_t i = 0; i < entries; i++) {
            uint64_t entry = table[i];
            if (!ARCH_PT_IS_TABLE(entry))
                continue;

            uint64_t paddr = ARCH_READ_PTE(entry);

            uint64_t *page_table_next = (uint64_t *)phys_to_virt(paddr);
            free_page_table_recursive(page_table_next, level - 1);
        }
    }

    address_release(table_phys);
}

task_mm_info_t *clone_page_table(task_mm_info_t *old, uint64_t clone_flags) {
    if (!old)
        return NULL;

    vma_manager_t *mgr = &old->task_vma_mgr;

    if (clone_flags & CLONE_VM) {
        spin_lock(&mgr->lock);
        task_mm_get(old);
        spin_unlock(&mgr->lock);
        return old;
    }

    task_mm_info_t *new_mm = (task_mm_info_t *)malloc(sizeof(task_mm_info_t));
    if (!new_mm)
        return NULL;

    memset(new_mm, 0, sizeof(task_mm_info_t));
    spin_init(&new_mm->lock);

    spin_lock(&mgr->lock);
    spin_lock(&old->lock);

    uint64_t *old_root = phys_to_virt(old->page_table_addr);
    uint64_t levels = arch_page_table_levels();
    if (!page_table_levels_valid(levels)) {
        free(new_mm);
        spin_unlock(&old->lock);
        spin_unlock(&mgr->lock);
        return NULL;
    }

    bool source_changed = false;
    uint64_t *new_root =
        copy_page_table_recursive(old_root, levels, 0, mgr, &source_changed);
    if (!new_root) {
        free(new_mm);
        spin_unlock(&old->lock);
        spin_unlock(&mgr->lock);
        return NULL;
    }

    arch_page_table_copy_kernel(new_root, old_root);

    new_mm->page_table_addr = (uint64_t)virt_to_phys(new_root);
    new_mm->ref_count = 1;

    if (vma_manager_copy(&new_mm->task_vma_mgr, mgr) != 0) {
        free_page_table_recursive(new_root, levels);
        free(new_mm);
        spin_unlock(&old->lock);
        spin_unlock(&mgr->lock);
        return NULL;
    }

    page_table_account_shared_file_mappings(new_mm, true);

    spin_unlock(&old->lock);
    spin_unlock(&mgr->lock);

    if (source_changed)
        task_mm_flush_tlb_all(old);

    new_mm->task_vma_mgr.initialized = mgr->initialized;
    new_mm->mmap_top = old->mmap_top;
    new_mm->signal_trampoline_start = old->signal_trampoline_start;
    new_mm->pie_base = old->pie_base;
    new_mm->interpreter_base = old->interpreter_base;
    new_mm->brk_start = old->brk_start;
    new_mm->brk_current = old->brk_current;
    new_mm->brk_end = old->brk_end;
    new_mm->stack_start = old->stack_start;
    new_mm->stack_end = old->stack_end;
    new_mm->resident_pages = task_mm_resident_pages(old);

    return new_mm;
}

void free_page_table(task_mm_info_t *directory) {
    if (!directory)
        return;

    vma_manager_t *mgr = &directory->task_vma_mgr;
    int old_ref = __atomic_load_n(&directory->ref_count, __ATOMIC_ACQUIRE);
    while (true) {
        if (old_ref <= 0)
            return;
        int new_ref = old_ref - 1;
        if (__atomic_compare_exchange_n(&directory->ref_count, &old_ref,
                                        new_ref, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (new_ref != 0)
                return;
            break;
        }
    }

    /* ref_count reaching zero gives this path exclusive ownership of the mm.
     * Do not hold the VMA spinlock while dropping inode and mapping references:
     * filesystem eviction and page-cache teardown are potentially lengthy and
     * must remain preemptible. */
    page_table_account_shared_file_mappings(directory, false);
    vma_manager_exit_cleanup(mgr);

    uint64_t levels = arch_page_table_levels();
    if (page_table_levels_valid(levels)) {
        free_page_table_recursive(
            (uint64_t *)phys_to_virt(directory->page_table_addr), levels);
    }

    free(directory);
}

void page_table_init() {
    arch_page_table_init();
    kernel_page_dir = get_current_page_dir(false);
}

void unmap_release_batch_commit(unmap_release_batch_t *batch) {
    if (!batch)
        return;

    unmap_release_deferred_drain();

    bool can_release = true;
    bool has_entries = batch->page_count || batch->table_count;
    if (has_entries) {
        if (batch->mm) {
            can_release = task_mm_flush_tlb_all(batch->mm);
        } else {
            arch_flush_tlb_all();
        }
    }

    if (!can_release) {
        unmap_defer_release(batch);
        goto out;
    }

    unmap_release_addrs(batch->page_addrs, batch->page_count,
                        batch->table_addrs, batch->table_count);

out:
    batch->page_count = 0;
    batch->table_count = 0;
    batch->mm = NULL;
    batch->flush_start = 0;
    batch->flush_end = 0;
}
