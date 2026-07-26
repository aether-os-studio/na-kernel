#pragma once

#include <fs/vfs/vfs.h>
#include <mm/mm.h>
#include <task/wait.h>

typedef struct page_cache_page {
    rb_node_t node;
    struct llist_header lru;
    wait_queue_head_t wait;
    struct vfs_address_space *mapping;
    uint64_t index;
    uint64_t paddr;
    int ref_count;
    int mmap_count;
    bool uptodate;
    bool loading;
    bool dirty;
    bool writeback;
    bool truncated;
    bool on_lru;
    bool reclaiming;
    bool referenced;
} page_cache_page_t;

typedef struct page_cache_stats {
    uint64_t cached_pages;
    uint64_t dirty_pages;
    uint64_t writeback_pages;
    uint64_t mapped_pages;
    uint64_t lru_pages;
    uint64_t reclaimed_pages;
    uint64_t reclaim_scanned_pages;
} page_cache_stats_t;

void page_cache_mapping_init(struct vfs_address_space *mapping,
                             struct vfs_inode *host);
void page_cache_stats_snapshot(page_cache_stats_t *stats);
uint64_t page_cache_reclaim(uint64_t target_pages);
int page_cache_read(struct vfs_file *file, void *buf, size_t count,
                    loff_t *ppos);
int page_cache_write(struct vfs_file *file, const void *buf, size_t count,
                     loff_t *ppos);
int page_cache_get_page(struct vfs_file *file,
                        struct vfs_address_space *mapping, uint64_t index,
                        bool create, bool read, page_cache_page_t **out_page);
void page_cache_page_put(page_cache_page_t *page);
uint64_t page_cache_page_paddr(page_cache_page_t *page);
void *page_cache_page_data(page_cache_page_t *page);
void page_cache_mark_dirty(page_cache_page_t *page);
int page_cache_writeback_range(struct vfs_address_space *mapping,
                               uint64_t start, uint64_t end, bool datasync);
int page_cache_invalidate_range(struct vfs_address_space *mapping,
                                uint64_t start, uint64_t end, bool writeback);
void page_cache_truncate(struct vfs_address_space *mapping, uint64_t new_size);
void page_cache_evict(struct vfs_address_space *mapping);
int page_cache_readahead(struct vfs_file *file, uint64_t offset,
                         uint64_t count);
void page_cache_mmap_inc_page(struct vfs_address_space *mapping,
                              uint64_t index);
void page_cache_mmap_dec_page(struct vfs_address_space *mapping,
                              uint64_t index);
void page_cache_unmap_shared_range(struct vfs_address_space *mapping,
                                   task_mm_info_t *mm, uint64_t vaddr,
                                   uint64_t file_start, uint64_t len);
void page_cache_zap_inode_shared_mappings(vfs_node_t *node,
                                          struct vfs_address_space *mapping,
                                          uint64_t file_start,
                                          uint64_t file_end);
int page_cache_map_fault(struct vfs_file *file, uint64_t vaddr,
                         uint64_t file_off, uint64_t pt_flags);
