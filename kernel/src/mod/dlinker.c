#include "dlinker.h"
#include <boot/boot.h>
#include <drivers/logger.h>
#include <fs/vfs/vfs.h>
#include <mm/mm.h>

uint64_t kernel_modules_load_offset = 0;

size_t dlfunc_count = 0;

static dlfunc_t __printf = {.name = "printf", .addr = (void *)printk};
static dlfunc_t resolved_func;
static module_symbol_t *loaded_module_symbols = NULL;
static size_t loaded_module_symbol_count = 0;
static size_t loaded_module_symbol_capacity = 0;

static void *find_symbol_address(const char *symbol_name, Elf64_Ehdr *ehdr,
                                 uint64_t offset);
static dlfunc_t *find_kernel_func(const char *name);
static bool get_elf_symbol_table(Elf64_Ehdr *ehdr, Elf64_Sym **symtab,
                                 char **strtab, size_t *num_symbols);
static bool get_elf_section(module_t *module, const char *section_name,
                            const char **data, size_t *size);
static bool elf_symbol_is_exported(const Elf64_Sym *sym);
static bool elf_symbol_can_describe_ip(const Elf64_Sym *sym);
static bool module_symbol_can_describe_ip(const module_symbol_t *sym);
static inline uint64_t dlinker_call_ifunc_resolver(uint64_t resolver_addr);
static bool update_symbol_lookup_result(symbol_lookup_result_t *result,
                                        const char *name,
                                        const char *module_name,
                                        uint64_t symbol_addr,
                                        uint64_t symbol_size, bool is_module,
                                        bool exact_match);

typedef struct {
    size_t *deps;
    size_t dep_count;
    size_t dep_capacity;
    bool scan_ok;
    bool has_missing_dependency;
    bool has_ambiguous_dependency;
} module_plan_t;

typedef struct {
    struct vfs_dir_context ctx;
    module_t **mods;
    size_t *count;
    size_t *capacity;
} module_collect_ctx_t;

static int collect_module_actor(struct vfs_dir_context *ctx, const char *name,
                                int namelen, loff_t pos, uint64_t ino,
                                unsigned type) {
    module_collect_ctx_t *collect = ctx ? ctx->private : NULL;
    module_t *module;
    struct vfs_file *file = NULL;
    struct vfs_open_how how = {.flags = O_RDONLY};
    char path[512];

    (void)pos;
    (void)ino;
    if (!collect || !collect->mods || !collect->count || !collect->capacity)
        return -EINVAL;
    if (type == DT_DIR || namelen <= 0)
        return 0;

    if (*collect->count >= *collect->capacity) {
        size_t new_capacity = *collect->capacity ? *collect->capacity * 2 : 16;
        module_t *new_modules =
            realloc(*collect->mods, new_capacity * sizeof(**collect->mods));
        if (!new_modules)
            return -ENOMEM;
        *collect->mods = new_modules;
        *collect->capacity = new_capacity;
    }

    module = &(*collect->mods)[*collect->count];
    memset(module, 0, sizeof(*module));
    snprintf(module->module_name, sizeof(module->module_name), "%.*s", namelen,
             name);
    snprintf(path, sizeof(path), "/lib/modules/%.*s", namelen, name);
    module->path = strdup(path);
    if (!module->path)
        return -ENOMEM;

    if (vfs_openat(AT_FDCWD, path, &how, &file, true) < 0 || !file) {
        free(module->path);
        module->path = NULL;
        return 0;
    }

    module->size = file->f_inode ? file->f_inode->i_size : 0;
    module->data = alloc_frames_bytes(module->size);
    if (!module->data) {
        vfs_close_file(file);
        free(module->path);
        module->path = NULL;
        return -ENOMEM;
    }

    loff_t file_pos = 0;
    vfs_read_kernel_file(file, module->data, module->size, &file_pos);
    vfs_close_file(file);
    (*collect->count)++;
    return 0;
}

static void load_segment(Elf64_Phdr *phdr, void *elf, uint64_t offset) {
    size_t hi = PADDING_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE) + offset;
    size_t lo = PADDING_DOWN(phdr->p_vaddr, PAGE_SIZE) + offset;

    uint64_t flags = PT_FLAG_R | PT_FLAG_W;

    map_page_range(get_current_page_dir(false), lo, (uint64_t)-1, hi - lo,
                   flags | ((phdr->p_flags & PF_X) ? PT_FLAG_X : 0));

    uint64_t p_vaddr = (uint64_t)phdr->p_vaddr + offset;
    uint64_t p_filesz = (uint64_t)phdr->p_filesz;
    uint64_t p_memsz = (uint64_t)phdr->p_memsz;

    memcpy((void *)p_vaddr, (const uint8_t *)elf + phdr->p_offset, p_filesz);

    if (p_memsz > p_filesz) {
        memset((void *)(p_vaddr + p_filesz), 0, p_memsz - p_filesz);
    }

    dma_sync_cpu_to_device((void *)p_vaddr, p_memsz);
}

