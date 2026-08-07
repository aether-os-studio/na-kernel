#pragma once

#include <libs/klibc.h>
#include <libs/fdt/libfdt.h>

#define MAX_FDT_DRIVERS 256
#define MAX_FDT_DEVICES 256

struct fdt_driver;
typedef struct fdt_driver fdt_driver_t;

typedef struct fdt_device {
    char *name;
    int node;
    void *fdt;
    fdt_driver_t *driver;
    void *driver_data;
} fdt_device_t;

struct fdt_driver {
    const char *name;
    const char **compatible;
    int (*probe)(fdt_device_t *dev, const char *compatible);
    void (*remove)(fdt_device_t *dev);
    void (*shutdown)(fdt_device_t *dev);
    int flags;
};

extern fdt_driver_t *fdt_drivers[MAX_FDT_DRIVERS];
extern int fdt_driver_count;
extern fdt_device_t fdt_devices[MAX_FDT_DEVICES];
extern int fdt_device_count;

void fdt_init();
int regist_fdt_driver(fdt_driver_t *driver);
