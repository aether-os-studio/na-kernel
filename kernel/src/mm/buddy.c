#include <mm/buddy.h>
#include <mm/bitmap.h>
#include <mm/cache.h>
#include <mm/page.h>
#include <task/task.h>

extern Bitmap usable_regions;
extern void *early_alloc(size_t size);

#define PCP_HIGH 64
#define PCP_BATCH 16

typedef struct per_cpu_pages {
    spinlock_t lock;
    uint64_t head_pfn;
    uint16_t count;
} per_cpu_pages_t;

const char *zone_names[__MAX_NR_ZONES] = {"Normal"};
zone_t *zones[__MAX_NR_ZONES] = {NULL};
int nr_zones;
static per_cpu_pages_t pcp_pages[MAX_CPU_NUM] = {0};

static inline uint64_t order_to_pages(size_t order) { return 1ULL << order; }

static inline uint64_t order_to_bytes(size_t order) {
    return order_to_pages(order) * PAGE_SIZE;
}

static inline bool order_valid(size_t order) { return order < MAX_ORDER; }

static inline uint64_t phys_to_pfn(uintptr_t phys) { return phys / PAGE_SIZE; }

static inline uintptr_t pfn_to_phys(uint64_t pfn) { return pfn * PAGE_SIZE; }

static inline page_t *page_from_pfn(uint64_t pfn) {
    return pfn == PAGE_LIST_NONE ? NULL : &page_maps[pfn];
}

static inline uintptr_t zone_phys_start(zone_t *zone) {
    return zone->zone_start_pfn * PAGE_SIZE;
}

static inline uintptr_t zone_phys_end(zone_t *zone) {
    return zone->zone_end_pfn * PAGE_SIZE;
}

static inline bool zone_contains_pfn(zone_t *zone, uint64_t pfn) {
    return zone && pfn >= zone->zone_start_pfn && pfn < zone->zone_end_pfn;
}

static inline void page_detach(page_t *page) {
    page->prev_pfn = PAGE_LIST_NONE;
    page->next_pfn = PAGE_LIST_NONE;
}

static inline void page_clear_owner(page_t *page) {
    page->slab_cache = NULL;
    page->freelist = NULL;
    page->slab_state = 0;
}

static inline void page_mark_free(page_t *page, enum zone_type type,
                                  size_t order) {
    __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
    page->flags = PAGE_FLAG_BUDDY;
    page->order = (uint8_t)order;
    page->zone_id = (uint8_t)type;
    page_detach(page);
    page_clear_owner(page);
}

static inline void page_mark_allocated(page_t *page, enum zone_type type,
                                       size_t order) {
    __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
    page->flags = 0;
    page->order = (uint8_t)order;
    page->zone_id = (uint8_t)type;
    page_detach(page);
    page_clear_owner(page);
}

static inline void page_mark_pcp(page_t *page, enum zone_type type) {
    __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
    page->flags = PAGE_FLAG_PCP;
    page->order = 0;
    page->zone_id = (uint8_t)type;
    page_detach(page);
    page_clear_owner(page);
}

static inline bool page_is_buddy(page_t *page, size_t order) {
    return page && (page->flags & PAGE_FLAG_BUDDY) && page->order == order;
}

static bool count_to_order(size_t count, size_t *order_out, size_t *pages_out) {
    if (!order_out || !pages_out || count == 0)
        return false;

    size_t order = 0;
    uint64_t pages = 1;
    while (pages < count) {
        order++;
        if (!order_valid(order))
            return false;
        pages <<= 1;
    }

    *order_out = order;
    *pages_out = (size_t)pages;
    return true;
}

static bool zone_block_valid(zone_t *zone, uintptr_t addr, size_t order) {
    if (!zone || !order_valid(order))
        return false;

    const uint64_t bytes = order_to_bytes(order);
    const uintptr_t end = addr + bytes;
    if (end < addr)
        return false;
    if ((addr & (bytes - 1)) != 0)
        return false;
    return addr >= zone_phys_start(zone) && end <= zone_phys_end(zone);
}

static uint64_t zone_free_pages_locked(zone_t *zone) {
    uint64_t pages = 0;
    for (size_t order = 0; order < MAX_ORDER; order++)
        pages +=
            zone->allocator.free_area[order].nr_free * order_to_pages(order);
    return pages;
}

