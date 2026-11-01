#ifndef APTOS_PART_H
#define APTOS_PART_H

void part_scan_all(void);
int  part_create_single(int parent, u8 ptype, char *out_name, int out_max);

#endif
