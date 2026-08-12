#pragma once

#include <libs/klibc.h>

#define MODULE_NAME_SECTION ".naos.module.name"
#define MODULE_DEPS_SECTION ".naos.module.deps"
#define MODULE_LICENSE_SECTION ".naos.module.license"
#define MODULE_ALIAS_SECTION ".naos.module.alias"

#define __MODULE_CONCAT_INNER(a, b) a##b
#define __MODULE_CONCAT(a, b) __MODULE_CONCAT_INNER(a, b)
#define __MODULE_METADATA_STRING(section_name, value, id)                      \
    static const char __MODULE_CONCAT(__module_metadata_, id)[]                \
        __attribute__((used, section(section_name), aligned(1))) = value

#define MODULE_DEPENDS(module_name)                                            \
    __MODULE_METADATA_STRING(MODULE_DEPS_SECTION, module_name, __COUNTER__)

#define MODULE_DECLARE_NAME(module_name)                                       \
    __MODULE_METADATA_STRING(MODULE_NAME_SECTION, module_name, __COUNTER__)

#define MODULE_LICENSE(license)                                                \
    __MODULE_METADATA_STRING(MODULE_LICENSE_SECTION, license, __COUNTER__)
#define MODULE_ALIAS(alias)                                                    \
    __MODULE_METADATA_STRING(MODULE_ALIAS_SECTION, alias, __COUNTER__)
#define MODULE_AUTHOR(author) ((void)0)
#define MODULE_DESCRIPTION(description) ((void)0)
#define MODULE_VERSION(version) ((void)0)
#define MODULE_FIRMWARE(name) ((void)0)
#define MODULE_DEVICE_TABLE(type, name) ((void)0)

#define __NAOS_MODULE_INIT_NAME __naos_linux_init
#define __NAOS_MODULE_EXIT_NAME __naos_linux_exit
#define module_init(initfn)                                                    \
    int __NAOS_MODULE_INIT_NAME(void) { return (initfn)(); }
#define module_exit(exitfn)                                                    \
    void __NAOS_MODULE_EXIT_NAME(void) { (exitfn)(); }

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
    void (*exit)(void);
    size_t symbol_start;
    size_t symbol_count;
    bool syscall_owned;
    size_t refcount;
} module_t;
