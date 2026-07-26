#include <mm/slub.h>
#include <mm/buddy.h>
#include <mm/cache.h>
#include <mm/hhdm.h>
#include <mm/mm.h>
#include <mm/page.h>

#define KMALLOC_ALIGN 16UL
#define SLAB_INUSE_SHIFT 16
#define SLAB_OBJECT_MASK 0xffffU

typedef struct slab_object {
    struct slab_object *next;
} slab_object_t;

typedef struct kmem_cache {
    size_t object_size;
    size_t order;
    spinlock_t lock;
    uint64_t partial_head_pfn;
    uint64_t empty_slab_pfn;
} kmem_cache_t;

static const size_t kmalloc_cache_sizes[] = {
    32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096,
};

#define KMALLOC_NR_CACHES                                                      \
    (sizeof(kmalloc_cache_sizes) / sizeof(kmalloc_cache_sizes[0]))

static kmem_cache_t kmalloc_caches[KMALLOC_NR_CACHES];

static size_t malloc_requested_size(void *ptr);
static void malloc_set_requested_size(void *ptr, size_t size);

static inline bool is_power_of_two(size_t value) {
    return value && ((value & (value - 1)) == 0);
}

static bool normalize_alignment(size_t *alignment) {
    if (*alignment < KMALLOC_ALIGN) {
        *alignment = KMALLOC_ALIGN;
        return true;
    }

    if (is_power_of_two(*alignment))
        return true;

    size_t fixed = KMALLOC_ALIGN;
    while (fixed < *alignment) {
        if (fixed > (SIZE_MAX >> 1))
            return false;
        fixed <<= 1;
    }

    *alignment = fixed;
    return true;
}

static inline uint64_t order_pages(size_t order) { return 1ULL << order; }

static inline size_t order_bytes(size_t order) {
    return (size_t)order_pages(order) * PAGE_SIZE;
}

static inline uint16_t slab_inuse(page_t *page) {
    return (uint16_t)(page->slab_state >> SLAB_INUSE_SHIFT);
}

static inline uint16_t slab_objects(page_t *page) {
    return (uint16_t)(page->slab_state & SLAB_OBJECT_MASK);
}

static inline void slab_set_state(page_t *page, uint16_t inuse,
                                  uint16_t objects) {
    page->slab_state = ((uint32_t)inuse << SLAB_INUSE_SHIFT) | objects;
}

static inline size_t bitmap_bytes(size_t bits) { return (bits + 7) / 8; }

