/* Host shim for <linux/types.h> — the pool header needs bool, size_t, s64. */
#ifndef ANE_POOL_SHIM_LINUX_TYPES_H
#define ANE_POOL_SHIM_LINUX_TYPES_H

#include <stddef.h>
#include <stdbool.h>

typedef signed long long s64;
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long dma_addr_t;

#endif /* ANE_POOL_SHIM_LINUX_TYPES_H */