static bool mmap_phdr_segment(Elf64_Ehdr *ehdr, Elf64_Phdr *phdrs,
                              uint64_t offset, uint64_t *load_size) {
    size_t i = 0;
    while (i < ehdr->e_phnum && phdrs[i].p_type != PT_LOAD) {
        i++;
    }

    if (i == ehdr->e_phnum) {
        return false;
    }

    uint64_t load_min = UINT64_MAX;
    uint64_t load_max = 0;

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }

        load_segment(&phdrs[i], (void *)ehdr, offset);

        if (phdrs[i].p_vaddr + offset + phdrs[i].p_memsz > load_max) {
            load_max = PADDING_UP(phdrs[i].p_vaddr + offset + phdrs[i].p_memsz,
                                  PAGE_SIZE);
        }
        if (phdrs[i].p_vaddr + offset < load_min) {
            load_min = PADDING_DOWN(phdrs[i].p_vaddr + offset, PAGE_SIZE);
        }
    }

    if (load_size) {
        *load_size = load_max - offset;
    }

    return true;
}

static bool kernel_symbol_table_available() {
    return (uintptr_t)kallsyms_symbols != 0 && (uintptr_t)&kallsyms_num != 0 &&
           kallsyms_num != 0;
}

static void *lookup_kernel_symbol_by_name(const char *name) {
    if (name == NULL || !kernel_symbol_table_available()) {
        return NULL;
    }

    for (size_t i = 0; i < (size_t)kallsyms_num; i++) {
        const kernel_builtin_symbol_t *sym = &kallsyms_symbols[i];
        if (!sym->exported || sym->name == NULL) {
            continue;
        }

        if (strcmp(sym->name, name) == 0) {
            return (void *)(uintptr_t)sym->addr;
        }
    }

    return NULL;
}

static module_symbol_t *find_module_symbol(const char *module_name,
                                           const char *name) {
    if (module_name == NULL || name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < loaded_module_symbol_count; i++) {
        if (!loaded_module_symbols[i].exported) {
            continue;
        }

        if (strcmp(loaded_module_symbols[i].module_name, module_name) == 0 &&
            strcmp(loaded_module_symbols[i].name, name) == 0) {
            return &loaded_module_symbols[i];
        }
    }

    return NULL;
}

static bool ensure_module_symbol_capacity(size_t wanted) {
    if (wanted <= loaded_module_symbol_capacity) {
        return true;
    }

    size_t new_capacity =
        loaded_module_symbol_capacity ? loaded_module_symbol_capacity * 2 : 128;
    while (new_capacity < wanted) {
        new_capacity *= 2;
    }

    module_symbol_t *new_symbols =
        realloc(loaded_module_symbols, new_capacity * sizeof(*new_symbols));
    if (new_symbols == NULL) {
        return false;
    }

    loaded_module_symbols = new_symbols;
    loaded_module_symbol_capacity = new_capacity;
    return true;
}

static bool register_module_symbol(const char *module_name, const char *name,
                                   uint64_t addr, uint64_t size, uint8_t type,
                                   bool exported) {
    if (name == NULL || *name == '\0') {
        return true;
    }

    if (exported && !strcmp(name, "dlmain")) {
        return true;
    }

    if (exported && find_module_symbol(module_name, name) != NULL) {
        serial_fprintk("Skipping duplicate module symbol %s from %s\n", name,
                       module_name);
        return true;
    }

    if (!ensure_module_symbol_capacity(loaded_module_symbol_count + 1)) {
        serial_fprintk("Cannot grow module symbol registry for %s\n", name);
        return false;
    }

    char *dup_name = strdup(name);
    if (dup_name == NULL) {
        serial_fprintk("Cannot duplicate module symbol name %s\n", name);
        return false;
    }

    char *dup_module_name = strdup(module_name ? module_name : "<module>");
    if (dup_module_name == NULL) {
        serial_fprintk("Cannot duplicate module name %s for symbol %s\n",
                       module_name ? module_name : "<module>", name);
        free(dup_name);
        return false;
    }

    loaded_module_symbols[loaded_module_symbol_count].module_name =
        dup_module_name;
    loaded_module_symbols[loaded_module_symbol_count].name = dup_name;
    loaded_module_symbols[loaded_module_symbol_count].addr = addr;
    loaded_module_symbols[loaded_module_symbol_count].size = size;
    loaded_module_symbols[loaded_module_symbol_count].type = type;
    loaded_module_symbols[loaded_module_symbol_count].exported = exported;
    loaded_module_symbol_count++;
    return true;
}