static inline size_t align_up_size(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static inline page_t *page_from_pfn(uint64_t pfn) {
    return pfn == PAGE_LIST_NONE ? NULL : &page_maps[pfn];
}

static page_t *slab_head_from_page(page_t *page) {
    if (!page || !(page->flags & (PAGE_FLAG_SLAB | PAGE_FLAG_LARGE)))
        return NULL;

    uint64_t mask = order_pages(page->order) - 1;
    return page_from_pfn(page_to_pfn(page) & ~mask);
}

static void list_push(kmem_cache_t *cache, page_t *page) {
    uint64_t pfn = page_to_pfn(page);
    page->prev_pfn = PAGE_LIST_NONE;
    page->next_pfn = cache->partial_head_pfn;
    if (cache->partial_head_pfn != PAGE_LIST_NONE)
        page_from_pfn(cache->partial_head_pfn)->prev_pfn = pfn;
    cache->partial_head_pfn = pfn;
}

static void list_remove(kmem_cache_t *cache, page_t *page) {
    if (page->prev_pfn != PAGE_LIST_NONE)
        page_from_pfn(page->prev_pfn)->next_pfn = page->next_pfn;
    else
        cache->partial_head_pfn = page->next_pfn;

    if (page->next_pfn != PAGE_LIST_NONE)
        page_from_pfn(page->next_pfn)->prev_pfn = page->prev_pfn;

    page->prev_pfn = PAGE_LIST_NONE;
    page->next_pfn = PAGE_LIST_NONE;
}

static void clear_slab_pages(page_t *head) {
    size_t pages = order_pages(head->order);
    uintptr_t phys = page_to_phys(head);

    for (size_t i = 0; i < pages; i++) {
        page_t *page = phys_to_page(phys + i * PAGE_SIZE);
        __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
        page->flags = 0;
        page->prev_pfn = PAGE_LIST_NONE;
        page->next_pfn = PAGE_LIST_NONE;
        page->slab_cache = NULL;
        page->freelist = NULL;
        page->slab_state = 0;
    }
}

static void release_slab(page_t *slab) {
    uintptr_t phys = page_to_phys(slab);
    size_t order = slab->order;

    clear_slab_pages(slab);
    buddy_free_zone(get_zone(ZONE_NORMAL), phys, order);
}

static void clear_claimed_large_pages(page_t *head) {
    size_t pages = order_pages(head->order);
    uintptr_t phys = page_to_phys(head);

    for (size_t i = 0; i < pages; i++) {
        page_t *page = phys_to_page(phys + i * PAGE_SIZE);
        __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
        if (i != 0)
            page->flags = 0;
        page->prev_pfn = PAGE_LIST_NONE;
        page->next_pfn = PAGE_LIST_NONE;
        page->slab_cache = NULL;
        page->freelist = NULL;
        page->slab_state = 0;
    }
}

static uint8_t *slab_bitmap(page_t *slab) {
    return (uint8_t *)phys_to_virt(page_to_phys(slab));
}

static uint16_t *slab_requested_sizes(page_t *slab) {
    size_t offset =
        align_up_size(bitmap_bytes(slab_objects(slab)), sizeof(uint16_t));
    return (uint16_t *)(slab_bitmap(slab) + offset);
}

static size_t slab_metadata_size(size_t objects) {
    size_t offset = align_up_size(bitmap_bytes(objects), sizeof(uint16_t));
    return align_up_size(offset + objects * sizeof(uint16_t), KMALLOC_ALIGN);
}

static bool slab_object_allocated(page_t *slab, size_t index) {
    uint8_t *bitmap = slab_bitmap(slab);
    return bitmap[index / 8] & (uint8_t)(1U << (index % 8));
}

static void slab_set_object_allocated(page_t *slab, size_t index,
                                      bool allocated) {
    uint8_t *bitmap = slab_bitmap(slab);
    uint8_t mask = (uint8_t)(1U << (index % 8));
    if (allocated)
        bitmap[index / 8] |= mask;
    else
        bitmap[index / 8] &= (uint8_t)~mask;
}

static size_t cache_objects_for_order(size_t object_size, size_t order) {
    size_t bytes = order_bytes(order);
    size_t objects = bytes / object_size;

    while (objects > 0) {
        size_t meta = slab_metadata_size(objects);
        if (meta < bytes && (bytes - meta) / object_size >= objects)
            return objects;
        objects--;
    }

    return 0;
}

static size_t cache_order_for_size(size_t object_size) {
    for (size_t order = 0; order < MAX_ORDER; order++) {
        if (cache_objects_for_order(object_size, order) != 0)
            return order;
    }

    return MAX_ORDER;
}

void kmalloc_init(void) {
    zone_t *zone = get_zone(ZONE_NORMAL);
    if (!zone || !zone_has_memory(zone)) {
        return;
    }

    for (size_t i = 0; i < KMALLOC_NR_CACHES; i++) {
        kmem_cache_t *cache = &kmalloc_caches[i];
        cache->object_size = kmalloc_cache_sizes[i];
        cache->order = cache_order_for_size(cache->object_size);
        if (cache->order >= MAX_ORDER) {
            return;
        }
        cache->partial_head_pfn = PAGE_LIST_NONE;
        cache->empty_slab_pfn = PAGE_LIST_NONE;
        spin_init(&cache->lock);
    }
}

static int cache_index_for_size(size_t size) {
    if (size == 0)
        size = 1;

    for (size_t i = 0; i < KMALLOC_NR_CACHES; i++) {
        if (size <= kmalloc_cache_sizes[i])
            return (int)i;
    }

    return -1;
}

static page_t *cache_grow(kmem_cache_t *cache) {
    size_t pages = order_pages(cache->order);
    size_t allocated_pages = 0;
    zone_t *zone = get_zone(ZONE_NORMAL);
    uintptr_t phys = buddy_alloc_zone_pages(zone, pages, &allocated_pages);
    if (!phys || allocated_pages != pages)
        return NULL;

    void *mem = phys_to_virt(phys);
    if (!mem) {
        buddy_free_zone(zone, phys, cache->order);
        return NULL;
    }

    size_t objects = cache_objects_for_order(cache->object_size, cache->order);
    if (objects == 0 || objects > SLAB_OBJECT_MASK) {
        buddy_free_zone(zone, phys, cache->order);
        return NULL;
    }

    memset(mem, 0, slab_metadata_size(objects));
    uintptr_t object_start = (uintptr_t)mem + slab_metadata_size(objects);
    slab_object_t *freelist = NULL;
    for (size_t i = 0; i < objects; i++) {
        slab_object_t *object =
            (slab_object_t *)(object_start + i * cache->object_size);
        object->next = freelist;
        freelist = object;
    }

    page_t *head = phys_to_page(phys);
    for (size_t i = 0; i < pages; i++) {
        page_t *page = phys_to_page(phys + i * PAGE_SIZE);
        __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
        page->flags = PAGE_FLAG_SLAB;
        page->order = (uint8_t)cache->order;
        page->zone_id = ZONE_NORMAL;
        page->prev_pfn = PAGE_LIST_NONE;
        page->next_pfn = PAGE_LIST_NONE;
        page->slab_cache = cache;
        page->freelist = NULL;
        page->slab_state = 0;
    }

    head->freelist = freelist;
    slab_set_state(head, 0, (uint16_t)objects);
    return head;
}

static void *cache_alloc(size_t size, size_t cache_index) {
    kmem_cache_t *cache = &kmalloc_caches[cache_index];
    bool reclaimed = false;

retry:
    spin_lock(&cache->lock);

    page_t *slab = page_from_pfn(cache->partial_head_pfn);
    if (!slab) {
        slab = cache_grow(cache);
        if (!slab) {
            spin_unlock(&cache->lock);
            if (!reclaimed) {
                reclaimed = true;
                (void)malloc_trim(0);
                (void)page_cache_reclaim(32);
                goto retry;
            }
            return NULL;
        }
        list_push(cache, slab);
    }

    if (cache->empty_slab_pfn != PAGE_LIST_NONE &&
        cache->empty_slab_pfn == page_to_pfn(slab))
        cache->empty_slab_pfn = PAGE_LIST_NONE;

    slab_object_t *object = slab->freelist;
    slab->freelist = object->next;

    uint16_t inuse = slab_inuse(slab) + 1;
    uint16_t objects = slab_objects(slab);
    size_t index =
        ((uintptr_t)object - ((uintptr_t)phys_to_virt(page_to_phys(slab)) +
                              slab_metadata_size(objects))) /
        cache->object_size;
    slab_set_object_allocated(slab, index, true);
    slab_requested_sizes(slab)[index] = (uint16_t)size;
    slab_set_state(slab, inuse, objects);
    if (inuse == objects)
        list_remove(cache, slab);

    spin_unlock(&cache->lock);
    return object;
}

static bool cache_object_index(page_t *slab, void *ptr, size_t *index_out) {
    uintptr_t base = (uintptr_t)phys_to_virt(page_to_phys(slab));
    uintptr_t addr = (uintptr_t)ptr;
    size_t bytes = order_bytes(slab->order);
    kmem_cache_t *cache = slab->slab_cache;
    size_t objects = slab_objects(slab);
    uintptr_t object_base = base + slab_metadata_size(objects);

    if (!cache || addr < object_base || addr >= base + bytes)
        return false;
    if (((addr - object_base) % cache->object_size) != 0)
        return false;
    size_t index = (addr - object_base) / cache->object_size;
    if (index >= objects)
        return false;
    if (index_out)
        *index_out = index;
    return true;
}

static void cache_free(page_t *page, void *ptr) {
    page_t *slab = slab_head_from_page(page);
    size_t index = 0;
    if (!slab || !(slab->flags & PAGE_FLAG_SLAB) ||
        !cache_object_index(slab, ptr, &index))
        return;

    kmem_cache_t *cache = slab->slab_cache;
    spin_lock(&cache->lock);

    if (!slab_object_allocated(slab, index)) {
        spin_unlock(&cache->lock);
#ifdef DEBUG
        ASSERT(!"double free or invalid slab free");
#else
        return;
#endif
    }

    uint16_t inuse = slab_inuse(slab);
    uint16_t objects = slab_objects(slab);
    bool was_full = inuse == objects;

    slab_set_object_allocated(slab, index, false);
    slab_requested_sizes(slab)[index] = 0;
    slab_object_t *object = ptr;
    object->next = slab->freelist;
    slab->freelist = object;
    if (inuse)
        inuse--;
    slab_set_state(slab, inuse, objects);

    if (inuse == 0) {
        if (cache->empty_slab_pfn == PAGE_LIST_NONE) {
            if (was_full)
                list_push(cache, slab);
            cache->empty_slab_pfn = page_to_pfn(slab);
            spin_unlock(&cache->lock);
            return;
        }

        if (!was_full)
            list_remove(cache, slab);
        spin_unlock(&cache->lock);
        release_slab(slab);
        return;
    }

    if (was_full)
        list_push(cache, slab);

    spin_unlock(&cache->lock);
}

static size_t pages_to_order(size_t pages) {
    size_t order = 0;
    size_t count = 1;
    while (count < pages) {
        order++;
        if (order >= MAX_ORDER)
            return MAX_ORDER;
        count <<= 1;
    }
    return order;
}

static void mark_large_pages(uintptr_t phys, size_t order,
                             size_t requested_size) {
    size_t pages = order_pages(order);
    for (size_t i = 0; i < pages; i++) {
        page_t *page = phys_to_page(phys + i * PAGE_SIZE);
        __atomic_store_n(&page->refcount, 0, __ATOMIC_RELEASE);
        page->flags = PAGE_FLAG_LARGE;
        page->order = (uint8_t)order;
        page->zone_id = ZONE_NORMAL;
        page->prev_pfn = PAGE_LIST_NONE;
        page->next_pfn = PAGE_LIST_NONE;
        page->slab_cache = NULL;
        page->freelist = NULL;
        page->slab_state = 0;
    }
    phys_to_page(phys)->requested_size = requested_size;
}

static void *large_alloc_aligned(size_t size, size_t alignment) {
    if (size == 0)
        size = 1;
    if (!normalize_alignment(&alignment))
        return NULL;

    size_t bytes = MAX(size, alignment);
    if (bytes > SIZE_MAX - PAGE_SIZE + 1)
        return NULL;

    size_t pages = PADDING_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    size_t order = pages_to_order(pages);
    if (order >= MAX_ORDER)
        return NULL;

    size_t allocated_pages = 0;
    zone_t *zone = get_zone(ZONE_NORMAL);
    uintptr_t phys =
        buddy_alloc_zone_pages(zone, order_pages(order), &allocated_pages);
    if (!phys || allocated_pages != order_pages(order)) {
        (void)malloc_trim(0);
        (void)page_cache_reclaim(MAX((uint64_t)order_pages(order), 64ULL));
        phys =
            buddy_alloc_zone_pages(zone, order_pages(order), &allocated_pages);
    }
    if (!phys || allocated_pages != order_pages(order))
        return NULL;

    void *ptr = phys_to_virt(phys);
    if (!ptr) {
        buddy_free_zone(zone, phys, order);
        return NULL;
    }

    if (((uintptr_t)ptr & (alignment - 1)) != 0) {
        buddy_free_zone(zone, phys, order);
        return NULL;
    }

    mark_large_pages(phys, order, size);
    return ptr;
}

static void large_free(page_t *page, void *ptr) {
    page_t *head = slab_head_from_page(page);
    if (!head && page && (page->flags & PAGE_FLAG_FREEING))
        head = page;
    if (!head)
        return;
    if (ptr != phys_to_virt(page_to_phys(head)))
        return;

    uintptr_t phys = page_to_phys(head);
    size_t order = head->order;
    uint8_t expected = PAGE_FLAG_LARGE;
    if (!__atomic_compare_exchange_n(&head->flags, &expected, PAGE_FLAG_FREEING,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ASSERT(!"double free or invalid large free");

    clear_claimed_large_pages(head);
    buddy_free_zone_claimed(get_zone(ZONE_NORMAL), phys, order);
}

void *malloc(size_t size) {
    if (size == 0)
        size = 1;

    int cache_index = cache_index_for_size(size);
    if (cache_index >= 0)
        return cache_alloc(size, (size_t)cache_index);

    return large_alloc_aligned(size, KMALLOC_ALIGN);
}

void free(void *ptr) {
    if (!ptr)
        return;

    uint64_t phys = virt_to_phys(ptr);
    page_t *page = phys_to_page(phys);
    if (!page)
        return;

    if (page->flags & PAGE_FLAG_SLAB)
        cache_free(page, ptr);
    else if (page->flags & (PAGE_FLAG_LARGE | PAGE_FLAG_FREEING))
        large_free(page, ptr);
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb)
        return NULL;

    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)
        return malloc(size);

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    size_t old_requested = malloc_requested_size(ptr);
    size_t old_usable = malloc_usable_size(ptr);
    if (old_usable == 0)
        return NULL;
    if (size <= old_usable) {
        malloc_set_requested_size(ptr, size);
        return ptr;
    }

    void *new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, MIN(old_requested, size));
    free(ptr);
    return new_ptr;
}

