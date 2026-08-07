#pragma once

#include <libs/klibc.h>

#define MODULE_NAME_SECTION ".naos.module.name"
#define MODULE_DEPS_SECTION ".naos.module.deps"

#define __MODULE_CONCAT_INNER(a, b) a##b
#define __MODULE_CONCAT(a, b) __MODULE_CONCAT_INNER(a, b)
#define __MODULE_METADATA_STRING(section_name, value, id)                      \
    static const char __MODULE_CONCAT(__module_metadata_, id)[]                \
        __attribute__((used, section(section_name), aligned(1))) = value

/*
 * Declare a direct module dependency. Undefined symbols are resolved only
 * against the kernel and modules named by MODULE_DEPENDS(). Dependencies are
 * intentionally not inherited: users of a transitive dependency must declare
 * it themselves.
 */
#define MODULE_DEPENDS(module_name)                                            \
    __MODULE_METADATA_STRING(MODULE_DEPS_SECTION, module_name, __COUNTER__)

/* The build system emits this record from KM_NAME for every loadable module. */
#define MODULE_DECLARE_NAME(module_name)                                       \
    __MODULE_METADATA_STRING(MODULE_NAME_SECTION, module_name, __COUNTER__)

#ifdef MODULE_BUILD_NAME
MODULE_DECLARE_NAME(MODULE_BUILD_NAME);
#endif

#ifdef MODULE_BUILD_DEPENDENCY
MODULE_DEPENDS(MODULE_BUILD_DEPENDENCY);
#endif

typedef struct {
    bool is_use;
    bool mapped;
    char module_name[64];
    char **dependencies;
    size_t dependency_count;
    char *path;
    uint8_t *data;
    size_t size;
    uint64_t load_base;
    size_t load_size;
} module_t;
