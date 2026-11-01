#include "e1000.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "paging.h"
#include "klog.h"
#include "pci.h"
#include "timer.h"
#include "util.h"

/* ---- register blocks -------------------------------------------------- */
#define REG_CTRL      0x0000
#define REG_STATUS    0x0008
#define REG_EEPROM    0x0014          /* 82540EM "EEPROM read" register */
#define REG_ICR       0x00C0          /* interrupt cause read/clear */
#define REG_IMS       0x00D0          /* interrupt mask set */
#define REG_RCTL      0x0100
#define REG_TCTL      0x0400
#define REG_RDBAL     0x2800
#define REG_RDBAH     0x2804
#define REG_RDLEN     0x2808
#define REG_RDH       0x2810
#define REG_RDT       0x2818
#define REG_RADV      0x282C
#define REG_TDBAL     0x3800
#define REG_TDBAH     0x3804
#define REG_TDLEN     0x3808
#define REG_TDH       0x3810
#define REG_TDT       0x3818
#define REG_TIPG      0x0410

/* CTRL bits */
#define CTRL_SLU      0x40            /* set link up */
#define CTRL_LRST     (1u << 3)       /* link reset */

/* RCTL bits */
#define RCTL_EN       (1u << 1)
#define RCTL_SBP      (1u << 2)
#define RCTL_UPE      (1u << 3)       /* unicast promiscuous */
#define RCTL_MPE      (1u << 4)       /* multicast promiscuous */
#define RCTL_LPE      (1u << 5)
#define RCTL_BAM      (1u << 15)      /* broadcast accept */
#define RCTL_BSIZE_2048 (0u << 16)
#define RCTL_SECRC    (1u << 26)      /* strip CRC */

/* TCTL bits */
#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)

/* ICR / IMS bits we use */
#define ICR_TXDW      (1u << 0)
#define ICR_RXO       (1u << 6)
#define ICR_RXT0      (1u << 7)       /* receiver timer interrupt */

/* descriptor status bits */
#define TXD_STATUS_DD 0x01            /* descriptor done */
#define RXD_STATUS_DD 0x01

/* 82540EM descriptor layout: 16 bytes each. */
/* Legacy TX descriptor (16 bytes) */
typedef struct {
    u32 addr_lo;       /* 64-bit buffer address, low half (we are 32-bit) */
    u32 addr_hi;       /* always 0 for us */
    u16 length;
    u8  cso;
    u8  cmd;           /* EOP | IFCS | RS */
    u8  status;
    u8  errors;
    u16 special;
} e1000_tx_desc_t;

/* Legacy RX descriptor (16 bytes): like TX, status lives at offset 12. */
typedef struct {
    u32 addr_lo;
    u32 addr_hi;
    u16 length;
    u16 csum;
    u8  status;
    u8  errors;
    u16 special;
} e1000_rx_desc_t;

#define TX_DESCS 8
#define RX_DESCS 32

/* legacy TX descriptor CMD bits */
#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08

/* Full compiler barrier: DMA-visible stores must land before the ring
 * tail register is advanced. */
#define dma_barrier() asm volatile("" ::: "memory")

static volatile u8   *mmio;         /* BAR0, memory mapped */
static const pci_dev_t *nic;

static e1000_tx_desc_t *tx_ring;
static e1000_rx_desc_t *rx_ring;
static u8            *tx_buf[TX_DESCS];
static u8            *rx_buf[RX_DESCS];
static u32            tx_tail, rx_tail;
static u8             mac[ETH_MAC_LEN];
static char           chip_name[24];

static inline u32 rd(int reg)
{
    return *(volatile u32 *)(mmio + reg);
}

static inline void wr(int reg, u32 v)
{
    *(volatile u32 *)(mmio + reg) = v;
}

/* The 82540EM reads its "EEPROM" through register 0x0014:
 *   bits 15:8  word address
 *   bit   4    EE_PRES (eeprom present)
 *   bit   1    EE_GNT  (access granted)
 *   bit   0    EE_REQ  (set to start a read)
 *   bits 31:16 data word once the access is done. */
#define EE_REQ  0x1
#define EE_GNT  0x2
#define EE_PRES 0x10