static bool get_elf_symbol_table(Elf64_Ehdr *ehdr, Elf64_Sym **symtab,
                                 char **strtab, size_t *num_symbols) {
    if (ehdr == NULL || symtab == NULL || strtab == NULL ||
        num_symbols == NULL) {
        return false;
    }

    Elf64_Shdr *shdrs = (Elf64_Shdr *)((char *)ehdr + ehdr->e_shoff);
    Elf64_Shdr *candidate = NULL;

    for (size_t i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            candidate = &shdrs[i];
            break;
        }
    }

    if (candidate == NULL) {
        for (size_t i = 0; i < ehdr->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_DYNSYM) {
                candidate = &shdrs[i];
                break;
            }
        }
    }

    if (candidate == NULL || candidate->sh_link >= ehdr->e_shnum) {
        return false;
    }

    *symtab = (Elf64_Sym *)((char *)ehdr + candidate->sh_offset);
    *strtab = (char *)ehdr + shdrs[candidate->sh_link].sh_offset;
    *num_symbols = candidate->sh_size / sizeof(Elf64_Sym);
    return true;
}

static bool get_elf_section(module_t *module, const char *section_name,
                            const char **data, size_t *size) {
    if (module == NULL || section_name == NULL || data == NULL ||
        size == NULL || module->data == NULL ||
        module->size < sizeof(Elf64_Ehdr)) {
        return false;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)module->data;
    if (ehdr->e_shentsize != sizeof(Elf64_Shdr) || ehdr->e_shnum == 0 ||
        ehdr->e_shstrndx >= ehdr->e_shnum || ehdr->e_shoff > module->size ||
        (size_t)ehdr->e_shnum >
            (module->size - ehdr->e_shoff) / sizeof(Elf64_Shdr)) {
        return false;
    }

    Elf64_Shdr *shdrs = (Elf64_Shdr *)(module->data + ehdr->e_shoff);
    Elf64_Shdr *shstr = &shdrs[ehdr->e_shstrndx];
    if (shstr->sh_offset > module->size ||
        shstr->sh_size > module->size - shstr->sh_offset) {
        return false;
    }

    const char *section_names = (const char *)module->data + shstr->sh_offset;
    for (size_t i = 0; i < ehdr->e_shnum; i++) {
        Elf64_Shdr *section = &shdrs[i];
        if (section->sh_name >= shstr->sh_size) {
            return false;
        }

        const char *name = section_names + section->sh_name;
        size_t remaining = shstr->sh_size - section->sh_name;
        if (strnlen(name, remaining) == remaining) {
            return false;
        }

        if (strcmp(name, section_name) != 0) {
            continue;
        }

        if (section->sh_offset > module->size ||
            section->sh_size > module->size - section->sh_offset) {
            return false;
        }

        *data = (const char *)module->data + section->sh_offset;
        *size = section->sh_size;
        return true;
    }

    return false;
}

static bool ensure_index_list_capacity(size_t **items, size_t *capacity,
                                       size_t wanted) {
    if (wanted <= *capacity) {
        return true;
    }

    size_t new_capacity = *capacity ? *capacity * 2 : 16;
    while (new_capacity < wanted) {
        new_capacity *= 2;
    }

    size_t *new_items =
        realloc(items ? *items : NULL, new_capacity * sizeof(*new_items));
    if (new_items == NULL) {
        return false;
    }

    *items = new_items;
    *capacity = new_capacity;
    return true;
}

static bool append_module_dependency(module_t *module, const char *name) {
    if (module == NULL || name == NULL || *name == '\0') {
        return false;
    }

    for (size_t i = 0; i < module->dependency_count; i++) {
        if (strcmp(module->dependencies[i], name) == 0) {
            return true;
        }
    }

    char **dependencies =
        realloc(module->dependencies,
                (module->dependency_count + 1) * sizeof(*dependencies));
    if (dependencies == NULL) {
        return false;
    }

    module->dependencies = dependencies;
    module->dependencies[module->dependency_count] = strdup(name);
    if (module->dependencies[module->dependency_count] == NULL) {
        return false;
    }

    module->dependency_count++;
    return true;
}

static bool index_list_contains(const size_t *items, size_t count,
                                size_t value) {
    for (size_t i = 0; i < count; i++) {
        if (items[i] == value) {
            return true;
        }
    }

    return false;
}

static bool append_unique_index(size_t **items, size_t *count, size_t *capacity,
                                size_t value) {
    if (index_list_contains(*items, *count, value)) {
        return true;
    }

    if (!ensure_index_list_capacity(items, capacity, *count + 1)) {
        return false;
    }

    (*items)[*count] = value;
    (*count)++;
    return true;
}

