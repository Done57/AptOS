#ifndef APTOS_USER_H
#define APTOS_USER_H

#include "types.h"

#define USER_STACK_TOP  0xC8000000u
#define USER_STACK_SIZE (64 * 1024)

int  user_exec_image(const void *elf, u32 size, const char *arg0, const char *arg1);
int  user_active(void);
void user_exit(int status);
int  user_last_status(void);

#endif