static u16 eeprom_read(int word)
{
    u32 r;

    wr(REG_EEPROM, ((u32)word << 8) | EE_REQ | EE_GNT);
    for (int i = 0; i < 100000; i++) {
        r = rd(REG_EEPROM);
        if (!(r & EE_REQ))                 /* controller cleared it: done */
            break;
    }
    return (u16)((r >> 16) & 0xFFFF);
}

#define REG_RAL 0x5400           /* receive address low (first 4 MAC bytes) */
#define REG_RAH 0x5404           /* receive address high (last 2 + AV bit) */

static void read_mac(void)
{
    u32 lo = rd(REG_RAL);
    u32 hi = rd(REG_RAH);

    mac[0] = (u8)lo;
    mac[1] = (u8)(lo >> 8);
    mac[2] = (u8)(lo >> 16);
    mac[3] = (u8)(lo >> 24);
    mac[4] = (u8)hi;
    mac[5] = (u8)(hi >> 8);

    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
        for (int i = 0; i < 3; i++) {
            u16 w = eeprom_read(i);
            mac[i * 2] = (u8)(w & 0xFF);
            mac[i * 2 + 1] = (u8)(w >> 8);
        }
    }

    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
        /* Nothing from the hardware (QEMU leaves RAL/RAH at 0 until the
         * guest writes them and its EEPROM interface is a stub): program a
         * locally administered address and store it in RAL/RAH so the NIC
         * accepts unicast frames addressed to us. */
        u32 t = (u32)(rdtsc() >> 12);
        mac[0] = 0x52;
        mac[1] = 0x54;
        mac[2] = 0x00;
        mac[3] = 0xA2;
        mac[4] = (u8)(t >> 8);
        mac[5] = (u8)t;
        wr(REG_RAL, ((u32)mac[3] << 24) | ((u32)mac[2] << 16) |
                    ((u32)mac[1] << 8) | mac[0]);
        wr(REG_RAH, ((u32)mac[5] << 8) | mac[4] | (1u << 31));  /* AV */
    }
}

/* Alignment: the hardware wants descriptors 128 byte aligned and buffers at
 * least 16 byte aligned.  kmalloc_a never frees, and this runs once. */
static int alloc_rings(void)
{
    tx_ring = (e1000_tx_desc_t *)kmalloc_a(TX_DESCS * sizeof(e1000_tx_desc_t), 128);
    rx_ring = (e1000_rx_desc_t *)kmalloc_a(RX_DESCS * sizeof(e1000_rx_desc_t), 128);
    if (!tx_ring || !rx_ring)
        return -1;
    memset((void *)tx_ring, 0, TX_DESCS * sizeof(e1000_tx_desc_t));
    memset((void *)rx_ring, 0, RX_DESCS * sizeof(e1000_rx_desc_t));

    for (int i = 0; i < TX_DESCS; i++) {
        tx_buf[i] = (u8 *)kmalloc_a(ETH_MAX_FRAME, 16);
        if (!tx_buf[i])
            return -1;
    }
    for (int i = 0; i < RX_DESCS; i++) {
        rx_buf[i] = (u8 *)kmalloc_a(ETH_MAX_FRAME, 16);
        if (!rx_buf[i])
            return -1;
        rx_ring[i].addr_lo = (u32)(unsigned long)rx_buf[i];
        rx_ring[i].addr_hi = 0;
        rx_ring[i].status = 0;
    }
    return 0;
}

/* QEMU's e1000 raises IRQ 11 (pin A); treat it as a wake-up nudge only.  The
 * actual RX work happens in e1000_recv() from the net poll loop. */
static void e1000_irq(regs_t *r)
{
    (void)r;
    wr(REG_IMS, 0);            /* one shot: mask again until next poll */
}

