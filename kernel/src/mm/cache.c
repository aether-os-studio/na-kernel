#include <mm/cache.h>
#include <mm/buddy.h>
#include <mm/page.h>
#include <task/task.h>
#include <arch/arch.h>

#define PAGE_CACHE_MIN_READAHEAD 2ULL
#define PAGE_CACHE_MAX_READAHEAD 32ULL
#define PAGE_CACHE_UNMAP_LOCK_BATCH_MAX 64ULL
#define PAGE_CACHE_DIRTY_MAPPING_LIMIT 256ULL
#define PAGE_CACHE_DIRTY_GLOBAL_MIN 512ULL
#define PAGE_CACHE_WRITEBACK_BATCH 8ULL
#define PAGE_CACHE_RECLAIM_SCAN_BATCH 64ULL

static DEFINE_LLIST(pcache_lru);
static spinlock_t pcache_lru_lock = SPIN_INIT;
static volatile int pcache_reclaim_active;

static uint64_t pcache_cached_pages;
static uint64_t pcache_dirty_pages;
static uint64_t pcache_writeback_pages;
static uint64_t pcache_mapped_pages;
static uint64_t pcache_lru_pages;
static uint64_t pcache_reclaimed_pages;
static uint64_t pcache_reclaim_scanned_pages;
static uint64_t pcache_dirty_global_limit;
static uint8_t pcache_writeback_active;

static void pcache_stat_add(uint64_t *counter, uint64_t delta) {
    if (delta)
        __atomic_add_fetch(counter, delta, __ATOMIC_ACQ_REL);
}