static bool elf_symbol_is_exported(const Elf64_Sym *sym) {
    if (sym == NULL || sym->st_name == 0 || sym->st_shndx == SHN_UNDEF) {
        return false;
    }

    uint8_t bind = ELF64_ST_BIND(sym->st_info);
    if (bind != STB_GLOBAL && bind != STB_WEAK) {
        return false;
    }

    if (ELF64_ST_VISIBILITY(sym->st_other) != STV_DEFAULT) {
        return false;
    }

    uint8_t type = ELF64_ST_TYPE(sym->st_info);
    switch (type) {
    case STT_NOTYPE:
    case STT_OBJECT:
    case STT_FUNC:
    case STT_COMMON:
        return true;
    default:
        return false;
    }
}

static bool elf_symbol_can_describe_ip(const Elf64_Sym *sym) {
    if (sym == NULL || sym->st_name == 0 || sym->st_shndx == SHN_UNDEF ||
        sym->st_value == 0) {
        return false;
    }

    uint8_t type = ELF64_ST_TYPE(sym->st_info);
    return type == STT_FUNC || type == STT_NOTYPE;
}

static bool module_symbol_can_describe_ip(const module_symbol_t *sym) {
    if (sym == NULL || sym->name == NULL || sym->addr == 0) {
        return false;
    }

    return sym->type == STT_FUNC || sym->type == STT_NOTYPE;
}

static bool update_symbol_lookup_result(symbol_lookup_result_t *result,
                                        const char *name,
                                        const char *module_name,
                                        uint64_t symbol_addr,
                                        uint64_t symbol_size, bool is_module,
                                        bool exact_match) {
    if (result == NULL || name == NULL || *name == '\0') {
        return false;
    }

    if (result->name != NULL) {
        if (exact_match != result->exact_match) {
            if (!exact_match) {
                return false;
            }
        } else if (symbol_addr < result->symbol_addr) {
            return false;
        } else if (symbol_addr == result->symbol_addr) {
            if (exact_match) {
                if (result->symbol_size != 0 &&
                    (symbol_size == 0 || symbol_size >= result->symbol_size)) {
                    return false;
                }
            } else if (symbol_size <= result->symbol_size) {
                return false;
            }
        }
    }

    result->name = name;
    result->module_name = module_name;
    result->symbol_addr = symbol_addr;
    result->symbol_size = symbol_size;
    result->offset = 0;
    result->is_module = is_module;
    result->exact_match = exact_match;
    return true;
}

static void register_module_symbols(module_t *module, Elf64_Ehdr *ehdr,
                                    uint64_t offset) {
    Elf64_Sym *symtab = NULL;
    char *strtab = NULL;
    size_t num_symbols = 0;

    if (!get_elf_symbol_table(ehdr, &symtab, &strtab, &num_symbols)) {
        serial_fprintk("Cannot find symbol table in module %s\n",
                       module->module_name);
        return;
    }

    for (size_t i = 0; i < num_symbols; i++) {
        Elf64_Sym *sym = &symtab[i];
        bool exported = elf_symbol_is_exported(sym);
        if (!exported && !elf_symbol_can_describe_ip(sym)) {
            continue;
        }

        uint64_t addr =
            sym->st_shndx == SHN_ABS ? sym->st_value : offset + sym->st_value;
        register_module_symbol(module->module_name, &strtab[sym->st_name], addr,
                               sym->st_size, ELF64_ST_TYPE(sym->st_info),
                               exported);
    }
}

static void free_module_plan(module_plan_t *plan) {
    if (plan == NULL) {
        return;
    }

    free(plan->deps);
    memset(plan, 0, sizeof(*plan));
}

static bool scan_module_metadata(module_t *module, module_plan_t *plan) {
    if (module == NULL || plan == NULL) {
        return false;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)module->data;
    if (!arch_check_elf(ehdr)) {
        serial_fprintk("Module %s is not a valid ELF file.\n",
                       module->module_name);
        return false;
    }

    if (ehdr->e_type != ET_DYN) {
        serial_fprintk("Module %s is not a dynamic ELF file.\n",
                       module->module_name);
        return false;
    }

    const char *module_name = NULL;
    size_t module_name_size = 0;
    if (!get_elf_section(module, MODULE_NAME_SECTION, &module_name,
                         &module_name_size) ||
        module_name_size < 2) {
        serial_fprintk("Module %s has no build-generated name metadata\n",
                       module->path);
        return false;
    }

    size_t name_length = strnlen(module_name, module_name_size);
    if (name_length == 0 || name_length + 1 != module_name_size ||
        name_length >= sizeof(module->module_name)) {
        serial_fprintk("Module %s has invalid name metadata\n", module->path);
        return false;
    }
    memcpy(module->module_name, module_name, name_length + 1);

    const char *dependencies = NULL;
    size_t dependencies_size = 0;
    if (get_elf_section(module, MODULE_DEPS_SECTION, &dependencies,
                        &dependencies_size)) {
        size_t cursor = 0;
        while (cursor < dependencies_size) {
            const char *dependency = dependencies + cursor;
            size_t remaining = dependencies_size - cursor;
            size_t dependency_length = strnlen(dependency, remaining);
            if (dependency_length == 0 || dependency_length == remaining) {
                serial_fprintk("Module %s has invalid dependency metadata\n",
                               module->module_name);
                return false;
            }

            if (!append_module_dependency(module, dependency)) {
                serial_fprintk("Cannot record dependency %s -> %s\n",
                               module->module_name, dependency);
                return false;
            }
            cursor += dependency_length + 1;
        }
    }

    plan->scan_ok = true;
    return true;
}