static size_t malloc_requested_size(void *ptr) {
    uint64_t phys = virt_to_phys(ptr);
    page_t *page = phys_to_page(phys);
    if (!page)
        return 0;

    if (page->flags & PAGE_FLAG_LARGE) {
        page_t *head = slab_head_from_page(page);
        if (!head || ptr != phys_to_virt(page_to_phys(head)))
            return 0;
        return head->requested_size;
    }

    if (page->flags & PAGE_FLAG_SLAB) {
        page_t *slab = slab_head_from_page(page);
        size_t index = 0;
        if (!slab || !cache_object_index(slab, ptr, &index) ||
            !slab_object_allocated(slab, index))
            return 0;
        return slab_requested_sizes(slab)[index];
    }

    return 0;
}

static void malloc_set_requested_size(void *ptr, size_t size) {
    uint64_t phys = virt_to_phys(ptr);
    page_t *page = phys_to_page(phys);
    if (!page)
        return;

    if (page->flags & PAGE_FLAG_LARGE) {
        page_t *head = slab_head_from_page(page);
        if (head && ptr == phys_to_virt(page_to_phys(head)))
            head->requested_size = size;
        return;
    }

    if (page->flags & PAGE_FLAG_SLAB) {
        page_t *slab = slab_head_from_page(page);
        size_t index = 0;
        if (slab && cache_object_index(slab, ptr, &index) &&
            slab_object_allocated(slab, index))
            slab_requested_sizes(slab)[index] = (uint16_t)size;
    }
}