static void pcache_stat_sub(uint64_t *counter, uint64_t delta) {
    uint64_t old;
    uint64_t new;

    if (!delta)
        return;

    do {
        old = __atomic_load_n(counter, __ATOMIC_ACQUIRE);
        if (!old)
            return;
        new = old > delta ? old - delta : 0;
    } while (!__atomic_compare_exchange_n(counter, &old, new, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

void page_cache_stats_snapshot(page_cache_stats_t *stats) {
    if (!stats)
        return;

    stats->cached_pages =
        __atomic_load_n(&pcache_cached_pages, __ATOMIC_ACQUIRE);
    stats->dirty_pages = __atomic_load_n(&pcache_dirty_pages, __ATOMIC_ACQUIRE);
    stats->writeback_pages =
        __atomic_load_n(&pcache_writeback_pages, __ATOMIC_ACQUIRE);
    stats->mapped_pages =
        __atomic_load_n(&pcache_mapped_pages, __ATOMIC_ACQUIRE);
    stats->lru_pages = __atomic_load_n(&pcache_lru_pages, __ATOMIC_ACQUIRE);
    stats->reclaimed_pages =
        __atomic_load_n(&pcache_reclaimed_pages, __ATOMIC_ACQUIRE);
    stats->reclaim_scanned_pages =
        __atomic_load_n(&pcache_reclaim_scanned_pages, __ATOMIC_ACQUIRE);
}

static bool pcache_user_buffer(struct vfs_file *file, const void *buf,
                               size_t len) {
    if (!buf || (file && (file->f_mode & VFS_FMODE_KERNEL_IO)))
        return false;
    return !check_user_overflow((uint64_t)buf, len);
}

static void pcache_remove_locked_lru(page_cache_page_t *page, bool remove_lru);

static void pcache_wake_waiters(page_cache_page_t *page) {
    if (page)
        wait_queue_wake_all(&page->wait, 0, EOK);
}

static void pcache_lru_add_tail_locked(page_cache_page_t *page) {
    if (!page || page->on_lru || page->reclaiming)
        return;

    llist_append(&pcache_lru, &page->lru);
    page->on_lru = true;
    pcache_stat_add(&pcache_lru_pages, 1);
}

static void pcache_lru_add_tail(page_cache_page_t *page) {
    spin_lock(&pcache_lru_lock);
    pcache_lru_add_tail_locked(page);
    spin_unlock(&pcache_lru_lock);
}

static void pcache_lru_remove_locked(page_cache_page_t *page) {
    if (!page || !page->on_lru)
        return;

    llist_delete(&page->lru);
    page->on_lru = false;
    pcache_stat_sub(&pcache_lru_pages, 1);
}

static void pcache_lru_remove(page_cache_page_t *page) {
    spin_lock(&pcache_lru_lock);
    pcache_lru_remove_locked(page);
    spin_unlock(&pcache_lru_lock);
}

static void pcache_lru_touch(page_cache_page_t *page) {
    if (!page)
        return;

    /* Cache hits are much hotter than reclaim. Defer LRU list mutation to
     * the reclaimer so readers on different CPUs do not bounce one lock. */
    __atomic_store_n(&page->referenced, true, __ATOMIC_RELAXED);
}

static void pcache_lru_move_tail_locked(page_cache_page_t *page) {
    if (!page || !page->on_lru || page->reclaiming)
        return;

    llist_delete(&page->lru);
    llist_append(&pcache_lru, &page->lru);
}

static page_cache_page_t *
pcache_lookup_locked(struct vfs_address_space *mapping, uint64_t index) {
    rb_node_t *node = mapping ? mapping->pages.rb_node : NULL;

    while (node) {
        page_cache_page_t *page = rb_entry(node, page_cache_page_t, node);
        if (index < page->index)
            node = node->rb_left;
        else if (index > page->index)
            node = node->rb_right;
        else
            return page;
    }

    return NULL;
}

static rb_node_t *pcache_lower_bound_locked(struct vfs_address_space *mapping,
                                            uint64_t index) {
    rb_node_t *node = mapping ? mapping->pages.rb_node : NULL;
    rb_node_t *best = NULL;

    while (node) {
        page_cache_page_t *page = rb_entry(node, page_cache_page_t, node);
        if (index <= page->index) {
            best = node;
            node = node->rb_left;
        } else {
            node = node->rb_right;
        }
    }

    return best;
}

static int pcache_insert_locked(struct vfs_address_space *mapping,
                                page_cache_page_t *page) {
    rb_node_t **link;
    rb_node_t *parent = NULL;

    if (!mapping || !page)
        return -EINVAL;

    link = &mapping->pages.rb_node;
    while (*link) {
        parent = *link;
        page_cache_page_t *cur = rb_entry(parent, page_cache_page_t, node);
        if (page->index < cur->index) {
            link = &(*link)->rb_left;
        } else if (page->index > cur->index) {
            link = &(*link)->rb_right;
        } else {
            return -EEXIST;
        }
    }

    page->node.rb_parent_color = (uint64_t)parent;
    page->node.rb_left = NULL;
    page->node.rb_right = NULL;
    *link = &page->node;
    rb_insert_color(&page->node, &mapping->pages);
    mapping->cached_pages++;
    pcache_stat_add(&pcache_cached_pages, 1);
    return 0;
}

static void pcache_remove_locked(page_cache_page_t *page) {
    pcache_remove_locked_lru(page, true);
}

static void pcache_remove_locked_lru(page_cache_page_t *page, bool remove_lru) {
    struct vfs_address_space *mapping;

    if (!page || !page->mapping)
        return;

    mapping = page->mapping;
    if (remove_lru)
        pcache_lru_remove(page);
    rb_erase(&page->node, &mapping->pages);
    page->node.rb_parent_color = 0;
    page->node.rb_left = NULL;
    page->node.rb_right = NULL;
    page->mapping = NULL;
    page->truncated = true;
    page->loading = false;
    if (mapping->cached_pages)
        mapping->cached_pages--;
    pcache_stat_sub(&pcache_cached_pages, 1);
    if (page->dirty) {
        if (mapping->dirty_pages)
            mapping->dirty_pages--;
        pcache_stat_sub(&pcache_dirty_pages, 1);
        page->dirty = false;
    }
    if (page->writeback) {
        pcache_stat_sub(&pcache_writeback_pages, 1);
        page->writeback = false;
    }
    if (page->mmap_count > 0) {
        uint64_t mapped = (uint64_t)page->mmap_count;
        if (mapping->mmap_pages >= mapped)
            mapping->mmap_pages -= mapped;
        else
            mapping->mmap_pages = 0;
        pcache_stat_sub(&pcache_mapped_pages, 1);
        page->mmap_count = 0;
    }
    pcache_wake_waiters(page);
}

static void pcache_free(page_cache_page_t *page) {
    if (!page)
        return;
    pcache_lru_remove(page);
    if (page->paddr)
        address_release(page->paddr);
    free(page);
}

static void pcache_drop_ref(page_cache_page_t *page) {
    bool free_page = false;

    if (!page)
        return;

    struct vfs_address_space *mapping = page->mapping;
    if (mapping)
        spin_lock(&mapping->lock);
    if (page->ref_count > 0)
        page->ref_count--;
    if (page->ref_count == 0 && !page->mapping && !page->reclaiming)
        free_page = true;
    if (mapping)
        spin_unlock(&mapping->lock);

    if (free_page)
        pcache_free(page);
}

static bool pcache_take_ref(page_cache_page_t *page) {
    if (!page || page->ref_count < 0)
        return false;
    page->ref_count++;
    return true;
}

static void pcache_wait_unlocked(page_cache_page_t *page) {
    if (!page || !page->mapping)
        return;

    struct vfs_address_space *mapping = page->mapping;
    while (true) {
        if (!current_task || current_task->preempt_count) {
            spin_lock(&mapping->lock);
            bool busy =
                page->mapping == mapping && (page->loading || page->writeback);
            spin_unlock(&mapping->lock);
            if (!busy)
                return;
            arch_pause();
            continue;
        }

        wait_queue_entry_t wait;
        task_prepare_block(current_task);
        wait_queue_entry_init(&wait, current_task, 0, NULL, NULL);
        wait_queue_add(&page->wait, &wait);

        spin_lock(&mapping->lock);
        bool busy =
            page->mapping == mapping && (page->loading || page->writeback);
        spin_unlock(&mapping->lock);

        if (!busy) {
            wait_queue_remove(&page->wait, &wait);
            task_cancel_block_prepare(current_task);
            return;
        }

        (void)task_block(current_task, TASK_UNINTERRUPTABLE, -1,
                         "page_cache_busy");
        wait_queue_remove(&page->wait, &wait);
        task_cancel_block_prepare(current_task);
    }
}

static int pcache_begin_update(page_cache_page_t *page) {
    if (!page || !page->mapping)
        return -EIO;

    struct vfs_address_space *mapping = page->mapping;
    while (true) {
        spin_lock(&mapping->lock);
        if (page->mapping != mapping) {
            spin_unlock(&mapping->lock);
            return -EIO;
        }
        if (!page->loading && !page->writeback) {
            page->loading = true;
            spin_unlock(&mapping->lock);
            return 0;
        }
        spin_unlock(&mapping->lock);
        pcache_wait_unlocked(page);
    }
}

static void pcache_finish_update(page_cache_page_t *page, bool uptodate,
                                 bool dirty) {
    if (!page || !page->mapping)
        return;

    struct vfs_address_space *mapping = page->mapping;
    bool finished = false;
    spin_lock(&mapping->lock);
    if (page->mapping == mapping) {
        if (uptodate)
            page->uptodate = true;
        if (dirty && !page->dirty) {
            page->dirty = true;
            mapping->dirty_pages++;
            pcache_stat_add(&pcache_dirty_pages, 1);
        }
        page->loading = false;
        finished = true;
    }
    spin_unlock(&mapping->lock);
    if (finished)
        pcache_wake_waiters(page);
}

void page_cache_zap_inode_shared_mappings(vfs_node_t *node,
                                          struct vfs_address_space *mapping,
                                          uint64_t file_start,
                                          uint64_t file_end);

static int pcache_load_page(struct vfs_file *file,
                            struct vfs_address_space *mapping,
                            page_cache_page_t *page) {
    int ret;

    if (!page || page->uptodate)
        return 0;
    if (!mapping || !mapping->a_ops || !mapping->a_ops->readpage)
        return -EIO;

    spin_lock(&mapping->lock);
    if (page->mapping != mapping) {
        spin_unlock(&mapping->lock);
        return -EIO;
    }
    if (page->uptodate) {
        spin_unlock(&mapping->lock);
        return 0;
    }
    if (page->loading) {
        spin_unlock(&mapping->lock);
        pcache_wait_unlocked(page);
        return page->uptodate ? 0 : -EIO;
    }
    page->loading = true;
    spin_unlock(&mapping->lock);

    ret = mapping->a_ops->readpage(file, mapping, page->index,
                                   (void *)phys_to_virt(page->paddr));
    bool valid = false;
    spin_lock(&mapping->lock);
    if (page->mapping == mapping) {
        if (ret >= 0)
            page->uptodate = true;
        page->loading = false;
        valid = true;
    }
    spin_unlock(&mapping->lock);
    if (valid)
        pcache_wake_waiters(page);
    if (ret < 0)
        return ret;
    return valid ? 0 : -EIO;
}

static int pcache_write_page(struct vfs_address_space *mapping,
                             page_cache_page_t *page) {
    int ret;
    bool accounted_writeback = false;

    if (!page)
        return 0;
    if (!mapping || !mapping->a_ops || !mapping->a_ops->writepage)
        return -EIO;

    while (true) {
        spin_lock(&mapping->lock);
        if (page->mapping != mapping || !page->dirty) {
            spin_unlock(&mapping->lock);
            return 0;
        }
        if (!page->loading && !page->writeback)
            break;
        spin_unlock(&mapping->lock);
        pcache_wait_unlocked(page);
    }
    if (!page->writeback) {
        page->writeback = true;
        accounted_writeback = true;
        pcache_stat_add(&pcache_writeback_pages, 1);
    }
    spin_unlock(&mapping->lock);

    ret = mapping->a_ops->writepage(NULL, mapping, page->index,
                                    (const void *)phys_to_virt(page->paddr));

    bool finished_writeback = false;
    spin_lock(&mapping->lock);
    if (accounted_writeback && page->writeback) {
        page->writeback = false;
        finished_writeback = true;
        pcache_stat_sub(&pcache_writeback_pages, 1);
    }
    if (ret == 0 && page->mapping == mapping && page->dirty) {
        page->dirty = false;
        if (mapping->dirty_pages)
            mapping->dirty_pages--;
        pcache_stat_sub(&pcache_dirty_pages, 1);
    }
    spin_unlock(&mapping->lock);
    if (finished_writeback)
        pcache_wake_waiters(page);
    return ret < 0 ? ret : 0;
}

static int pcache_copy_to(void *dst, const void *src, size_t len,
                          bool user_dst) {
    if (!len)
        return 0;
    if (user_dst)
        return copy_to_user(dst, src, len) ? -EFAULT : 0;
    memcpy(dst, src, len);
    return 0;
}

static int pcache_copy_from(void *dst, const void *src, size_t len,
                            bool user_src) {
    if (!len)
        return 0;
    if (user_src)
        return copy_from_user(dst, src, len) ? -EFAULT : 0;
    memcpy(dst, src, len);
    return 0;
}

void page_cache_mapping_init(struct vfs_address_space *mapping,
                             struct vfs_inode *host) {
    if (!mapping)
        return;
    mapping->host = host;
    spin_init(&mapping->lock);
    mapping->pages = RB_ROOT_INIT;
    mapping->cached_pages = 0;
    mapping->dirty_pages = 0;
    mapping->mmap_pages = 0;
    mapping->readahead_window = PAGE_CACHE_MIN_READAHEAD;
    mapping->readahead_last_index = UINT64_MAX;
}

int page_cache_get_page(struct vfs_file *file,
                        struct vfs_address_space *mapping, uint64_t index,
                        bool create, bool read, page_cache_page_t **out_page) {
    page_cache_page_t *page;
    page_cache_page_t *new_page = NULL;
    int ret = 0;

    if (!mapping || !out_page)
        return -EINVAL;
    *out_page = NULL;

retry:
    spin_lock(&mapping->lock);
    page = pcache_lookup_locked(mapping, index);
    if (page) {
        pcache_take_ref(page);
        spin_unlock(&mapping->lock);
        pcache_lru_touch(page);
        if (new_page)
            pcache_free(new_page);
        if (read && !page->uptodate) {
            ret = pcache_load_page(file, mapping, page);
            if (ret < 0) {
                page_cache_page_put(page);
                return ret;
            }
        }
        *out_page = page;
        return 0;
    }

    if (!create) {
        spin_unlock(&mapping->lock);
        return 0;
    }
    spin_unlock(&mapping->lock);

    if (!new_page) {
        new_page = calloc(1, sizeof(*new_page));
        if (!new_page) {
            (void)page_cache_reclaim(32);
            new_page = calloc(1, sizeof(*new_page));
            if (!new_page)
                return -ENOMEM;
        }

        llist_init_head(&new_page->lru);
        wait_queue_init(&new_page->wait);
        new_page->paddr = alloc_frames(1);
        if (!new_page->paddr) {
            pcache_free(new_page);
            return -ENOMEM;
        }
        memset((void *)phys_to_virt(new_page->paddr), 0, PAGE_SIZE);
        new_page->index = index;
        new_page->ref_count = 1;
    }

    spin_lock(&mapping->lock);
    page = pcache_lookup_locked(mapping, index);
    if (page) {
        pcache_take_ref(page);
        spin_unlock(&mapping->lock);
        pcache_lru_touch(page);
        pcache_free(new_page);
        new_page = NULL;
        if (read && !page->uptodate) {
            ret = pcache_load_page(file, mapping, page);
            if (ret < 0) {
                page_cache_page_put(page);
                return ret;
            }
        }
        *out_page = page;
        return 0;
    }

    new_page->mapping = mapping;
    ret = pcache_insert_locked(mapping, new_page);
    if (ret < 0) {
        spin_unlock(&mapping->lock);
        new_page->mapping = NULL;
        if (ret == -EEXIST)
            goto retry;
        pcache_free(new_page);
        return ret;
    }
    spin_unlock(&mapping->lock);

    page = new_page;
    new_page = NULL;
    pcache_lru_add_tail(page);

    if (read) {
        ret = pcache_load_page(file, mapping, page);
        if (ret < 0) {
            page_cache_page_put(page);
            return ret;
        }
    }

    *out_page = page;
    return 0;
}

void page_cache_page_put(page_cache_page_t *page) { pcache_drop_ref(page); }

static bool pcache_lru_reclaim_oldest(uint64_t *scanned_out,
                                      uint64_t scan_limit) {
    page_cache_page_t *reclaimed_page = NULL;
    bool reclaimed = false;

    if (scanned_out)
        *scanned_out = 0;

    spin_lock(&pcache_lru_lock);
    struct llist_header *pos = pcache_lru.next;
    while (pos != &pcache_lru && (!scanned_out || *scanned_out < scan_limit)) {
        page_cache_page_t *candidate = list_entry(pos, page_cache_page_t, lru);
        struct vfs_address_space *mapping = candidate->mapping;
        pos = pos->next;
        if (candidate->reclaiming || !mapping)
            continue;
        if (scanned_out)
            (*scanned_out)++;
        if (__atomic_exchange_n(&candidate->referenced, false,
                                __ATOMIC_RELAXED)) {
            pcache_lru_move_tail_locked(candidate);
            continue;
        }
        if (!spin_trylock(&mapping->lock)) {
            pcache_lru_move_tail_locked(candidate);
            continue;
        }
        if (candidate->mapping != mapping) {
            spin_unlock(&mapping->lock);
            continue;
        }

        int phys_refs = page_refcount_read(get_page_by_addr(candidate->paddr));
        if (candidate->mmap_count == 0 && candidate->ref_count == 0 &&
            phys_refs == 1 && !candidate->loading && !candidate->writeback &&
            !candidate->dirty) {
            pcache_lru_remove_locked(candidate);
            candidate->reclaiming = true;
            pcache_remove_locked_lru(candidate, false);
            reclaimed_page = candidate;
            reclaimed = true;
            spin_unlock(&mapping->lock);
            break;
        }

        pcache_lru_move_tail_locked(candidate);
        spin_unlock(&mapping->lock);
    }
    spin_unlock(&pcache_lru_lock);

    if (reclaimed_page) {
        reclaimed_page->reclaiming = false;
        pcache_free(reclaimed_page);
        pcache_stat_add(&pcache_reclaimed_pages, 1);
    }
    return reclaimed;
}

uint64_t page_cache_reclaim(uint64_t target_pages) {
    uint64_t lru_pages;
    uint64_t scan_limit;
    uint64_t scanned = 0;
    uint64_t reclaimed = 0;

    if (!target_pages)
        return 0;
    if (__atomic_exchange_n(&pcache_reclaim_active, 1, __ATOMIC_ACQ_REL))
        return 0;

    lru_pages = __atomic_load_n(&pcache_lru_pages, __ATOMIC_ACQUIRE);
    target_pages = MIN(target_pages, lru_pages);
    scan_limit = target_pages > UINT64_MAX / 8 ? UINT64_MAX : target_pages * 8;
    scan_limit = MIN(lru_pages, scan_limit);

    while (scanned < scan_limit && reclaimed < target_pages) {
        uint64_t pass_scanned = 0;
        uint64_t pass_limit =
            MIN(scan_limit - scanned, PAGE_CACHE_RECLAIM_SCAN_BATCH);
        bool did_reclaim = pcache_lru_reclaim_oldest(&pass_scanned, pass_limit);
        if (!pass_scanned)
            break;
        scanned += pass_scanned;
        if (did_reclaim)
            reclaimed++;
    }

    pcache_stat_add(&pcache_reclaim_scanned_pages, scanned);
    __atomic_store_n(&pcache_reclaim_active, 0, __ATOMIC_RELEASE);
    return reclaimed;
}

uint64_t page_cache_page_paddr(page_cache_page_t *page) {
    return page ? page->paddr : 0;
}

void *page_cache_page_data(page_cache_page_t *page) {
    return page && page->paddr ? (void *)phys_to_virt(page->paddr) : NULL;
}

void page_cache_mark_dirty(page_cache_page_t *page) {
    if (!page || !page->mapping)
        return;

    struct vfs_address_space *mapping = page->mapping;
    spin_lock(&mapping->lock);
    if (!page->dirty) {
        page->dirty = true;
        mapping->dirty_pages++;
        pcache_stat_add(&pcache_dirty_pages, 1);
    }
    spin_unlock(&mapping->lock);
}

static void pcache_update_readahead(struct vfs_address_space *mapping,
                                    uint64_t index, size_t read_bytes) {
    if (!mapping || read_bytes == 0)
        return;

    spin_lock(&mapping->lock);
    if (mapping->readahead_last_index != UINT64_MAX &&
        index == mapping->readahead_last_index + 1) {
        if (mapping->readahead_window < PAGE_CACHE_MAX_READAHEAD)
            mapping->readahead_window *= 2;
        if (mapping->readahead_window > PAGE_CACHE_MAX_READAHEAD)
            mapping->readahead_window = PAGE_CACHE_MAX_READAHEAD;
    } else {
        mapping->readahead_window = PAGE_CACHE_MIN_READAHEAD;
    }
    mapping->readahead_last_index = index;
    spin_unlock(&mapping->lock);
}

int page_cache_readahead(struct vfs_file *file, uint64_t offset,
                         uint64_t count) {
    struct vfs_inode *inode;
    struct vfs_address_space *mapping;
    uint64_t start;
    uint64_t pages;

    if (!file || !file->f_inode || count == 0)
        return 0;
    inode = file->f_inode;
    mapping = &inode->i_mapping;
    if (!mapping->a_ops || !mapping->a_ops->readpage)
        return 0;
    if (offset >= inode->i_size)
        return 0;

    start = offset / PAGE_SIZE;
    pages =
        PADDING_UP(MIN(count, inode->i_size - offset), PAGE_SIZE) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        page_cache_page_t *page = NULL;
        int ret =
            page_cache_get_page(file, mapping, start + i, true, true, &page);
        if (ret < 0)
            return ret == -EAGAIN ? 0 : ret;
        page_cache_page_put(page);
    }

    return 0;
}

int page_cache_read(struct vfs_file *file, void *buf, size_t count,
                    loff_t *ppos) {
    struct vfs_inode *inode;
    struct vfs_address_space *mapping;
    uint64_t file_size;
    uint64_t pos;
    size_t copy_len;
    size_t done = 0;
    bool user_dst;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (*ppos < 0)
        return -EINVAL;
    if (count == 0)
        return 0;

    inode = file->f_inode;
    mapping = &inode->i_mapping;
    file_size = inode->i_size;
    pos = (uint64_t)*ppos;
    if (pos >= file_size)
        return 0;

    copy_len = (size_t)MIN((uint64_t)count, file_size - pos);
    user_dst = pcache_user_buffer(file, buf, copy_len);

    while (done < copy_len) {
        uint64_t off = pos + done;
        uint64_t index = off / PAGE_SIZE;
        size_t in_page = (size_t)(off & (PAGE_SIZE - 1));
        size_t chunk = MIN(copy_len - done, PAGE_SIZE - in_page);
        page_cache_page_t *page = NULL;
        int ret = page_cache_get_page(file, mapping, index, true, true, &page);
        if (ret < 0)
            return done ? (int)done : ret;

        ret = pcache_copy_to((uint8_t *)buf + done,
                             (uint8_t *)page_cache_page_data(page) + in_page,
                             chunk, user_dst);
        page_cache_page_put(page);
        if (ret < 0)
            return done ? (int)done : ret;
        done += chunk;
        pcache_update_readahead(mapping, index, chunk);
    }

    *ppos += (loff_t)done;

    uint64_t ra_window;
    spin_lock(&mapping->lock);
    ra_window = mapping->readahead_window;
    spin_unlock(&mapping->lock);
    if (ra_window)
        (void)page_cache_readahead(file, (uint64_t)*ppos,
                                   ra_window * PAGE_SIZE);
    return (int)done;
}

static int pcache_writeback_range_limit(struct vfs_address_space *mapping,
                                        uint64_t start, uint64_t end,
                                        uint64_t max_pages);

static bool pcache_dirty_limit_exceeded(struct vfs_address_space *mapping) {
    uint64_t mapping_dirty = 0;
    uint64_t global_dirty =
        __atomic_load_n(&pcache_dirty_pages, __ATOMIC_ACQUIRE);
    uint64_t global_limit =
        __atomic_load_n(&pcache_dirty_global_limit, __ATOMIC_ACQUIRE);

    if (!global_limit) {
        uint64_t total_pages = 0;
        for (int i = 0; i < __MAX_NR_ZONES; i++) {
            if (zones[i])
                total_pages += zones[i]->managed_pages;
        }
        global_limit = MAX(PAGE_CACHE_DIRTY_GLOBAL_MIN, total_pages / 8);
        __atomic_store_n(&pcache_dirty_global_limit, global_limit,
                         __ATOMIC_RELEASE);
    }

    spin_lock(&mapping->lock);
    mapping_dirty = mapping->dirty_pages;
    spin_unlock(&mapping->lock);

    return mapping_dirty > PAGE_CACHE_DIRTY_MAPPING_LIMIT ||
           global_dirty > global_limit;
}

int page_cache_write(struct vfs_file *file, const void *buf, size_t count,
                     loff_t *ppos) {
    struct vfs_inode *inode;
    struct vfs_address_space *mapping;
    uint64_t pos;
    size_t done = 0;
    bool user_src;

    if (!file || !file->f_inode || !buf || !ppos)
        return -EINVAL;
    if (*ppos < 0)
        return -EINVAL;
    if (count == 0)
        return 0;

    inode = file->f_inode;
    mapping = &inode->i_mapping;
    pos = (uint64_t)*ppos;
    user_src = pcache_user_buffer(file, buf, count);

    while (done < count) {
        uint64_t off = pos + done;
        uint64_t index = off / PAGE_SIZE;
        size_t in_page = (size_t)(off & (PAGE_SIZE - 1));
        size_t chunk = MIN(count - done, PAGE_SIZE - in_page);
        bool need_read = in_page != 0 || chunk != PAGE_SIZE;
        page_cache_page_t *page = NULL;
        int ret =
            page_cache_get_page(file, mapping, index, true, need_read, &page);
        if (ret < 0)
            return done ? (int)done : ret;

        ret = pcache_begin_update(page);
        if (ret < 0) {
            page_cache_page_put(page);
            return done ? (int)done : ret;
        }
        ret = pcache_copy_from((uint8_t *)page_cache_page_data(page) + in_page,
                               (const uint8_t *)buf + done, chunk, user_src);
        if (ret < 0) {
            pcache_finish_update(page, false, false);
            page_cache_page_put(page);
            return done ? (int)done : ret;
        }
        pcache_finish_update(page, true, true);
        page_cache_page_put(page);
        done += chunk;

        uint64_t written_end = pos + done;
        spin_lock(&inode->i_lock);
        if (written_end > inode->i_size)
            inode->i_size = written_end;
        spin_unlock(&inode->i_lock);

        if (pcache_dirty_limit_exceeded(mapping) &&
            !__atomic_exchange_n(&pcache_writeback_active, 1,
                                 __ATOMIC_ACQ_REL)) {
            (void)pcache_writeback_range_limit(mapping, 0, UINT64_MAX,
                                               PAGE_CACHE_WRITEBACK_BATCH);
            __atomic_store_n(&pcache_writeback_active, 0, __ATOMIC_RELEASE);
        }
    }

    *ppos += (loff_t)done;
    return (int)done;
}

static int pcache_writeback_range_limit(struct vfs_address_space *mapping,
                                        uint64_t start, uint64_t end,
                                        uint64_t max_pages) {
    rb_node_t *node;
    uint64_t written = 0;
    int ret = 0;

    if (!mapping || start >= end)
        return 0;

    spin_lock(&mapping->lock);
    node = pcache_lower_bound_locked(mapping, start / PAGE_SIZE);
    while (node) {
        page_cache_page_t *page = rb_entry(node, page_cache_page_t, node);
        node = rb_next(node);
        uint64_t page_start = page->index * PAGE_SIZE;
        uint64_t page_end = page_start + PAGE_SIZE;

        if (page_start >= end)
            break;
        if (page_end <= start || page_start >= end || !page->dirty)
            continue;

        pcache_take_ref(page);
        uint64_t next_index = page->index + 1;
        spin_unlock(&mapping->lock);
        ret = pcache_write_page(mapping, page);
        page_cache_page_put(page);
        if (ret < 0)
            return ret;
        written++;
        if (written >= max_pages)
            return 0;
        spin_lock(&mapping->lock);
        node = pcache_lower_bound_locked(mapping, next_index);
    }
    spin_unlock(&mapping->lock);
    return 0;
}

int page_cache_writeback_range(struct vfs_address_space *mapping,
                               uint64_t start, uint64_t end, bool datasync) {
    (void)datasync;
    return pcache_writeback_range_limit(mapping, start, end, UINT64_MAX);
}

int page_cache_invalidate_range(struct vfs_address_space *mapping,
                                uint64_t start, uint64_t end, bool writeback) {
    rb_node_t *node;
    int ret = 0;

    if (!mapping || start >= end)
        return 0;
    if (writeback) {
        ret = page_cache_writeback_range(mapping, start, end, false);
        if (ret < 0)
            return ret;
    }

    spin_lock(&mapping->lock);
    node = pcache_lower_bound_locked(mapping, start / PAGE_SIZE);
    while (node) {
        page_cache_page_t *page = rb_entry(node, page_cache_page_t, node);
        node = rb_next(node);
        uint64_t page_start = page->index * PAGE_SIZE;
        uint64_t page_end = page_start + PAGE_SIZE;

        if (page_start >= end)
            break;
        if (page_end <= start || page_start >= end)
            continue;
        if (page->mmap_count > 0)
            continue;
        pcache_remove_locked(page);
        if (page->ref_count == 0 && !page->reclaiming) {
            uint64_t next_index = page->index + 1;
            spin_unlock(&mapping->lock);
            pcache_free(page);
            spin_lock(&mapping->lock);
            node = pcache_lower_bound_locked(mapping, next_index);
        }
    }
    spin_unlock(&mapping->lock);
    return 0;
}

void page_cache_truncate(struct vfs_address_space *mapping, uint64_t new_size) {
    if (!mapping)
        return;

    uint64_t tail = new_size & (PAGE_SIZE - 1);
    if (tail) {
        page_cache_page_t *page = NULL;
        uint64_t index = new_size / PAGE_SIZE;
        spin_lock(&mapping->lock);
        page = pcache_lookup_locked(mapping, index);
        if (page && page->uptodate)
            memset((uint8_t *)phys_to_virt(page->paddr) + tail, 0,
                   PAGE_SIZE - tail);
        spin_unlock(&mapping->lock);
    }

    uint64_t drop_start = PADDING_UP(new_size, PAGE_SIZE);
    page_cache_zap_inode_shared_mappings(mapping->host, mapping, drop_start,
                                         UINT64_MAX);
    (void)page_cache_invalidate_range(mapping, drop_start, UINT64_MAX, false);
}

void page_cache_evict(struct vfs_address_space *mapping) {
    if (!mapping)
        return;
    (void)page_cache_invalidate_range(mapping, 0, UINT64_MAX, true);
}

void page_cache_mmap_inc_page(struct vfs_address_space *mapping,
                              uint64_t index) {
    if (!mapping)
        return;

    spin_lock(&mapping->lock);
    page_cache_page_t *page = pcache_lookup_locked(mapping, index);
    if (page) {
        if (page->mmap_count == 0)
            pcache_stat_add(&pcache_mapped_pages, 1);
        page->mmap_count++;
        mapping->mmap_pages++;
    }
    spin_unlock(&mapping->lock);
}

void page_cache_mmap_dec_page(struct vfs_address_space *mapping,
                              uint64_t index) {
    if (!mapping)
        return;

    spin_lock(&mapping->lock);
    page_cache_page_t *page = pcache_lookup_locked(mapping, index);
    if (page && page->mmap_count > 0) {
        page->mmap_count--;
        if (mapping->mmap_pages)
            mapping->mmap_pages--;
        if (page->mmap_count == 0)
            pcache_stat_sub(&pcache_mapped_pages, 1);
    }
    spin_unlock(&mapping->lock);
}

static void pcache_sub_resident_pages(task_mm_info_t *mm, uint64_t pages) {
    if (!mm || !pages)
        return;

    uint64_t old = __atomic_load_n(&mm->resident_pages, __ATOMIC_RELAXED);
    uint64_t new_value = old > pages ? old - pages : 0;
    __atomic_store_n(&mm->resident_pages, new_value, __ATOMIC_RELAXED);
}

static void pcache_mmap_dec_index_batch(struct vfs_address_space *mapping,
                                        uint64_t *indices, size_t count) {
    if (!mapping || !indices)
        return;

    for (size_t i = 0; i < count; i++)
        page_cache_mmap_dec_page(mapping, indices[i]);
}

static bool pcache_vma_is_shared_file(vma_t *vma, vfs_node_t *node) {
    return vma && vma->vm_type == VMA_TYPE_FILE && vma->node == node &&
           (vma->vm_flags & VMA_SHARED) && vma->vm_offset >= 0;
}

static bool pcache_vma_file_overlap(vma_t *vma, vfs_node_t *node,
                                    uint64_t file_start, uint64_t file_end,
                                    uint64_t *unmap_start,
                                    uint64_t *unmap_file_start,
                                    uint64_t *unmap_len) {
    if (!pcache_vma_is_shared_file(vma, node))
        return false;

    uint64_t vma_file_start = (uint64_t)vma->vm_offset;
    uint64_t vma_len = vma->vm_end - vma->vm_start;
    uint64_t vma_file_end = UINT64_MAX;
    if (vma_len <= UINT64_MAX - vma_file_start)
        vma_file_end = vma_file_start + vma_len;

    uint64_t overlap_start = MAX(file_start, vma_file_start);
    uint64_t overlap_end = MIN(file_end, vma_file_end);
    if (overlap_start >= overlap_end)
        return false;

    if (unmap_start)
        *unmap_start = vma->vm_start + (overlap_start - vma_file_start);
    if (unmap_file_start)
        *unmap_file_start = overlap_start;
    if (unmap_len)
        *unmap_len = overlap_end - overlap_start;
    return true;
}

static bool pcache_vma_maps_file_range(vma_t *vma, vfs_node_t *node,
                                       uint64_t vaddr, uint64_t file_start,
                                       uint64_t len) {
    if (!pcache_vma_is_shared_file(vma, node) || len == 0)
        return false;
    if (vaddr < vma->vm_start || vaddr + len > vma->vm_end)
        return false;

    uint64_t expected = (uint64_t)vma->vm_offset + (vaddr - vma->vm_start);
    return expected == file_start;
}

static uint64_t page_cache_unmap_shared_range_checked(
    struct vfs_address_space *mapping, vfs_node_t *node, task_mm_info_t *mm,
    uint64_t vaddr, uint64_t file_start, uint64_t len) {
    if (!node || !mm || len == 0)
        return vaddr;

    uint64_t *pgdir = task_mm_pgdir(mm);
    if (!pgdir)
        return vaddr;

    vma_manager_t *mgr = &mm->task_vma_mgr;
    uint64_t end = vaddr + len;
    uint64_t cursor = vaddr;
    uint64_t unmapped = 0;

    while (cursor < end) {
        uint64_t chunk_end = end;
        uint64_t chunk_limit =
            cursor + PAGE_CACHE_UNMAP_LOCK_BATCH_MAX * PAGE_SIZE;
        if (chunk_limit > cursor && chunk_limit < chunk_end)
            chunk_end = chunk_limit;
        if (chunk_end <= cursor)
            break;

        uint64_t indices[UNMAP_RELEASE_BATCH_MAX];
        size_t index_count = 0;
        unmap_release_batch_t batch = {
            .mm = mm,
            .flush_start = vaddr,
            .flush_end = end,
        };

        spin_lock(&mgr->lock);
        vma_t *vma = vma_find(mgr, cursor);
        uint64_t chunk_file_start = file_start + (cursor - vaddr);
        if (vma && vma->vm_end < chunk_end)
            chunk_end = vma->vm_end;
        uint64_t chunk_len = chunk_end - cursor;
        if (!pcache_vma_maps_file_range(vma, node, cursor, chunk_file_start,
                                        chunk_len)) {
            spin_unlock(&mgr->lock);
            cursor += PAGE_SIZE;
            continue;
        }

        spin_lock(&mm->lock);
        for (uint64_t va = cursor; va < chunk_end; va += PAGE_SIZE) {
            uint64_t off = va - vaddr;
            if (off > UINT64_MAX - file_start)
                break;
            if (!translate_address(pgdir, va))
                continue;
            if (!unmap_page_defer_release(pgdir, va, &batch, false, false))
                continue;
            indices[index_count++] = (file_start + off) / PAGE_SIZE;
            unmapped++;
        }
        spin_unlock(&mm->lock);
        spin_unlock(&mgr->lock);

        unmap_release_batch_commit(&batch);
        pcache_mmap_dec_index_batch(mapping, indices, index_count);
        cursor = chunk_end;
    }

    pcache_sub_resident_pages(mm, unmapped);
    return cursor;
}

void page_cache_unmap_shared_range(struct vfs_address_space *mapping,
                                   task_mm_info_t *mm, uint64_t vaddr,
                                   uint64_t file_start, uint64_t len) {
    uint64_t *pgdir;
    uint64_t unmapped = 0;

    if (!mapping || !mm || len == 0)
        return;
    pgdir = task_mm_pgdir(mm);
    if (!pgdir)
        return;

    uint64_t end = vaddr + len;
    uint64_t cursor = vaddr;
    while (cursor < end) {
        uint64_t indices[UNMAP_RELEASE_BATCH_MAX];
        size_t index_count = 0;
        uint64_t scanned = 0;
        unmap_release_batch_t batch = {
            .mm = mm,
            .flush_start = vaddr,
            .flush_end = end,
        };

        spin_lock(&mm->lock);
        while (cursor < end && scanned < PAGE_CACHE_UNMAP_LOCK_BATCH_MAX &&
               batch.page_count < UNMAP_RELEASE_BATCH_MAX) {
            uint64_t off = cursor - vaddr;
            uint64_t va = cursor;

            cursor += PAGE_SIZE;
            scanned++;

            if (off > UINT64_MAX - file_start)
                break;
            if (!translate_address(pgdir, va))
                continue;
            if (!unmap_page_defer_release(pgdir, va, &batch, false, false))
                continue;
            indices[index_count++] = (file_start + off) / PAGE_SIZE;
            unmapped++;
        }
        spin_unlock(&mm->lock);

        unmap_release_batch_commit(&batch);
        pcache_mmap_dec_index_batch(mapping, indices, index_count);
    }

    pcache_sub_resident_pages(mm, unmapped);
}

void page_cache_zap_inode_shared_mappings(vfs_node_t *node,
                                          struct vfs_address_space *mapping,
                                          uint64_t file_start,
                                          uint64_t file_end) {
    if (!node || file_start >= file_end)
        return;

    size_t capacity;
    spin_lock(&task_queue_lock);
    capacity = hashmap_size(&task_pid_map);
    spin_unlock(&task_queue_lock);
    if (capacity == 0)
        return;

    if (capacity < 16)
        capacity = 16;

    task_mm_info_t **mms = NULL;
    size_t mm_count = 0;
    while (true) {
        bool overflow = false;
        mms = calloc(capacity, sizeof(*mms));
        if (!mms)
            return;

        spin_lock(&task_queue_lock);
        if (task_pid_map.buckets) {
            for (size_t i = 0; i < task_pid_map.bucket_count; i++) {
                hashmap_entry_t *entry = &task_pid_map.buckets[i];
                if (!hashmap_entry_is_occupied(entry))
                    continue;

                task_t *task = (task_t *)entry->value;
                task_mm_info_t *mm = task ? task->mm : NULL;
                if (!mm || !mm->task_vma_mgr.initialized)
                    continue;

                bool seen = false;
                for (size_t j = 0; j < mm_count; j++) {
                    if (mms[j] == mm) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    continue;

                if (mm_count == capacity) {
                    overflow = true;
                    break;
                }

                task_mm_get(mm);
                mms[mm_count++] = mm;
            }
        }
        spin_unlock(&task_queue_lock);

        if (!overflow)
            break;

        for (size_t i = 0; i < mm_count; i++)
            task_mm_put(mms[i]);
        free(mms);
        mms = NULL;
        mm_count = 0;
        if (capacity > SIZE_MAX / 2)
            return;
        capacity *= 2;
    }

    for (size_t i = 0; i < mm_count; i++) {
        task_mm_info_t *mm = mms[i];
        vma_manager_t *mgr = &mm->task_vma_mgr;
        uint64_t mmap_top = task_mm_mmap_top(mm);
        uint64_t cursor = USER_MMAP_START;

        while (cursor < mmap_top) {
            bool found = false;
            bool matched = false;
            uint64_t next = cursor;
            uint64_t unmap_start = 0;
            uint64_t unmap_file_start = 0;
            uint64_t unmap_len = 0;

            spin_lock(&mgr->lock);
            vma_t *vma = vma_find_intersection(mgr, cursor, mmap_top);
            if (vma) {
                found = true;
                next = vma->vm_end;
                matched = pcache_vma_file_overlap(
                    vma, node, file_start, file_end, &unmap_start,
                    &unmap_file_start, &unmap_len);
            }
            spin_unlock(&mgr->lock);

            if (!found)
                break;

            if (matched) {
                uint64_t unmapped_until = page_cache_unmap_shared_range_checked(
                    mapping, node, mm, unmap_start, unmap_file_start,
                    unmap_len);
                if (unmapped_until > cursor) {
                    cursor = unmapped_until;
                    continue;
                }
            }

            if (next <= cursor)
                break;
            cursor = next;
        }

        task_mm_put(mm);
    }

    free(mms);
}

int page_cache_map_fault(struct vfs_file *file, uint64_t vaddr,
                         uint64_t file_off, uint64_t pt_flags) {
    struct vfs_inode *inode;
    page_cache_page_t *page = NULL;
    uint64_t index;
    int ret;

    if (!file || !file->f_inode || !current_task || !current_task->mm)
        return -EINVAL;
    inode = file->f_inode;
    if (file_off >= inode->i_size)
        return -EFAULT;

    index = file_off / PAGE_SIZE;
    ret =
        page_cache_get_page(file, &inode->i_mapping, index, true, true, &page);
    if (ret < 0)
        return ret;

    dcache_flush_range(page_cache_page_data(page), PAGE_SIZE);
    if (pt_flags & PT_FLAG_X)
        sync_instruction_memory_range(page_cache_page_data(page), PAGE_SIZE);

    bool mapped_new = false;
    spin_lock(&current_task->mm->lock);
    if (!translate_address(task_mm_pgdir(current_task->mm), vaddr)) {
        ret = map_page_range_mm(current_task->mm, vaddr, page->paddr, PAGE_SIZE,
                                pt_flags);
        mapped_new = ret == 0;
    } else {
        ret = 0;
    }
    spin_unlock(&current_task->mm->lock);

    if (mapped_new) {
        page_cache_mmap_inc_page(&inode->i_mapping, page->index);
        if (pt_flags & PT_FLAG_W)
            page_cache_mark_dirty(page);
    }

    page_cache_page_put(page);
    return ret < 0 ? ret : 0;
}
