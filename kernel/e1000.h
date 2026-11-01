#ifndef APTOS_E1000_H
#define APTOS_E1000_H

/* Intel 82540EM / 82573 / QEMU "e1000" NIC - polled TX, interrupt RX.
 *
 * QEMU's default NIC emulates the 82540EM at PCI 00:03.0 with its MMIO BAR
 * somewhere low (typically 0xFEB80000 / IOBAR 0xC000).  Everything the driver
 * needs is memory mapped registers plus a ring of descriptors in RAM.
 */

#include "types.h"

#define ETH_MAC_LEN     6
#define ETH_MTU         1500
#define ETH_MAX_FRAME   (ETH_MTU + 14 + 4)   /* header + FCS */

int  e1000_init(void);                  /* 0 on success, -1 no NIC */

int  e1000_present(void);
void e1000_mac(u8 mac[ETH_MAC_LEN]);
const char *e1000_name(void);

/* Send one ethernet frame (payload includes the 14 byte header).  Returns 0
 * on success, -1 if the frame is too big or the NIC is missing. */
int  e1000_send(const void *frame, u32 len);

/* Poll the RX ring: copy the next received frame into buf (max buflen).
 * Returns the frame length, 0 when nothing is waiting. */
int  e1000_recv(void *buf, u32 buflen);

#endif