static size_t count_named_modules(module_t *modules, module_plan_t *plans,
                                  size_t module_count, const char *module_name,
                                  size_t requester_index,
                                  size_t *provider_index) {
    size_t matches = 0;

    for (size_t i = 0; i < module_count; i++) {
        if (i == requester_index || !plans[i].scan_ok) {
            continue;
        }

        if (strcmp(modules[i].module_name, module_name) != 0) {
            continue;
        }

        if (provider_index != NULL) {
            *provider_index = i;
        }
        matches++;
    }

    return matches;
}

static void resolve_module_dependencies(module_t *modules, module_plan_t *plans,
                                        size_t module_count) {
    for (size_t i = 0; i < module_count; i++) {
        if (!plans[i].scan_ok) {
            continue;
        }

        for (size_t j = i + 1; j < module_count; j++) {
            if (plans[j].scan_ok &&
                strcmp(modules[i].module_name, modules[j].module_name) == 0) {
                serial_fprintk("Duplicate module name %s in %s and %s\n",
                               modules[i].module_name, modules[i].path,
                               modules[j].path);
                plans[i].has_ambiguous_dependency = true;
                plans[j].has_ambiguous_dependency = true;
            }
        }
    }

    for (size_t i = 0; i < module_count; i++) {
        if (!plans[i].scan_ok) {
            continue;
        }

        for (size_t j = 0; j < modules[i].dependency_count; j++) {
            const char *dependency_name = modules[i].dependencies[j];
            size_t provider_index = 0;
            size_t provider_count =
                count_named_modules(modules, plans, module_count,
                                    dependency_name, i, &provider_index);

            if (provider_count == 1) {
                if (!append_unique_index(&plans[i].deps, &plans[i].dep_count,
                                         &plans[i].dep_capacity,
                                         provider_index)) {
                    serial_fprintk("Cannot record dependency %s -> %s\n",
                                   modules[i].module_name, dependency_name);
                    plans[i].has_missing_dependency = true;
                }
                continue;
            }

            if (provider_count == 0) {
                if (strcmp(modules[i].module_name, dependency_name) == 0) {
                    serial_fprintk(
                        "Module %s declares itself as a dependency\n",
                        modules[i].module_name);
                } else {
                    serial_fprintk("Module %s misses dependency %s\n",
                                   modules[i].module_name, dependency_name);
                }
                plans[i].has_missing_dependency = true;
                continue;
            }

            serial_fprintk("Module %s has ambiguous dependency %s\n",
                           modules[i].module_name, dependency_name);
            plans[i].has_ambiguous_dependency = true;
        }
    }
}

static bool module_dependencies_ready(const module_plan_t *plan,
                                      const bool *loaded_flags) {
    if (plan == NULL || loaded_flags == NULL) {
        return false;
    }

    if (!plan->scan_ok || plan->has_missing_dependency ||
        plan->has_ambiguous_dependency) {
        return false;
    }

    for (size_t i = 0; i < plan->dep_count; i++) {
        if (!loaded_flags[plan->deps[i]]) {
            return false;
        }
    }

    return true;
}

static dlfunc_t *find_scoped_func(module_t *module, const char *name) {
    dlfunc_t *kernel_func = find_kernel_func(name);
    if (kernel_func != NULL) {
        return kernel_func;
    }

    module_symbol_t *match = NULL;
    for (size_t i = 0; i < module->dependency_count; i++) {
        module_symbol_t *candidate =
            find_module_symbol(module->dependencies[i], name);
        if (candidate == NULL) {
            continue;
        }

        if (match != NULL) {
            serial_fprintk(
                "Module %s has ambiguous symbol %s in dependencies %s and %s\n",
                module->module_name, name, match->module_name,
                candidate->module_name);
            return NULL;
        }
        match = candidate;
    }

    if (match == NULL) {
        return NULL;
    }

    resolved_func.name = match->name;
    resolved_func.addr = (void *)match->addr;
    return &resolved_func;
}

