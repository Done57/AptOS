#ifndef APTOS_ACPI_H
#define APTOS_ACPI_H

#include "types.h"

typedef struct {
    int  present;
    u8   revision;
    char oem_id[7];
    char oem_table_id[9];
    u32  rsdt_addr;
    u64  xsdt_addr;
    int  table_count;
    int  cpus;
    u32  lapic_addr;
    /* MADT 里逐个 CPU 条目：Local APIC ID 与"可启动"标志 */
#define ACPI_MAX_CPUS 8
    u8   cpu_apic_id[ACPI_MAX_CPUS];
    int  cpu_count;
    int  ioapics;
    u16  pm1a_cnt;
    u16  pm1b_cnt;
    u32  dsdt_addr;
    char facs_sig[5];
    char dsdt_sig[5];
} acpi_info_t;

int  acpi_init(void);
void acpi_shutdown(void);
void acpi_reboot(void);
const acpi_info_t *acpi_info(void);
int  acpi_find_table(const char *sig, void **addr, u32 *length);

#endif
