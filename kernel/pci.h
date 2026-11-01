#ifndef APTOS_PCI_H
#define APTOS_PCI_H

/* Minimal PCI bus walker for the QEMU hardware we care about (e1000, HDA).
 *
 * Configuration space is reached through the classic CF8/CFC port pair: write
 * the address DWORD (bus | device | function | register | ENABLE) to 0xCF8,
 * then read or write the 32 bit register at 0xCFC.  No ACPI, no MSI, no
 * PCIe - this is 1990s technology and that is all a toy OS needs.
 */

#include "types.h"

#define PCI_VENDOR_INTEL 0x8086

/* class / subclass / prog-if triple we look for */
#define PCI_CLASS_NET    0x02
#define PCI_SUBCL_ETH    0x00
#define PCI_CLASS_MMEDIA 0x04
#define PCI_SUBCL_AUDIO  0x03

#define PCI_CAP_MSI 0x05

typedef struct {
    int      present;
    u8       bus, dev, func;
    u16      vendor, device;
    u8       cls, subclass, prog_if, rev;
    u8       irq_pin, irq_line;
    u16      command;
    u32      bar[6];               /* mem: phys addr; io: addr|1 */
    u32      bar_size[6];
} pci_dev_t;

/* Enumerate the bus (once, from kmain).  Prints one line per device. */
void pci_init(void);

/* First device with this vendor/device id, NULL if absent. */
const pci_dev_t *pci_find(u16 vendor, u16 device);

/* First device of class/subclass, NULL if absent. */
const pci_dev_t *pci_find_class(u8 cls, u8 subclass);

int  pci_count(void);
const pci_dev_t *pci_dev(int index);

u32  pci_read32(const pci_dev_t *d, int reg);
void pci_write32(const pci_dev_t *d, int reg, u32 val);
u16  pci_read16(const pci_dev_t *d, int reg);

#endif
