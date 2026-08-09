#include <mm/hhdm.h>
#include <mm/mm.h>
#include <mm/slub.h>
#include <mod/rust/api.h>

void *na_memory_allocate(uint64_t bytes) { return alloc_frames_bytes(bytes); }

void na_memory_free(void *ptr, uint64_t bytes) {
    if (ptr)
        free_frames_bytes(ptr, bytes);
}

void *na_heap_allocate(size_t bytes) { return bytes ? malloc(bytes) : NULL; }

void *na_heap_allocate_aligned(size_t bytes, size_t alignment) {
    return bytes ? memalign(alignment, bytes) : NULL;
}

void *na_heap_reallocate(void *ptr, size_t bytes) {
    return realloc(ptr, bytes);
}

void *na_heap_reallocate_aligned(void *ptr, size_t bytes, size_t alignment) {
    if (!ptr)
        return bytes ? memalign(alignment, bytes) : NULL;
    if (!bytes) {
        free(ptr);
        return NULL;
    }

    size_t usable = malloc_usable_size(ptr);
    if (usable && bytes <= usable && ((uintptr_t)ptr & (alignment - 1)) == 0)
        return ptr;

    void *replacement = memalign(alignment, bytes);
    if (!replacement)
        return NULL;
    if (usable)
        memcpy(replacement, ptr, usable < bytes ? usable : bytes);
    free(ptr);
    return replacement;
}

void na_heap_free(void *ptr) { free(ptr); }

uint64_t na_memory_physical_address(const void *ptr) {
    return ptr ? virt_to_phys(ptr) : 0;
}

void na_dma_sync_for_device(void *address, size_t size) {
    write_barrier();
    dcache_clean_range(address, size);
}

void na_dma_sync_for_cpu(void *address, size_t size) {
    dcache_invalidate_range(address, size);
    read_barrier();
}

int na_user_read(uint64_t address, void *destination, size_t size) {
    if (!destination || !address)
        return -EFAULT;
    return copy_from_user(destination, (const void *)(uintptr_t)address, size)
               ? -EFAULT
               : 0;
}

int na_user_write(uint64_t address, const void *source, size_t size) {
    if (!source || !address)
        return -EFAULT;
    return copy_to_user((void *)(uintptr_t)address, source, size) ? -EFAULT : 0;
}