static void free_area_add(zone_t *zone, size_t order, page_t *page) {
    free_area_t *area = &zone->allocator.free_area[order];
    const uint64_t pfn = page_to_pfn(page);

    page_mark_free(page, zone->type, order);
    if (area->head_pfn != PAGE_LIST_NONE) {
        page_t *head = page_from_pfn(area->head_pfn);
        head->prev_pfn = pfn;
        page->next_pfn = area->head_pfn;
    }

    area->head_pfn = pfn;
    area->nr_free++;
}

static void free_area_del(zone_t *zone, size_t order, page_t *page) {
    free_area_t *area = &zone->allocator.free_area[order];

    if (page->prev_pfn != PAGE_LIST_NONE) {
        page_t *prev = page_from_pfn(page->prev_pfn);
        prev->next_pfn = page->next_pfn;
    } else {
        area->head_pfn = page->next_pfn;
    }

    if (page->next_pfn != PAGE_LIST_NONE) {
        page_t *next = page_from_pfn(page->next_pfn);
        next->prev_pfn = page->prev_pfn;
    }

    if (area->nr_free)
        area->nr_free--;
    page_mark_allocated(page, zone->type, order);
}

static page_t *free_area_pop(zone_t *zone, size_t order) {
    free_area_t *area = &zone->allocator.free_area[order];
    if (area->head_pfn == PAGE_LIST_NONE)
        return NULL;

    page_t *page = page_from_pfn(area->head_pfn);
    free_area_del(zone, order, page);
    return page;
}

static bool pcp_current_cpu(uint32_t *cpu_out) {
    task_t *task = current_task;
    if (!task || task->cpu_id >= cpu_count || task->cpu_id >= MAX_CPU_NUM)
        return false;

    *cpu_out = task->cpu_id;
    return true;
}

static void pcp_push_locked(per_cpu_pages_t *pcp, page_t *page,
                            enum zone_type type) {
    page_mark_pcp(page, type);
    page->next_pfn = pcp->head_pfn;
    pcp->head_pfn = page_to_pfn(page);
    pcp->count++;
}

static page_t *pcp_pop_locked(per_cpu_pages_t *pcp, enum zone_type type) {
    if (pcp->head_pfn == PAGE_LIST_NONE)
        return NULL;

    page_t *page = page_from_pfn(pcp->head_pfn);
    pcp->head_pfn = page->next_pfn;
    if (pcp->count)
        pcp->count--;
    page_mark_allocated(page, type, 0);
    return page;
}

static bool pcp_page_eligible(page_t *page) {
    return page && page->order == 0 && page_refcount_read(page) == 0 &&
           !(page->flags & (PAGE_FLAG_BUDDY | PAGE_FLAG_SLAB | PAGE_FLAG_LARGE |
                            PAGE_FLAG_PCP | PAGE_FLAG_FREEING));
}

static bool block_already_free(zone_t *zone, uint64_t pfn, size_t order) {
    uint64_t end_pfn = pfn + order_to_pages(order);
    if (end_pfn < pfn)
        return true;

    for (size_t free_order = order; free_order < MAX_ORDER; free_order++) {
        uint64_t free_pages = order_to_pages(free_order);
        uint64_t head_pfn = pfn & ~(free_pages - 1);
        if (!zone_contains_pfn(zone, head_pfn))
            continue;
        if (end_pfn <= head_pfn + free_pages &&
            page_is_buddy(page_from_pfn(head_pfn), free_order))
            return true;
    }

    return false;
}

static uintptr_t buddy_alloc_order_locked(zone_t *zone, size_t target_order) {
    page_t *page = NULL;
    size_t order = target_order;

    while (order < MAX_ORDER) {
        page = free_area_pop(zone, order);
        if (page)
            break;
        order++;
    }

    if (!page)
        return 0;

    while (order > target_order) {
        order--;
        page_t *right =
            page_from_pfn(page_to_pfn(page) + order_to_pages(order));
        free_area_add(zone, order, right);
        page_mark_allocated(page, zone->type, order);
    }

    page_mark_allocated(page, zone->type, target_order);
    return page_to_phys(page);
}

