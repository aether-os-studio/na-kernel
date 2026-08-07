#include <acpi/uacpi/tables.h>
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