int e1000_init(void)
{
    const pci_dev_t *d = pci_find_class(PCI_CLASS_NET, PCI_SUBCL_ETH);

    if (!d)
        return -1;
    if ((d->bar[0] & 1) || d->bar[0] == 0)
        return -1;

    nic = d;
    if (paging_map_device(d->bar[0] & ~0xFu, 128 * 1024) != 0) {
        kprintf("[e1000] cannot map BAR0 0x%x\n", d->bar[0] & ~0xFu);
        return -1;
    }
    mmio = (volatile u8 *)(unsigned long)(d->bar[0] & ~0xFu);

    if (alloc_rings() != 0)
        return -1;

    read_mac();

    snprintf(chip_name, sizeof(chip_name), "%04x:%04x",
             (u32)(d->vendor & 0xFFFFu), (u32)(d->device & 0xFFFFu));

    wr(REG_IMS, 0);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_LRST);
    wr(REG_CTRL, CTRL_SLU);

    /* TX ring */
    wr(REG_TDBAL, (u32)(unsigned long)tx_ring);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, TX_DESCS * sizeof(e1000_tx_desc_t));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_tail = 0;
    /* 8254x recommends TIPG = 0x00F0070Fish for gigabit; QEMU is happy with
     * the classic 10 Mbit-ish value below. */
    wr(REG_TIPG, 0x00601209u);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP);

    /* RX ring: 2048 byte buffers, broadcast accepted, strip CRC */
    wr(REG_RDBAL, (u32)(unsigned long)rx_ring);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_DESCS * sizeof(e1000_rx_desc_t));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESCS - 1);  /* tail = last free descriptor */
    rx_tail = 0;                /* next descriptor the NIC will fill */
    wr(REG_RADV, 32);                    /* interrupt after 32B idle... QEMU uses ticks */
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC | RCTL_UPE);

    if (nic->irq_line < 16 && nic->irq_pin)
        irq_register(nic->irq_line, e1000_irq);
    wr(REG_IMS, ICR_RXT0 | ICR_TXDW);

    kprintf("[e1000] %s at %02x:%02x.%d mmio 0x%x irq %d mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            chip_name, d->bus, d->dev, d->func, (u32)(unsigned long)mmio,
            nic->irq_line, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int e1000_present(void)
{
    return mmio != 0;
}

void e1000_mac(u8 out[ETH_MAC_LEN])
{
    memcpy(out, mac, ETH_MAC_LEN);
}

const char *e1000_name(void)
{
    return chip_name;
}

int e1000_send(const void *frame, u32 len)
{
    int head, tries;

    if (!mmio || len == 0 || len > ETH_MAX_FRAME)
        return -1;

    head = (int)rd(REG_TDH);
    if (head == (int)rd(REG_TDT) && tx_ring[tx_tail].status & TXD_STATUS_DD) {
        /* ring full-ish: wait for the NIC to drain (it always does, this is
         * a toy OS with one NIC) */
        tries = 0;
        while ((int)rd(REG_TDT) == head && tries < 100000) {
            for (volatile int i = 0; i < 64; i++)
                ;
            head = (int)rd(REG_TDH);
            tries++;
        }
    }

    memcpy(tx_buf[tx_tail], frame, len);
    tx_ring[tx_tail].addr_lo = (u32)(unsigned long)tx_buf[tx_tail];
    tx_ring[tx_tail].addr_hi = 0;
    tx_ring[tx_tail].length = (u16)len;
    tx_ring[tx_tail].cso = 0;
    tx_ring[tx_tail].cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_ring[tx_tail].status = 0;
    tx_ring[tx_tail].errors = 0;
    tx_ring[tx_tail].special = 0;
    dma_barrier();

    wr(REG_TDT, (u32)((tx_tail + 1) % TX_DESCS));
    tx_tail = (tx_tail + 1) % TX_DESCS;
    return 0;
}

int e1000_recv(void *buf, u32 buflen)
{
    int got = 0;

    if (!mmio)
        return 0;

    for (int n = 0; n < RX_DESCS; n++) {
        int i = rx_tail;
        volatile e1000_rx_desc_t *d = &rx_ring[i];

        if (!(d->status & RXD_STATUS_DD))
            break;

        u16 len = d->length;
        if (len > buflen)
            len = (u16)buflen;
        if (len && buf)
            memcpy(buf, rx_buf[i], len);

        d->status = 0;
        d->addr_lo = (u32)(unsigned long)rx_buf[i];
        rx_tail = (rx_tail + 1) % RX_DESCS;
        dma_barrier();
        /* RDT = index of the descriptor just freed (= rx_tail - 1): the
         * NIC stops at RDT, so this makes exactly one more slot usable. */
        wr(REG_RDT, (u32)((rx_tail + RX_DESCS - 1) % RX_DESCS));

        got = len;
        break;                              /* one frame per poll */
    }

    wr(REG_ICR, 0xFFFFFFFFu);               /* clear all causes */
    return got;
}