static uintptr_t pcp_alloc_order0(zone_t *zone) {
    if (!zone || zone->type != ZONE_NORMAL)
        return 0;

    uint32_t cpu_id = 0;
    if (!pcp_current_cpu(&cpu_id))
        return 0;

    per_cpu_pages_t *pcp = &pcp_pages[cpu_id];

    spin_lock(&pcp->lock);
    page_t *page = pcp_pop_locked(pcp, zone->type);
    spin_unlock(&pcp->lock);
    if (page)
        return page_to_phys(page);

    page_t *batch[PCP_BATCH];
    size_t nr = 0;

    spin_lock(&zone->allocator.lock);
    while (nr < PCP_BATCH) {
        uintptr_t phys = buddy_alloc_order_locked(zone, 0);
        if (!phys)
            break;
        batch[nr++] = phys_to_page(phys);
    }
    spin_unlock(&zone->allocator.lock);

    if (nr == 0)
        return 0;

    page = batch[--nr];
    spin_lock(&pcp->lock);
    while (nr > 0)
        pcp_push_locked(pcp, batch[--nr], zone->type);
    spin_unlock(&pcp->lock);

    page_mark_allocated(page, zone->type, 0);
    return page_to_phys(page);
}

static page_t *buddy_free_order_locked(zone_t *zone, uintptr_t addr,
                                       size_t order) {
    uint64_t pfn = phys_to_pfn(addr);

    while (order + 1 < MAX_ORDER) {
        const uint64_t buddy_pfn = pfn ^ order_to_pages(order);
        const uintptr_t buddy_phys = pfn_to_phys(buddy_pfn);

        if (!zone_contains_pfn(zone, buddy_pfn) ||
            !zone_block_valid(zone, buddy_phys, order))
            break;

        page_t *buddy = page_from_pfn(buddy_pfn);
        if (!page_is_buddy(buddy, order))
            break;

        free_area_del(zone, order, buddy);
        if (buddy_pfn < pfn)
            pfn = buddy_pfn;
        order++;
    }

    page_t *head = page_from_pfn(pfn);
    free_area_add(zone, order, head);
    return head;
}

static bool pcp_free_order0(zone_t *zone, page_t *page) {
    if (!zone || zone->type != ZONE_NORMAL || !pcp_page_eligible(page))
        return false;

    uint32_t cpu_id = 0;
    if (!pcp_current_cpu(&cpu_id))
        return false;

    page_t *drain[PCP_BATCH];
    size_t nr = 0;
    per_cpu_pages_t *pcp = &pcp_pages[cpu_id];

    spin_lock(&pcp->lock);
    pcp_push_locked(pcp, page, zone->type);

    if (pcp->count > PCP_HIGH) {
        while (nr < PCP_BATCH) {
            page_t *drained = pcp_pop_locked(pcp, zone->type);
            if (!drained)
                break;
            drain[nr++] = drained;
        }
    }
    spin_unlock(&pcp->lock);

    if (nr) {
        spin_lock(&zone->allocator.lock);
        for (size_t i = 0; i < nr; i++)
            buddy_free_order_locked(zone, page_to_phys(drain[i]), 0);
        spin_unlock(&zone->allocator.lock);
    }

    return true;
}

enum zone_type phys_to_zone_type(uintptr_t phys) {
    (void)phys;
    return ZONE_NORMAL;
}

zone_t *get_zone(enum zone_type type) {
    if (type >= __MAX_NR_ZONES)
        return NULL;
    return zones[type];
}

bool zone_has_memory(zone_t *zone) { return zone_free_pages(zone) > 0; }

uint64_t zone_free_pages(zone_t *zone) {
    if (!zone)
        return 0;

    spin_lock(&zone->allocator.lock);
    uint64_t pages = zone_free_pages_locked(zone);
    spin_unlock(&zone->allocator.lock);

    if (zone->type == ZONE_NORMAL) {
        for (uint32_t cpu = 0; cpu < MAX_CPU_NUM; cpu++) {
            spin_lock(&pcp_pages[cpu].lock);
            pages += pcp_pages[cpu].count;
            spin_unlock(&pcp_pages[cpu].lock);
        }
    }

    return pages;
}