static bool resolve_symbol_address(module_t *module, Elf64_Sym *symtab,
                                   char *strtab, uint32_t sym_idx,
                                   uint64_t offset, uint64_t *addr) {
    if (symtab == NULL || strtab == NULL || addr == NULL) {
        return false;
    }

    Elf64_Sym *sym = &symtab[sym_idx];
    char *sym_name = &strtab[sym->st_name];

    if (sym->st_shndx == SHN_UNDEF) {
        dlfunc_t *func = find_scoped_func(module, sym_name);
        if (func != NULL) {
            *addr = (uint64_t)func->addr;
            return true;
        }
        serial_fprintk("Module %s cannot resolve symbol %s from the kernel or "
                       "its declared dependencies\n",
                       module->module_name, sym_name);
        return false;
    }

    if (sym->st_shndx == SHN_ABS) {
        *addr = sym->st_value;
        return true;
    }

    *addr = offset + sym->st_value;
    return true;
}

static bool handle_relocations(module_t *module, Elf64_Rela *rela_start,
                               Elf64_Sym *symtab, char *strtab,
                               size_t jmprel_sz, uint64_t offset) {
    if (!rela_start || jmprel_sz == 0) {
        return true;
    }

    size_t rela_count = jmprel_sz / sizeof(Elf64_Rela);

    for (size_t i = 0; i < rela_count; i++) {
        Elf64_Rela *rela = &rela_start[i];
        uint64_t *target_addr = (uint64_t *)(rela->r_offset + offset);
        uint32_t sym_idx = ELF64_R_SYM(rela->r_info);
        uint32_t type = ELF64_R_TYPE(rela->r_info);

        Elf64_Sym *sym = &symtab[sym_idx];
        char *sym_name = &strtab[sym->st_name];

#if defined(__x86_64__)
        if (type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr;
        } else if (type == R_X86_64_RELATIVE) {
            *target_addr = offset + rela->r_addend;
        } else if (type == R_X86_64_64) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr + rela->r_addend;
        }
#elif defined(__aarch64__)
        if (type == R_AARCH64_JUMP_SLOT || type == R_AARCH64_GLOB_DAT) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr + rela->r_addend;
        } else if (type == R_AARCH64_RELATIVE) {
            *target_addr = offset + rela->r_addend;
        } else if (type == R_AARCH64_ABS64) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr + rela->r_addend;
        } else if (type == R_AARCH64_IRELATIVE) {
            uint64_t resolver_addr = offset + rela->r_addend;
            *target_addr = dlinker_call_ifunc_resolver(resolver_addr);
        } else if (type != R_AARCH64_NONE) {
            serial_fprintk(
                "Unsupported AArch64 relocation type %u for %s at %p\n", type,
                sym_name, target_addr);
            return false;
        }
#elif defined(__riscv) && (__riscv_xlen == 64)
        if (type == R_RISCV_JUMP_SLOT) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr;
        } else if (type == R_RISCV_RELATIVE) {
            *target_addr = offset + rela->r_addend;
        } else if (type == R_RISCV_64) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr + rela->r_addend;
        }
#elif defined(__loongarch64__)
        if (type == R_LARCH_JUMP_SLOT) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr;
        } else if (type == R_LARCH_RELATIVE) {
            *target_addr = offset + rela->r_addend;
        } else if (type == R_LARCH_64) {
            uint64_t sym_addr = 0;
            if (!resolve_symbol_address(module, symtab, strtab, sym_idx, offset,
                                        &sym_addr)) {
                serial_fprintk("Failed relocating %s at %p\n", sym_name,
                               target_addr);
                return false;
            }

            *target_addr = sym_addr + rela->r_addend;
        } else if (type != R_LARCH_NONE) {
            serial_fprintk(
                "Unsupported LoongArch relocation type %u for %s at %p\n", type,
                sym_name, target_addr);
            return false;
        }
#endif
    }

    return true;
}

static inline uint64_t dlinker_call_ifunc_resolver(uint64_t resolver_addr) {
    uint64_t (*resolver)(void) = (uint64_t (*)(void))resolver_addr;
    return resolver();
}

static void *find_symbol_address(const char *symbol_name, Elf64_Ehdr *ehdr,
                                 uint64_t offset) {
    if (symbol_name == NULL || ehdr == NULL) {
        return NULL;
    }

    Elf64_Sym *symtab = NULL;
    char *strtab = NULL;
    size_t num_symbols = 0;

    if (!get_elf_symbol_table(ehdr, &symtab, &strtab, &num_symbols)) {
        serial_fprintk("Cannot find symbol table in ELF file.\n");
        return NULL;
    }

    for (size_t i = 0; i < num_symbols; i++) {
        Elf64_Sym *sym = &symtab[i];
        char *sym_name = &strtab[sym->st_name];

        if (strcmp(symbol_name, sym_name) != 0) {
            continue;
        }

        if (sym->st_shndx == SHN_UNDEF) {
            serial_fprintk("Symbol %s is undefined.\n", sym_name);
            return NULL;
        }

        return (void *)(offset + sym->st_value);
    }

    serial_fprintk("Cannot find symbol %s in ELF file.\n", symbol_name);
    return NULL;
}

