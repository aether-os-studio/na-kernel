#include <acpi/uacpi/namespace.h>
#include <acpi/uacpi/tables.h>
#include <acpi/uacpi/types.h>
#include <acpi/uacpi/uacpi.h>
#include <acpi/uacpi/utilities.h>
#include <drivers/logger.h>
#include <libs/klibc.h>
#include <mod/rust/api.h>

_Static_assert(sizeof(UacpiTable) == sizeof(uacpi_table),
               "ACPI table size must match the Rust ABI");
_Static_assert(offsetof(UacpiTable, ptr) == offsetof(uacpi_table, ptr),
               "ACPI table pointer offset must match the Rust ABI");
_Static_assert(offsetof(UacpiTable, index) == offsetof(uacpi_table, index),
               "ACPI table index offset must match the Rust ABI");

int na_acpi_table_find(const char signature[4], UacpiTable *table) {
    if (!signature || !table)
        return -EINVAL;
    return uacpi_table_find_by_signature(signature, (uacpi_table *)table);
}

void na_acpi_table_release(UacpiTable *table) {
    if (table)
        uacpi_table_unref((uacpi_table *)table);
}

/*
 * ATRM is the AMD-private ACPI method used by discrete GPUs to expose the
 * VBIOS image to the OS. It lives on the VGA device's ACPI node and takes
 * (offset, length) integer arguments, returning a buffer chunk. Linux
 * `amdgpu_atrm_get_bios` reads the image in 4 KiB pages.
 */
#define NA_ATRM_PAGE 4096

typedef struct {
    uacpi_namespace_node *parent;
} na_atrm_find_ctx;

static uacpi_iteration_decision
na_atrm_find_cb(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    na_atrm_find_ctx *ctx = user;
    uacpi_namespace_node *method = NULL;
    if (uacpi_namespace_node_find(node, "ATRM", &method) == UACPI_STATUS_OK &&
        method) {
        ctx->parent = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

int na_acpi_atrm_read(uint8_t *buf, size_t buf_size, size_t *out_len) {
    if (!buf || !out_len)
        return -EINVAL;

    uacpi_namespace_node *root = uacpi_namespace_root();
    if (!root) {
        return -ENOENT;
    }

    na_atrm_find_ctx ctx = {.parent = NULL};
    uacpi_namespace_for_each_child_simple(root, na_atrm_find_cb, &ctx);
    if (!ctx.parent) {
        return -ENOENT;
    }

    const uacpi_char *path =
        uacpi_namespace_node_generate_absolute_path(ctx.parent);
    uacpi_free_absolute_path(path);

    size_t done = 0;
    while (done < buf_size) {
        uacpi_object *arg0 = uacpi_object_create_integer(done);
        uacpi_object *arg1 = uacpi_object_create_integer(NA_ATRM_PAGE);
        if (!arg0 || !arg1) {
            if (arg0)
                uacpi_object_unref(arg0);
            if (arg1)
                uacpi_object_unref(arg1);
            break;
        }

        uacpi_object *args_storage[2] = {arg0, arg1};
        uacpi_object_array args = {.objects = args_storage, .count = 2};
        uacpi_object *ret = NULL;
        uacpi_status st = uacpi_eval(ctx.parent, "ATRM", &args, &ret);
        uacpi_object_unref(arg0);
        uacpi_object_unref(arg1);
        if (st != UACPI_STATUS_OK || !ret) {
            break;
        }

        uacpi_data_view view = {0};
        st = uacpi_object_get_buffer(ret, &view);
        if (st != UACPI_STATUS_OK || view.length == 0) {
            uacpi_object_unref(ret);
            break;
        }

        size_t copy = view.length;
        if (copy > buf_size - done)
            copy = buf_size - done;
        memcpy(buf + done, view.const_bytes, copy);
        done += copy;
        uacpi_object_unref(ret);

        if (copy < NA_ATRM_PAGE)
            break;
    }

    *out_len = done;
    return done > 0 ? 0 : -ENODATA;
}
