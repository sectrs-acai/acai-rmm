/*
 * SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 */

#include <assert.h>
#include <buffer.h>
#include <feature.h>
#include <granule.h>
#include <measurement.h>
#include <realm.h>
#include <smc-handler.h>
#include <smc-rmi.h>
#include <smc.h>
#include <stddef.h>
#include <string.h>
#include <table.h>
#include <vmid.h>
#include <benchmark.h>

unsigned long smc_realm_activate(unsigned long rd_addr)
{
	smc_realm_activate_cca_marker();
	struct rd *rd;
	struct granule *g_rd;
	unsigned long ret;

	g_rd = find_lock_granule(rd_addr, GRANULE_STATE_RD);
	if (g_rd == NULL) {
		return RMI_ERROR_INPUT;
	}

	rd = granule_map(g_rd, SLOT_RD);
	if (get_rd_state_locked(rd) == REALM_STATE_NEW) {
		set_rd_state(rd, REALM_STATE_ACTIVE);
		ret = RMI_SUCCESS;
	} else {
		ret = RMI_ERROR_REALM;
	}
	buffer_unmap(rd);

	granule_unlock(g_rd);

	return ret;
}

static bool get_realm_params(struct rmi_realm_params *realm_params,
				unsigned long realm_params_addr)
{
	bool ns_access_ok;
	struct granule *g_realm_params;

	g_realm_params = find_granule(realm_params_addr);
	if ((g_realm_params == NULL) || (g_realm_params->state != GRANULE_STATE_NS)) {
		return false;
	}

	ns_access_ok = ns_buffer_read(SLOT_NS, g_realm_params, 0U,
				      sizeof(*realm_params), realm_params);

	return ns_access_ok;
}

/*
 * See the library pseudocode
 * aarch64/translation/vmsa_faults/AArch64.S2InconsistentSL on which this is
 * modeled.
 */
static bool s2_inconsistent_sl(unsigned int ipa_bits, int sl)
{
	int levels = RTT_PAGE_LEVEL - sl;
	unsigned int sl_min_ipa_bits, sl_max_ipa_bits;

	/*
	 * The maximum number of concatenated tables is 16,
	 * hence we are adding 4 to the 'sl_max_ipa_bits'.
	 */
	sl_min_ipa_bits = levels * S2TTE_STRIDE + GRANULE_SHIFT + 1U;
	sl_max_ipa_bits = sl_min_ipa_bits + (S2TTE_STRIDE - 1U) + 4U;

	return ((ipa_bits < sl_min_ipa_bits) || (ipa_bits > sl_max_ipa_bits));
}

static bool validate_ipa_bits_and_sl(unsigned int ipa_bits, long sl)
{
	if ((ipa_bits < MIN_IPA_BITS) || (ipa_bits > MAX_IPA_BITS)) {
		return false;
	}

	if ((sl < MIN_STARTING_LEVEL) || (sl > RTT_PAGE_LEVEL)) {
		return false;
	}

	/*
	 * We assume ARMv8.4-TTST is supported with RME so the only SL
	 * configuration we need to check with 4K granules is SL == 0 following
	 * the library pseudocode aarch64/translation/vmsa_faults/AArch64.S2InvalidSL.
	 *
	 * Note that this only checks invalid SL values against the properties
	 * of the hardware platform, other misconfigurations between IPA size
	 * and SL is checked in s2_inconsistent_sl.
	 */
	if ((sl == 0L) && (max_ipa_size() < 44U)) {
		return false;
	}

	return !s2_inconsistent_sl(ipa_bits, sl);
}

static unsigned int requested_ipa_bits(struct rmi_realm_params *p)
{
	return EXTRACT(RMM_FEATURE_REGISTER_0_S2SZ, p->features_0);
}

static unsigned int s2_num_root_rtts(unsigned int ipa_bits, int sl)
{
	unsigned int levels = (unsigned int)(RTT_PAGE_LEVEL - sl);
	unsigned int sl_ipa_bits;

	/* First calculate how many bits can be resolved without concatenation */
	sl_ipa_bits = levels * S2TTE_STRIDE /* Bits resolved by table walk without SL */
		      + GRANULE_SHIFT	    /* Bits directly mapped to OA */
		      + S2TTE_STRIDE;	    /* Bits resolved by single SL */

	if (sl_ipa_bits >= ipa_bits) {
		return 1U;
	}

	return (1U << (ipa_bits - sl_ipa_bits));
}