static dlinit_t load_dynamic(module_t *module, Elf64_Phdr *phdrs,
                             Elf64_Ehdr *ehdr, uint64_t offset) {
    Elf64_Dyn *dyn_entry = NULL;
    for (size_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_entry = (Elf64_Dyn *)(phdrs[i].p_vaddr + offset);
            break;
        }
    }
    if (dyn_entry == NULL) {
        serial_fprintk("Dynamic section not found.\n");
        return NULL;
    }

    Elf64_Sym *symtab = NULL;
    char *strtab = NULL;
    Elf64_Rela *rel = NULL;
    Elf64_Rela *jmprel = NULL;
    size_t relsz = 0;
    size_t jmprel_sz = 0;

    while (dyn_entry->d_tag != DT_NULL) {
        switch (dyn_entry->d_tag) {
        case DT_SYMTAB:
            symtab = (Elf64_Sym *)(dyn_entry->d_un.d_ptr + offset);
            break;
        case DT_STRTAB:
            strtab = (char *)(dyn_entry->d_un.d_ptr + offset);
            break;
        case DT_RELA:
            rel = (Elf64_Rela *)(dyn_entry->d_un.d_ptr + offset);
            break;
        case DT_RELASZ:
            relsz = dyn_entry->d_un.d_val;
            break;
        case DT_JMPREL:
            jmprel = (Elf64_Rela *)(dyn_entry->d_un.d_ptr + offset);
            break;
        case DT_PLTRELSZ:
            jmprel_sz = dyn_entry->d_un.d_val;
            break;
        }
        dyn_entry++;
    }

    if (!handle_relocations(module, rel, symtab, strtab, relsz, offset)) {
        serial_fprintk("Failed to handle RELA relocations.\n");
        return NULL;
    }

    if (!handle_relocations(module, jmprel, symtab, strtab, jmprel_sz,
                            offset)) {
        serial_fprintk("Failed to handle PLT relocations.\n");
        return NULL;
    }

    void *entry = find_symbol_address("dlmain", ehdr, offset);
    if (entry == NULL) {
        serial_fprintk("Cannot find dlmain symbol.\n");
        return NULL;
    }

    return (dlinit_t)entry;
}

bool dlinker_load(module_t *module) {
    if (module == NULL || module->is_use) {
        return module != NULL;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)module->data;
    if (!arch_check_elf(ehdr)) {
        serial_fprintk("Module %s is not a valid ELF file.\n",
                       module->module_name);
        return false;
    }

    if (ehdr->e_type != ET_DYN) {
        serial_fprintk("Module %s is not a dynamic ELF file.\n",
                       module->module_name);
        return false;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);

    if (!module->mapped) {
        uint64_t load_base =
            KERNEL_MODULES_SPACE_START + kernel_modules_load_offset;

        if (!mmap_phdr_segment(ehdr, phdrs, load_base, &module->load_size)) {
            serial_fprintk("Cannot map module %s\n", module->module_name);
            return false;
        }

        module->load_base = load_base;
        module->mapped = true;
        kernel_modules_load_offset += PADDING_UP(module->load_size, PAGE_SIZE);
    }

    dlinit_t dlinit = load_dynamic(module, phdrs, ehdr, module->load_base);
    if (dlinit == NULL) {
        return false;
    }

    register_module_symbols(module, ehdr, module->load_base);

    serial_fprintk("Loaded module %s at %#018lx\n", module->module_name,
                   module->load_base);

    dlinit();
    module->is_use = true;
    return true;
}

static dlfunc_t *find_kernel_func(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    if (strcmp(name, __printf.name) == 0) {
        return &__printf;
    }

    void *kernel_symbol = lookup_kernel_symbol_by_name(name);
    if (kernel_symbol != NULL) {
        resolved_func.name = (char *)name;
        resolved_func.addr = kernel_symbol;
        return &resolved_func;
    }

    return NULL;
}

static bool lookup_module_symbol_by_addr(uint64_t addr,
                                         symbol_lookup_result_t *result) {
    bool found = false;

    for (size_t i = 0; i < loaded_module_symbol_count; i++) {
        module_symbol_t *sym = &loaded_module_symbols[i];
        if (!module_symbol_can_describe_ip(sym)) {
            continue;
        }

        if (addr < sym->addr) {
            continue;
        }

        bool exact_match = sym->size != 0 && addr - sym->addr < sym->size;
        if (update_symbol_lookup_result(result, sym->name, sym->module_name,
                                        sym->addr, sym->size, true,
                                        exact_match)) {
            found = true;
        }
    }

    return found;
}