size_t malloc_usable_size(void *ptr) {
    if (!ptr)
        return 0;

    uint64_t phys = virt_to_phys(ptr);
    page_t *page = phys_to_page(phys);
    if (!page)
        return 0;

    if (page->flags & PAGE_FLAG_LARGE) {
        page_t *head = slab_head_from_page(page);
        if (!head || ptr != phys_to_virt(page_to_phys(head)))
            return 0;
        return order_bytes(head->order);
    }

    if (page->flags & PAGE_FLAG_SLAB) {
        page_t *slab = slab_head_from_page(page);
        size_t index = 0;
        if (!slab || !cache_object_index(slab, ptr, &index) ||
            !slab_object_allocated(slab, index))
            return 0;
        return slab->slab_cache->object_size;
    }

    return 0;
}

void *memalign(size_t alignment, size_t size) {
    if (alignment <= KMALLOC_ALIGN)
        return malloc(size);
    return large_alloc_aligned(size, alignment);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (!memptr || alignment < sizeof(void *) || !is_power_of_two(alignment))
        return EINVAL;

    void *ptr = memalign(alignment, size);
    if (!ptr)
        return ENOMEM;

    *memptr = ptr;
    return 0;
}

void *valloc(size_t size) { return memalign(PAGE_SIZE, size); }