void buddy_free_zone(zone_t *zone, uintptr_t addr, size_t order) {
    if (!zone_block_valid(zone, addr, order))
        return;

    page_t *head = phys_to_page(addr);
    if (!head ||
        (head->flags & (PAGE_FLAG_BUDDY | PAGE_FLAG_PCP | PAGE_FLAG_SLAB |
                        PAGE_FLAG_LARGE | PAGE_FLAG_FREEING)))
        return;
    if (block_already_free(zone, phys_to_pfn(addr), order))
        return;

    if (order == 0 && pcp_free_order0(zone, head))
        return;

    spin_lock(&zone->allocator.lock);
    buddy_free_order_locked(zone, addr, order);
    spin_unlock(&zone->allocator.lock);
}

void buddy_free_zone_claimed(zone_t *zone, uintptr_t addr, size_t order) {
    if (!zone_block_valid(zone, addr, order))
        return;

    page_t *claimed = phys_to_page(addr);
    if (!claimed || !(claimed->flags & PAGE_FLAG_FREEING))
        return;

    spin_lock(&zone->allocator.lock);
    page_t *head = buddy_free_order_locked(zone, addr, order);
    if (head != claimed && (claimed->flags & PAGE_FLAG_FREEING))
        page_mark_allocated(claimed, zone->type, 0);
    spin_unlock(&zone->allocator.lock);
}

uintptr_t buddy_alloc_zone_pages(zone_t *zone, size_t count,
                                 size_t *allocated_pages) {
    if (!zone || count == 0)
        return 0;

    size_t order = 0;
    size_t pages = 0;
    if (!count_to_order(count, &order, &pages))
        return 0;

    if (order == 0) {
        uintptr_t addr = pcp_alloc_order0(zone);
        if (addr) {
            if (allocated_pages)
                *allocated_pages = pages;
            return addr;
        }
    }

    spin_lock(&zone->allocator.lock);
    uintptr_t addr = buddy_alloc_order_locked(zone, order);
    spin_unlock(&zone->allocator.lock);

    if (addr && allocated_pages)
        *allocated_pages = pages;
    return addr;
}

uintptr_t buddy_alloc_zone(zone_t *zone, size_t count) {
    return buddy_alloc_zone_pages(zone, count, NULL);
}

static void init_zone(zone_t *zone, enum zone_type type, uint64_t start_pfn,
                      uint64_t end_pfn) {
    memset(zone, 0, sizeof(*zone));
    zone->type = type;
    zone->name = zone_names[type];
    zone->zone_start_pfn = start_pfn;
    zone->zone_end_pfn = end_pfn;
    spin_init(&zone->allocator.lock);

    for (size_t order = 0; order < MAX_ORDER; order++) {
        zone->allocator.free_area[order].head_pfn = PAGE_LIST_NONE;
        zone->allocator.free_area[order].nr_free = 0;
    }
}

static void create_zone(enum zone_type type, uint64_t start_pfn,
                        uint64_t end_pfn) {
    if (type >= __MAX_NR_ZONES || end_pfn <= start_pfn) {
        zones[type] = NULL;
        return;
    }

    zone_t *zone = early_alloc(sizeof(*zone));
    ASSERT(zone != NULL);
    init_zone(zone, type, start_pfn, end_pfn);
    zones[type] = zone;
    nr_zones++;
}

void buddy_init(void) {
    memset(zones, 0, sizeof(zones));
    memset(pcp_pages, 0, sizeof(pcp_pages));
    nr_zones = 0;
    for (uint32_t cpu = 0; cpu < MAX_CPU_NUM; cpu++) {
        spin_init(&pcp_pages[cpu].lock);
        pcp_pages[cpu].head_pfn = PAGE_LIST_NONE;
    }
    create_zone(ZONE_NORMAL, 0, memory_size / PAGE_SIZE);
}

static size_t floor_order_for_pages(uint64_t pages) {
    size_t order = 0;
    while (order + 1 < MAX_ORDER && (1ULL << (order + 1)) <= pages)
        order++;
    return order;
}