static bool lookup_kernel_symbol_by_addr(uint64_t addr,
                                         symbol_lookup_result_t *result) {
    if (!kernel_symbol_table_available()) {
        return false;
    }

    bool found = false;

    for (size_t i = 0; i < (size_t)kallsyms_num; i++) {
        const kernel_builtin_symbol_t *sym = &kallsyms_symbols[i];
        if (!sym->can_describe_ip || sym->name == NULL || addr < sym->addr) {
            continue;
        }

        if (update_symbol_lookup_result(result, sym->name, NULL, sym->addr, 0,
                                        false, false)) {
            found = true;
        }
    }

    return found;
}

bool dlinker_lookup_symbol_by_addr(uint64_t addr,
                                   symbol_lookup_result_t *result) {
    if (result == NULL) {
        return false;
    }

    memset(result, 0, sizeof(*result));

    bool found_module = lookup_module_symbol_by_addr(addr, result);
    bool found_kernel = lookup_kernel_symbol_by_addr(addr, result);

    if (result->name != NULL) {
        result->offset = addr - result->symbol_addr;
    }

    return found_module || found_kernel;
}

void dlinker_init() {
    struct vfs_file *modules_root = NULL;
    struct vfs_open_how dir_how = {
        .flags = O_RDONLY | O_DIRECTORY,
    };
    if (vfs_openat(AT_FDCWD, "/lib/modules", &dir_how, &modules_root, true) <
            0 ||
        !modules_root) {
        return;
    }

    module_t *modules = NULL;
    size_t module_count = 0;
    size_t module_capacity = 0;
    module_collect_ctx_t collect = {0};
    collect.ctx.actor = collect_module_actor;
    collect.ctx.private = &collect;
    collect.mods = &modules;
    collect.count = &module_count;
    collect.capacity = &module_capacity;
    vfs_iterate_dir(modules_root, &collect.ctx);

    module_plan_t *plans = calloc(module_count, sizeof(*plans));
    bool *loaded_flags = calloc(module_count, sizeof(*loaded_flags));
    bool *failed_flags = calloc(module_count, sizeof(*failed_flags));
    if (plans == NULL || loaded_flags == NULL || failed_flags == NULL) {
        serial_fprintk("Cannot allocate module dependency planner\n");
        free(plans);
        free(loaded_flags);
        free(failed_flags);
        plans = NULL;
        loaded_flags = NULL;
        failed_flags = NULL;
    }

    if (plans != NULL && loaded_flags != NULL && failed_flags != NULL) {
        for (size_t i = 0; i < module_count; i++) {
            if (!scan_module_metadata(&modules[i], &plans[i])) {
                serial_fprintk("Skipping metadata scan for module %s\n",
                               modules[i].path);
            }
        }

        resolve_module_dependencies(modules, plans, module_count);

        size_t finished_count = 0;
        bool progress = true;

        while (finished_count < module_count && progress) {
            progress = false;

            for (size_t i = 0; i < module_count; i++) {
                if (loaded_flags[i] || failed_flags[i] ||
                    !module_dependencies_ready(&plans[i], loaded_flags)) {
                    continue;
                }

                if (dlinker_load(&modules[i])) {
                    loaded_flags[i] = true;
                } else {
                    serial_fprintk(
                        "Module %s failed after dependency resolution\n",
                        modules[i].module_name);
                    failed_flags[i] = true;
                }
                finished_count++;
                progress = true;
            }
        }

        for (size_t i = 0; i < module_count; i++) {
            if (modules[i].is_use) {
                continue;
            }

            if (!plans[i].scan_ok) {
                serial_fprintk("Module %s was not loaded: scan failed\n",
                               modules[i].module_name);
                continue;
            }

            if (plans[i].has_missing_dependency) {
                serial_fprintk(
                    "Module %s was not loaded: missing declared dependency\n",
                    modules[i].module_name);
                continue;
            }

            if (plans[i].has_ambiguous_dependency) {
                serial_fprintk("Module %s was not loaded: ambiguous module "
                               "identity or dependency\n",
                               modules[i].module_name);
                continue;
            }

            serial_fprintk(
                "Module %s was not loaded: dependency cycle or init failure\n",
                modules[i].module_name);
        }
    }

    for (size_t i = 0; i < module_count; i++) {
        if (plans != NULL) {
            free_module_plan(&plans[i]);
        }

        if (modules[i].data != NULL) {
            free_frames_bytes(modules[i].data, modules[i].size);
        }

        for (size_t j = 0; j < modules[i].dependency_count; j++) {
            free(modules[i].dependencies[j]);
        }
        free(modules[i].dependencies);
        free(modules[i].path);
    }

    free(plans);
    free(loaded_flags);
    free(failed_flags);
    free(modules);
    vfs_close_file(modules_root);
}