static bool validate_realm_params(struct rmi_realm_params *p)
{
	if (!validate_feature_register(RMM_FEATURE_REGISTER_0_INDEX,
					p->features_0)) {
		return false;
	}

	if (!validate_ipa_bits_and_sl(requested_ipa_bits(p),
					p->rtt_level_start)) {
		return false;
	}

	if (s2_num_root_rtts(requested_ipa_bits(p),
				p->rtt_level_start) != p->rtt_num_start) {
		return false;
	}

	/*
	 * TODO: Check the VMSA configuration which is either static for the
	 * RMM or per realm with the supplied parameters and store the
	 * configuration on the RD, and it can potentially be copied into RECs
	 * later.
	 */

	switch (p->hash_algo) {
	case RMI_HASH_ALGO_SHA256:
	case RMI_HASH_ALGO_SHA512:
		break;
	default:
		return false;
	}

	/* Check VMID collision and reserve it atomically if available */
	return vmid_reserve((unsigned int)p->vmid);
}

/*
 * Update the realm measurement with the realm parameters.
 */
static void realm_params_measure(struct rd *rd,
				 struct rmi_realm_params *realm_params)
{
	/* By specification realm_params is 4KB */
	unsigned char buffer[SZ_4K] = {0};
	struct rmi_realm_params *realm_params_measured =
		(struct rmi_realm_params *)&buffer[0];

	realm_params_measured->hash_algo = realm_params->hash_algo;
	/* TODO: Add later */
	/* realm_params_measured->features_0 = realm_params->features_0; */

	/* Measure relevant realm params this will be the init value of RIM */
	measurement_hash_compute(rd->algorithm,
			       buffer,
			       sizeof(buffer),
			       rd->measurement[RIM_MEASUREMENT_SLOT]);
}

static void free_sl_rtts(struct granule *g_rtt, unsigned int num_rtts)
{
	unsigned int i;

	for (i = 0U; i < num_rtts; i++) {
		struct granule *g = g_rtt + i;

		granule_lock(g, GRANULE_STATE_RTT);
		granule_memzero(g, SLOT_RTT);
		granule_unlock_transition(g, GRANULE_STATE_DELEGATED);
	}
}

static bool find_lock_rd_granules(unsigned long rd_addr,
				  struct granule **p_g_rd,
				  unsigned long rtt_base_addr,
				  unsigned int num_rtts,
				  struct granule **p_g_rtt_base)
{
	struct granule *g_rd = NULL, *g_rtt_base = NULL;
	int i = 0;

	if (rd_addr < rtt_base_addr) {
		g_rd = find_lock_granule(rd_addr, GRANULE_STATE_DELEGATED);
		if (g_rd == NULL) {
			goto out_err;
		}
	}

	for (; i < num_rtts; i++) {
		unsigned long rtt_addr = rtt_base_addr + i * GRANULE_SIZE;
		struct granule *g_rtt;

		g_rtt = find_lock_granule(rtt_addr, GRANULE_STATE_DELEGATED);
		if (g_rtt == NULL) {
			goto out_err;
		}

		if (i == 0) {
			g_rtt_base = g_rtt;
		}
	}

	if (g_rd == NULL) {
		g_rd = find_lock_granule(rd_addr, GRANULE_STATE_DELEGATED);
		if (g_rd == NULL) {
			goto out_err;
		}
	}

	*p_g_rd = g_rd;
	*p_g_rtt_base = g_rtt_base;

	return true;

out_err:
	for (i = i - 1; i >= 0; i--) {
		granule_unlock(g_rtt_base + i);
	}

	if (g_rd != NULL) {
		granule_unlock(g_rd);
	}

	return false;
}