void add_memory_region(uintptr_t start, uintptr_t end, enum zone_type type) {
    zone_t *zone = get_zone(type);
    if (!zone || start >= end)
        return;

    start = PADDING_UP(start, PAGE_SIZE);
    end = PADDING_DOWN(end, PAGE_SIZE);
    if (start == 0)
        start = PAGE_SIZE;
    if (start >= end)
        return;

    if (start < zone_phys_start(zone))
        start = zone_phys_start(zone);
    if (end > zone_phys_end(zone))
        end = zone_phys_end(zone);
    if (start >= end)
        return;

    uint64_t pfn = phys_to_pfn(start);
    const uint64_t end_pfn = phys_to_pfn(end);

    spin_lock(&zone->allocator.lock);
    while (pfn < end_pfn) {
        size_t order_by_size = floor_order_for_pages(end_pfn - pfn);
        size_t order_by_align = (size_t)__builtin_ctzll(pfn);
        if (order_by_align >= MAX_ORDER)
            order_by_align = MAX_ORDER - 1;

        size_t order = MIN(order_by_size, order_by_align);
        buddy_free_order_locked(zone, pfn_to_phys(pfn), order);
        zone->managed_pages += order_to_pages(order);
        pfn += order_to_pages(order);
    }
    spin_unlock(&zone->allocator.lock);
}

static bool claim_last_page_refs(uintptr_t addr, size_t pages) {
    for (size_t offset = 0; offset < pages; offset++) {
        page_t *page = phys_to_page(addr + offset * PAGE_SIZE);
        if (page && page_try_release_last(page))
            continue;

        for (size_t rollback = 0; rollback < offset; rollback++)
            page_ref(phys_to_page(addr + rollback * PAGE_SIZE));
        return false;
    }

    return true;
}

static bool pages_are_unreferenced(uintptr_t addr, size_t pages) {
    for (size_t offset = 0; offset < pages; offset++) {
        page_t *page = phys_to_page(addr + offset * PAGE_SIZE);
        if (!page || page_refcount_read(page) != 0)
            return false;
    }

    return true;
}

uintptr_t alloc_frames(size_t count) {
    if (count == 0)
        return 0;

    size_t order = 0;
    size_t pages = 0;
    if (!count_to_order(count, &order, &pages))
        return 0;

    uintptr_t addr = 0;
    zone_t *zone = get_zone(ZONE_NORMAL);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (zone) {
            addr = buddy_alloc_zone(zone, count);
            if (addr)
                break;
        }

        if (attempt == 0)
            task_reap_deferred(512);
        else if (attempt == 1)
            (void)page_cache_reclaim(MAX((uint64_t)pages, 64ULL));
    }

    if (!addr)
        ASSERT(!"Out of memory");

    for (size_t offset = 0; offset < pages; offset++) {
        page_t *page = phys_to_page(addr + offset * PAGE_SIZE);
        if (!page)
            continue;
        page_mark_allocated(page, ZONE_NORMAL, 0);
        page_ref(page);
    }

    page_t *head = phys_to_page(addr);
    if (head)
        head->order = (uint8_t)order;

    return addr;
}

static void free_frames_common(uintptr_t addr, size_t count,
                               bool refs_already_released) {
    if (addr == 0 || count == 0 || (addr & (PAGE_SIZE - 1)) != 0)
        return;

    size_t order = 0;
    size_t pages = 0;
    if (!count_to_order(count, &order, &pages))
        return;

    zone_t *zone = get_zone(ZONE_NORMAL);
    if (!zone || !zone_block_valid(zone, addr, order))
        return;

    const size_t start_page = addr / PAGE_SIZE;
    if (start_page + pages < start_page ||
        start_page + pages > usable_regions.length)
        return;

    for (size_t offset = 0; offset < pages; offset++) {
        if (!bitmap_get(&usable_regions, start_page + offset))
            return;
    }

    page_t *head = phys_to_page(addr);
    if (!head ||
        (head->flags & (PAGE_FLAG_BUDDY | PAGE_FLAG_SLAB | PAGE_FLAG_LARGE |
                        PAGE_FLAG_PCP | PAGE_FLAG_FREEING)))
        return;

    if (refs_already_released) {
        if (!pages_are_unreferenced(addr, pages))
            return;
    } else if (!claim_last_page_refs(addr, pages)) {
        return;
    }

    if (order == 0 && pcp_free_order0(zone, head))
        return;

    spin_lock(&zone->allocator.lock);
    buddy_free_order_locked(zone, addr, order);
    spin_unlock(&zone->allocator.lock);
}

void free_frames(uintptr_t addr, size_t count) {
    free_frames_common(addr, count, false);
}

void free_frames_released(uintptr_t addr, size_t count) {
    free_frames_common(addr, count, true);
}
