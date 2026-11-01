#ifndef APTOS_SMP_H
#define APTOS_SMP_H

#include "types.h"

/* ---------- SMP ---------- */
int  smp_init(void);                  /* 唤醒所有 AP,返回额外启动的核数 */
int  smp_cpu_count(void);             /* 总核数(BSP + AP) */
int  smp_cpu_id(void);                /* 当前核编号(BSP=0) */
u32  smp_apic_read_id(void);          /* 当前核 Local APIC ID */

/* ---------- 自旋锁(多核互斥) ---------- */
typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

void spin_lock(spinlock_t *lk);
void spin_unlock(spinlock_t *lk);
int  spin_trylock(spinlock_t *lk);

#endif