unsigned long smc_realm_create(unsigned long rd_addr,
			       unsigned long realm_params_addr)
{
	smc_realm_create_cca_marker();	
	struct granule *g_rd, *g_rtt_base;
	struct rd *rd;
	struct rmi_realm_params p;
	unsigned int i;

	if (!get_realm_params(&p, realm_params_addr)) {
		return RMI_ERROR_INPUT;
	}

	if (!validate_realm_params(&p)) {
		return RMI_ERROR_INPUT;
	}

	/*
	 * At this point VMID is reserved for the Realm
	 *
	 * Check for aliasing between rd_addr and
	 * starting level RTT address(es)
	 */
	if (addr_is_contained(p.rtt_base,
			      p.rtt_base + p.rtt_num_start * GRANULE_SIZE,
			      rd_addr)) {

		/* Free reserved VMID before returning */
		vmid_free((unsigned int)p.vmid);
		return RMI_ERROR_INPUT;
	}

	if (!find_lock_rd_granules(rd_addr, &g_rd, p.rtt_base,
				  p.rtt_num_start, &g_rtt_base)) {
		/* Free reserved VMID */
		vmid_free((unsigned int)p.vmid);
		return RMI_ERROR_INPUT;
	}

	rd = granule_map(g_rd, SLOT_RD);
	set_rd_state(rd, REALM_STATE_NEW);
	set_rd_rec_count(rd, 0UL);
	rd->s2_ctx.g_rtt = find_granule(p.rtt_base);
	rd->s2_ctx.ipa_bits = requested_ipa_bits(&p);
	rd->s2_ctx.s2_starting_level = p.rtt_level_start;
	rd->s2_ctx.num_root_rtts = p.rtt_num_start;
	memcpy(&rd->rpv[0], &p.rpv[0], RPV_SIZE);

	rd->s2_ctx.vmid = (unsigned int)p.vmid;

	rd->num_rec_aux = MAX_REC_AUX_GRANULES;

	(void)memcpy(&rd->rpv[0], &p.rpv[0], RPV_SIZE);

	rd->algorithm = p.hash_algo;

	switch (p.hash_algo) {
	case RMI_HASH_ALGO_SHA256:
		rd->algorithm = HASH_ALGO_SHA256;
		break;
	case RMI_HASH_ALGO_SHA512:
		rd->algorithm = HASH_ALGO_SHA512;
		break;
	}
	realm_params_measure(rd, &p);

	buffer_unmap(rd);

	granule_unlock_transition(g_rd, GRANULE_STATE_RD);

	for (i = 0U; i < p.rtt_num_start; i++) {
		granule_unlock_transition(g_rtt_base + i, GRANULE_STATE_RTT);
	}

	return RMI_SUCCESS;
}

static unsigned long total_root_rtt_refcount(struct granule *g_rtt,
					     unsigned int num_rtts)
{
	unsigned long refcount = 0UL;
	unsigned int i;

	for (i = 0U; i < num_rtts; i++) {
		struct granule *g = g_rtt + i;

	       /*
		* Lock starting from the RTT root.
		* Enforcing locking order RD->RTT is enough to ensure
		* deadlock free locking guarentee.
		*/
		granule_lock(g, GRANULE_STATE_RTT);
		refcount += g->refcount;
		granule_unlock(g);
	}

	return refcount;
}

unsigned long smc_realm_destroy(unsigned long rd_addr)
{
	smc_realm_destroy_cca_marker();

	struct granule *g_rd;
	struct granule *g_rtt;
	struct rd *rd;
	unsigned int num_rtts;

	/* RD should not be destroyed if refcount != 0. */
	g_rd = find_lock_unused_granule(rd_addr, GRANULE_STATE_RD);
	if (ptr_is_err(g_rd)) {
		return (unsigned long)ptr_status(g_rd);
	}

	rd = granule_map(g_rd, SLOT_RD);
	g_rtt = rd->s2_ctx.g_rtt;
	num_rtts = rd->s2_ctx.num_root_rtts;

	/*
	 * All the mappings in the Realm have been removed and the TLB caches
	 * are invalidated. Therefore, there are no TLB entries tagged with
	 * this Realm's VMID (in this security state).
	 * Just release the VMID value so it can be used in another Realm.
	 */
	vmid_free(rd->s2_ctx.vmid);
	buffer_unmap(rd);

	/* Check if granules are unused */
	if (total_root_rtt_refcount(g_rtt, num_rtts) != 0UL) {
		granule_unlock(g_rd);
		return RMI_ERROR_IN_USE;
	}

	free_sl_rtts(g_rtt, num_rtts);

	/* This implictly destroys the measurement */
	granule_memzero(g_rd, SLOT_RD);
	granule_unlock_transition(g_rd, GRANULE_STATE_DELEGATED);

	return RMI_SUCCESS;
}
