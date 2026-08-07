#include <boot/boot.h>
#include <drivers/logger.h>
#include <drivers/fdt/fdt.h>
#include <mm/mm.h>

fdt_driver_t *fdt_drivers[MAX_FDT_DRIVERS];
int fdt_driver_count = 0;
fdt_device_t fdt_devices[MAX_FDT_DEVICES];
int fdt_device_count = 0;

int regist_fdt_driver(fdt_driver_t *driver) {
    if (!driver || !driver->compatible)
        return -EINVAL;

    if (fdt_driver_count >= MAX_FDT_DRIVERS)
        return -ENOMEM;

    fdt_drivers[fdt_driver_count++] = driver;
    return 0;
}

static int fdt_match_compatible(const char **driver_compat,
                                const char *device_compat) {
    for (int i = 0; driver_compat[i] != NULL; i++) {
        if (strcmp(driver_compat[i], device_compat) == 0) {
            return i; // 返回匹配的索引
        }
    }
    return -1;
}

extern int fdt_driver_count;

static fdt_driver_t *fdt_find_driver(const void *fdt, int node_offset,
                                     const char **matched_compat) {
    int len;
    const char *compatible = fdt_getprop(fdt, node_offset, "compatible", &len);

    if (!compatible || len <= 0) {
        return NULL;
    }

    /* compatible 可能包含多个以 null 分隔的字符串 */
    const char *compat_str = compatible;
    while (compat_str < compatible + len) {
        /* 遍历所有注册的驱动 */
        for (int i = 0; i < fdt_driver_count; i++) {
            if (fdt_match_compatible(fdt_drivers[i]->compatible, compat_str) >=
                0) {
                if (matched_compat) {
                    *matched_compat = compat_str;
                }
                return fdt_drivers[i];
            }
        }

        /* 移动到下一个 compatible 字符串 */
        compat_str += strlen(compat_str) + 1;
    }

    return NULL;
}

void fdt_init() {
    void *fdt = (void *)boot_get_dtb();
    if (fdt) {
        int node;

        for (node = fdt_next_node(fdt, -1, NULL); node >= 0;
             node = fdt_next_node(fdt, node, NULL)) {

            const char *matched_compat = NULL;
            fdt_driver_t *driver = fdt_find_driver(fdt, node, &matched_compat);
            if (!driver)
                continue;

            char path[128];
            fdt_get_path(fdt, node, path, sizeof(path));

            /* 创建设备实例 */
            fdt_device_t *dev = &fdt_devices[fdt_device_count];
            dev->name = strdup(path);
            dev->node = node;
            dev->fdt = fdt;
            dev->driver = driver;
            dev->driver_data = NULL;

            printk(
                "FDT: Probing device '%s' with driver '%s' (compatible: %s)\n",
                dev->name, driver->name, matched_compat);

            if (driver->probe) {
                int ret = driver->probe(dev, matched_compat);
                if (ret == 0) {
                    fdt_device_count++;
                    printk("FDT: Device '%s' initialized successfully\n", path);
                } else {
                    printk("FDT: Device '%s' probe failed: %d\n", path, ret);
                }
            } else {
                fdt_device_count++;
            }
        }
    }
}
