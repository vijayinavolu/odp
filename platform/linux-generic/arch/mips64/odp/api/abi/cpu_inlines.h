/* Copyright (c) 2016-2018, Linaro Limited
 * Copyright (c) 2021, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#ifndef ODP_ARCH_CPU_INLINES_H_
#define ODP_ARCH_CPU_INLINES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

static inline void _odp_cpu_pause(void)
{
	__asm__ __volatile__ ("nop");
	__asm__ __volatile__ ("nop");
	__asm__ __volatile__ ("nop");
	__asm__ __volatile__ ("nop");
}

uint64_t _odp_cpu_cycles(void)
{
	#define CVMX_TMP_STR(x) CVMX_TMP_STR2(x)
	#define CVMX_TMP_STR2(x) #x
	uint64_t cycle;

	__asm__ __volatile__ ("rdhwr %[rt],$" CVMX_TMP_STR(31) :
			   [rt] "=d" (cycle) : : "memory");

	return cycle;
}

uint64_t _odp_cpu_cycles_max(void)
{
	return UINT64_MAX;
}

uint64_t _odp_cpu_cycles_resolution(void)
{
	return 1;
}

int _odp_cpu_cycles_init_global(void)
{
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif
