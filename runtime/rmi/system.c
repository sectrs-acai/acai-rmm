/*
 * SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 */
#include <assert.h>
#include <debug.h>
#include <smc-handler.h>
#include <smc-rmi.h>
#include <benchmark.h>
COMPILER_ASSERT(RMI_ABI_VERSION_MAJOR <= 0x7FFF);
COMPILER_ASSERT(RMI_ABI_VERSION_MINOR <= 0xFFFF);

unsigned long smc_version(void)
{
	smc_version_cca_marker();
	return RMI_ABI_VERSION;
}