void *pvalloc(size_t size) {
    if (size > SIZE_MAX - PAGE_SIZE + 1)
        return NULL;
    return memalign(PAGE_SIZE, PADDING_UP(size, PAGE_SIZE));
}

int malloc_trim(size_t pad) {
    size_t retained = 0;
    bool released = false;

    for (size_t i = 0; i < KMALLOC_NR_CACHES; i++) {
        kmem_cache_t *cache = &kmalloc_caches[i];
        size_t bytes = order_bytes(cache->order);

        spin_lock(&cache->lock);
        page_t *slab = page_from_pfn(cache->empty_slab_pfn);
        if (!slab) {
            cache->empty_slab_pfn = PAGE_LIST_NONE;
            spin_unlock(&cache->lock);
            continue;
        }
        if (retained < pad && bytes <= pad - retained) {
            retained += bytes;
            spin_unlock(&cache->lock);
            continue;
        }

        list_remove(cache, slab);
        cache->empty_slab_pfn = PAGE_LIST_NONE;
        spin_unlock(&cache->lock);

        release_slab(slab);
        released = true;
    }

    return released;
}

int mallopt(int param, int value) {
    (void)param;
    (void)value;
    return 0;
}

struct mallinfo mallinfo(void) {
    struct mallinfo info;
    memset(&info, 0, sizeof(info));
    return info;
}

void malloc_stats(void) {}
