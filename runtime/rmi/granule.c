/*
 * SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 */

#include <asc.h>
#include <granule.h>
#include <smc-handler.h>
#include <smc-rmi.h>
#include <smc.h>
#include <debug.h>

unsigned long smc_granule_delegate(unsigned long addr)
{
	struct granule *g;

	g = find_lock_granule(addr, GRANULE_STATE_NS);
	if (g == NULL) {
		return RMI_ERROR_INPUT;
	}

	granule_set_state(g, GRANULE_STATE_DELEGATED);
	asc_mark_secure(addr);
	granule_memzero(g, SLOT_DELEGATED);

	granule_unlock(g);
	return RMI_SUCCESS;
}

unsigned long smc_granule_delegate_dev(unsigned long addr)
{
	// struct granule *g;
	//TODO[Supraja]: if this call comes from data create then the lock is already acquired. If not get lock by uncommenting code below
	
	//  g = find_lock_granule(addr, GRANULE_STATE_DATA);
	// if (g == NULL) {
	// 	return RMI_ERROR_INPUT;
	// }

	// //granule_set_state(g, GRANULE_STATE_DELEGATED);
	asc_mark_secure_dev(addr);
	// //granule_memzero(g, SLOT_DELEGATED);
	// granule_unlock(g);
	return RMI_SUCCESS;
}

unsigned long smc_granule_undelegate(unsigned long addr)
{
	struct granule *g;

	g = find_lock_granule(addr, GRANULE_STATE_DELEGATED);
	if (g == NULL) {
		return RMI_ERROR_INPUT;
	}

	asc_mark_nonsecure(addr);
	granule_set_state(g, GRANULE_STATE_NS);

	granule_unlock(g);
	return RMI_SUCCESS;
}
