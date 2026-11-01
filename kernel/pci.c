#include "pci.h"
#include "io.h"
#include "klog.h"
#include "util.h"

#define PCI_ADDR_PORT 0xCF8
#define PCI_DATA_PORT 0xCFC
#define PCI_ENABLE    0x80000000u

#define REG_VENDOR 0x00
#define REG_CLASS  0x08
#define REG_HEADER 0x0C
#define REG_BAR0   0x10
#define REG_IRQ    0x3C

#define HDR_TYPE_MULTIFUNC 0x80

static pci_dev_t devs[16];
static int ndevs;

static u32 cfg_addr(const pci_dev_t *d, int reg)
{
    return PCI_ENABLE | ((u32)d->bus << 16) | ((u32)d->dev << 11) |
           ((u32)d->func << 8) | (u32)(reg & 0xFC);
}

u32 pci_read32(const pci_dev_t *d, int reg)
{
    outl(PCI_ADDR_PORT, cfg_addr(d, reg));
    return inl(PCI_DATA_PORT);
}

void pci_write32(const pci_dev_t *d, int reg, u32 val)
{
    outl(PCI_ADDR_PORT, cfg_addr(d, reg));
    outl(PCI_DATA_PORT, val);
}

u16 pci_read16(const pci_dev_t *d, int reg)
{
    u32 v = pci_read32(d, reg & 0xFC);
    return (u16)((v >> ((reg & 2) * 8)) & 0xFFFF);
}

static int check_present(u8 bus, u8 dev, u8 func)
{
    pci_dev_t probe;
    u32 v;

    memset(&probe, 0, sizeof(probe));
    probe.bus = bus;
    probe.dev = dev;
    probe.func = func;
    v = pci_read32(&probe, REG_VENDOR);
    return (u16)v != 0xFFFF;
}

/* Size a BAR the standard way: write all 1s, read back, mask flag bits. */
static u32 bar_size(const pci_dev_t *d, int reg)
{
    u32 orig, mask;

    orig = pci_read32(d, reg);
    pci_write32(d, reg, 0xFFFFFFFFu);
    mask = pci_read32(d, reg);
    pci_write32(d, reg, orig);

    if (orig & 1)
        return (~mask & 0xFFFFu) + 1;           /* io: low 16 bits */
    return ~(mask & ~0xFu) + 1;                 /* memory */
}

static void add_device(u8 bus, u8 dev, u8 func)
{
    pci_dev_t *d;
    u32 v;

    if (ndevs >= (int)(sizeof(devs) / sizeof(devs[0])))
        return;
    if (!check_present(bus, dev, func))
        return;

    d = &devs[ndevs];
    memset(d, 0, sizeof(*d));
    d->present = 1;
    d->bus = bus;
    d->dev = dev;
    d->func = func;

    v = pci_read32(d, REG_VENDOR);
    d->vendor = (u16)(v & 0xFFFF);
    d->device = (u16)(v >> 16);

    v = pci_read32(d, REG_CLASS);
    d->rev = (u8)(v & 0xFF);
    d->prog_if = (u8)((v >> 8) & 0xFF);
    d->subclass = (u8)((v >> 16) & 0xFF);
    d->cls = (u8)((v >> 24) & 0xFF);

    v = pci_read32(d, REG_IRQ);
    d->irq_line = (u8)(v & 0xFF);            /* 0x3C INT_LINE */
    d->irq_pin  = (u8)((v >> 8) & 0xFF);     /* 0x3D INT_PIN (1=A) */

    /* Enable the device: memory + I/O space and bus mastering.  Firmware
     * leaves the command register at 0, and QEMU honours that - without
     * the master bit every DMA the device attempts is silently dropped
     * (NIC rings never move, HDA reads its BDL as zeros). */
    {
        u32 cmd = pci_read32(d, 0x04) & 0xFFFFu;
        cmd |= 0x7u;                      /* IO | MEM | BUS_MASTER */
        pci_write32(d, 0x04, cmd);
        d->command = (u16)cmd;
    }

    for (int i = 0; i < 6; i++) {
        u32 raw = pci_read32(d, REG_BAR0 + i * 4);
        d->bar[i] = raw;
        if (raw != 0)
            d->bar_size[i] = bar_size(d, REG_BAR0 + i * 4);
        if ((raw & 1) == 0 && (raw & 0x7) == 0x4) {
            i++;                                /* 64 bit BAR: skip upper */
        }
    }

    ndevs++;
}

void pci_init(void)
{
    u8 hdr;

    ndevs = 0;
    memset(devs, 0, sizeof(devs));

    for (u8 dev = 0; dev < 32; dev++) {
        if (!check_present(0, dev, 0))
            continue;
        hdr = (u8)((pci_read32(&(pci_dev_t){ .bus = 0, .dev = dev, .func = 0 },
                              REG_HEADER) >> 16) & 0xFF);
        if (hdr & HDR_TYPE_MULTIFUNC) {
            for (u8 f = 0; f < 8; f++)
                add_device(0, dev, f);
        } else {
            add_device(0, dev, 0);
        }
    }

    /* Close the PIIX3 SMRAM window.  SeaBIOS leaves the SMRAM overlay
     * registered over the whole 4 GiB address space, which swallows every
     * PCI DMA access to RAM (PCI devices see zeros).  Writing 0 to the
     * SMRAM control register (bus 0, dev 1, func 0, reg 0x72) removes the
     * overlay so DMA reaches memory normally. */
    {
        outl(PCI_ADDR_PORT, PCI_ENABLE | (0u << 16) | (1u << 11) |
                            (0u << 8) | 0x70u);
        u32 v = inl(PCI_DATA_PORT);
        v &= 0xFF00FFFFu;                 /* clear the SMRAM byte at 0x72 */
        outl(PCI_ADDR_PORT, PCI_ENABLE | (0u << 16) | (1u << 11) |
                            (0u << 8) | 0x70u);
        outl(PCI_DATA_PORT, v);
    }

    kprintf("[pci] %d device(s) on bus 0\n", ndevs);
    for (int i = 0; i < ndevs; i++) {
        const pci_dev_t *d = &devs[i];
        kprintf("[pci] %02x:%02x.%d %04x:%04x class %02x%02x irq %d%s\n",
                d->bus, d->dev, d->func, d->vendor, d->device,
                d->cls, d->subclass, d->irq_line,
                d->cls == PCI_CLASS_NET      ? "  (network)" :
                d->cls == PCI_CLASS_MMEDIA   ? "  (multimedia)" : "");
    }
}

const pci_dev_t *pci_find(u16 vendor, u16 device)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].present && devs[i].vendor == vendor &&
            devs[i].device == device)
            return &devs[i];
    return NULL;
}

const pci_dev_t *pci_find_class(u8 cls, u8 subclass)
{
    for (int i = 0; i < ndevs; i++)
        if (devs[i].present && devs[i].cls == cls &&
            devs[i].subclass == subclass)
            return &devs[i];
    return NULL;
}

int pci_count(void)
{
    return ndevs;
}

const pci_dev_t *pci_dev(int index)
{
    if (index < 0 || index >= ndevs)
        return NULL;
    return &devs[index];
}
