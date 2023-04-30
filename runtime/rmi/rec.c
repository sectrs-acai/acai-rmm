/*
 * SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 */

#include <arch.h>
#include <arch_features.h>
#include <attestation.h>
#include <buffer.h>
#include <cpuid.h>
#include <gic.h>
#include <granule.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <measurement.h>
#include <memory_alloc.h>
#include <psci.h>
#include <realm.h>
#include <rec.h>
#include <smc-handler.h>
#include <smc-rmi.h>
#include <smc.h>
#include <spinlock.h>
#include <benchmark.h>
#include <stddef.h>
#include <string.h>

/*
 * Allocate a dummy rec_params for copying relevant parameters for measurement
 */
static struct rmi_rec_params rec_params_per_cpu[MAX_CPUS];

static void rec_params_measure(struct rd *rd, struct rmi_rec_params *rec_params)
{
	struct measurement_desc_rec measure_desc = {0};
	struct rmi_rec_params *rec_params_measured =
		&(rec_params_per_cpu[my_cpuid()]);

	memset(rec_params_measured, 0, sizeof(*rec_params_measured));

	/* Copy the relevant parts of the rmi_rec_params structure to be
	 * measured
	 */
	rec_params_measured->pc = rec_params->pc;
	rec_params_measured->flags = rec_params->flags;
	memcpy(rec_params_measured->gprs,
	       rec_params->gprs,
	       sizeof(rec_params->gprs));

	/* Initialize the measurement descriptior structure */
	measure_desc.desc_type = MEASURE_DESC_TYPE_REC;
	measure_desc.len = sizeof(struct measurement_desc_rec);
	memcpy(measure_desc.rim,
	       &rd->measurement[RIM_MEASUREMENT_SLOT],
	       measurement_get_size(rd->algorithm));

	/*
	 * Hashing the REC params structure and store the result in the
	 * measurement descriptor structure.
	 */
	measurement_hash_compute(rd->algorithm,
				rec_params_measured,
				sizeof(*rec_params_measured),
				measure_desc.content);

	/*
	 * Hashing the measurement descriptor structure; the result is the
	 * updated RIM.
	 */
	measurement_hash_compute(rd->algorithm,
			       &measure_desc,
			       sizeof(measure_desc),
			       rd->measurement[RIM_MEASUREMENT_SLOT]);
}

static void init_rec_sysregs(struct rec *rec, unsigned long mpidr)
{
	/* Set non-zero values only */
	rec->sysregs.pmcr_el0 = PMCR_EL0_RES1;
	rec->sysregs.sctlr_el1 = SCTLR_EL1_FLAGS;
	rec->sysregs.mdscr_el1 = MDSCR_EL1_TDCC_BIT;
	rec->sysregs.vmpidr_el2 = mpidr | VMPIDR_EL2_RES1;

	rec->sysregs.cnthctl_el2 = CNTHCTL_EL2_NO_TRAPS;
}

/*
 * Starting level of the stage 2 translation
 * lookup to VTCR_EL2.SL0[7:6].
 */
static const unsigned long sl0_val[] = {
	VTCR_SL0_4K_L0,
	VTCR_SL0_4K_L1,
	VTCR_SL0_4K_L2,
	VTCR_SL0_4K_L3
};

static unsigned long realm_vtcr(struct rd *rd)
{
	unsigned long t0sz, sl0;
	unsigned long vtcr = is_feat_vmid16_present() ?
				(VTCR_FLAGS | VTCR_VS) : VTCR_FLAGS;
	int s2_starting_level = realm_rtt_starting_level(rd);

	/* TODO: Support LPA2 with -1 */
	assert((s2_starting_level >= 0) && (s2_starting_level <= 3));
	sl0 = sl0_val[s2_starting_level];

	t0sz = 64UL - realm_ipa_bits(rd);
	t0sz &= MASK(VTCR_T0SZ);

	vtcr |= t0sz;
	vtcr |= sl0;

	return vtcr;
}

static void init_common_sysregs(struct rec *rec, struct rd *rd)
{
	/* Set non-zero values only */
	rec->common_sysregs.hcr_el2 = HCR_FLAGS;
	rec->common_sysregs.vtcr_el2 =  realm_vtcr(rd);
	rec->common_sysregs.vttbr_el2 = granule_addr(rd->s2_ctx.g_rtt);
	rec->common_sysregs.vttbr_el2 &= MASK(TTBRx_EL2_BADDR);
	rec->common_sysregs.vttbr_el2 |= INPLACE(VTTBR_EL2_VMID, rd->s2_ctx.vmid);
}

static void init_rec_regs(struct rec *rec,
			  struct rmi_rec_params *rec_params,
			  struct rd *rd)
{
	unsigned int i;

	/*
	 * We only need to set non-zero values here because we're intializing
	 * data structures in the rec granule which was just converted from
	 * the DELEGATED state to REC state, and we can rely on the RMM
	 * invariant that DELEGATED granules are always zero-filled.
	 */

	for (i = 0U; i < REC_CREATE_NR_GPRS; i++) {
		rec->regs[i] = rec_params->gprs[i];
	}

	rec->pc = rec_params->pc;
	rec->pstate = SPSR_EL2_MODE_EL1h |
		      SPSR_EL2_nRW_AARCH64 |
		      SPSR_EL2_F_BIT |
		      SPSR_EL2_I_BIT |
		      SPSR_EL2_A_BIT |
		      SPSR_EL2_D_BIT;

	init_rec_sysregs(rec, rec_params->mpidr);
	init_common_sysregs(rec, rd);
}

/*
 * This function will only be invoked when the REC create fails
 * or when REC is being destroyed. Hence the REC will not be in
 * use when this function is called and therefore no lock is
 * acquired before its invocation.
 */
static void free_rec_aux_granules(struct granule *rec_aux[],
				  unsigned int cnt, bool scrub)
{
	for (unsigned int i = 0U; i < cnt; i++) {
		struct granule *g_rec_aux = rec_aux[i];

		granule_lock(g_rec_aux, GRANULE_STATE_REC_AUX);
		if (scrub) {
			granule_memzero(g_rec_aux, SLOT_REC_AUX0 + i);
		}
		granule_unlock_transition(g_rec_aux, GRANULE_STATE_DELEGATED);
	}
}

unsigned long smc_rec_create(unsigned long rec_addr,
			     unsigned long rd_addr,
			     unsigned long rec_params_addr)
{
	struct granule *g_rd;
	struct granule *g_rec;
	struct granule *rec_aux_granules[MAX_REC_AUX_GRANULES];
	struct granule *g_rec_params;
	struct rec *rec;
	struct rd *rd;
	struct rmi_rec_params rec_params;
	unsigned long rec_idx;
	enum granule_state new_rec_state = GRANULE_STATE_DELEGATED;
	unsigned long ret;
	bool ns_access_ok;
	unsigned int num_rec_aux;
	smc_rec_create_cca_marker();

	g_rec_params = find_granule(rec_params_addr);
	if ((g_rec_params == NULL) || (g_rec_params->state != GRANULE_STATE_NS)) {
		return RMI_ERROR_INPUT;
	}

	ns_access_ok = ns_buffer_read(SLOT_NS, g_rec_params, 0U,
				      sizeof(rec_params), &rec_params);

	if (!ns_access_ok) {
		return RMI_ERROR_INPUT;
	}

	num_rec_aux = (unsigned int)rec_params.num_aux;
	if (num_rec_aux > MAX_REC_AUX_GRANULES) {
		return RMI_ERROR_INPUT;
	}

	/* Loop through rec_aux_granules and transit them */
	for (unsigned int i = 0U; i < num_rec_aux; i++) {
		struct granule *g_rec_aux = find_lock_granule(
						rec_params.aux[i],
						GRANULE_STATE_DELEGATED);
		if (g_rec_aux == NULL) {
			free_rec_aux_granules(rec_aux_granules, i, false);
			return RMI_ERROR_INPUT;
		}
		granule_unlock_transition(g_rec_aux, GRANULE_STATE_REC_AUX);
		rec_aux_granules[i] = g_rec_aux;
	}

	if (!find_lock_two_granules(rec_addr,
				GRANULE_STATE_DELEGATED,
				&g_rec,
				rd_addr,
				GRANULE_STATE_RD,
				&g_rd)) {
		ret = RMI_ERROR_INPUT;
		goto out_free_aux;
	}

	rec = granule_map(g_rec, SLOT_REC);
	rd = granule_map(g_rd, SLOT_RD);

	if (get_rd_state_locked(rd) != REALM_STATE_NEW) {
		ret = RMI_ERROR_REALM;
		goto out_unmap;
	}

	rec_idx = get_rd_rec_count_locked(rd);
	if (!mpidr_is_valid(rec_params.mpidr) ||
	   (rec_idx != mpidr_to_rec_idx(rec_params.mpidr))) {
		ret = RMI_ERROR_INPUT;
		goto out_unmap;
	}

	/* Verify the auxiliary granule count with rd lock held */
	if (num_rec_aux != rd->num_rec_aux) {
		ret = RMI_ERROR_INPUT;
		goto out_unmap;
	}

	rec->g_rec = g_rec;
	rec->rec_idx = rec_idx;

	init_rec_regs(rec, &rec_params, rd);
	gic_cpu_state_init(&rec->sysregs.gicstate);

	/* Copy addresses of auxiliary granules */
	(void)memcpy(rec->g_aux, rec_aux_granules,
			num_rec_aux * sizeof(rec->g_aux[0]));

	rec->num_rec_aux = num_rec_aux;

	rec->realm_info.ipa_bits = realm_ipa_bits(rd);
	rec->realm_info.s2_starting_level = realm_rtt_starting_level(rd);
	rec->realm_info.g_rtt = rd->s2_ctx.g_rtt;
	rec->realm_info.g_rd = g_rd;

	rec_params_measure(rd, &rec_params);

	/*
	 * RD has a lock-free access from RMI_REC_DESTROY, hence increment
	 * refcount atomically. Also, since the granule is only used for
	 * refcount update, only an atomic operation will suffice and
	 * release/acquire semantics are not required.
	 */
	atomic_granule_get(g_rd);
	new_rec_state = GRANULE_STATE_REC;
	rec->runnable = rec_params.flags & REC_PARAMS_FLAG_RUNNABLE;

	rec->alloc_info.ctx_initialised = false;
	/* Initialize attestation state */
	rec->token_sign_ctx.state = ATTEST_SIGN_NOT_STARTED;

	set_rd_rec_count(rd, rec_idx + 1U);

	ret = RMI_SUCCESS;

out_unmap:
	buffer_unmap(rd);
	buffer_unmap(rec);

	granule_unlock(g_rd);
	granule_unlock_transition(g_rec, new_rec_state);

out_free_aux:
	if (ret != RMI_SUCCESS) {
		free_rec_aux_granules(rec_aux_granules, num_rec_aux, false);
	}
	return ret;
}

unsigned long smc_rec_destroy(unsigned long rec_addr)
{
	struct granule *g_rec;
	struct granule *g_rd;
	struct rec *rec;
    smc_rec_destroy_cca_marker();

	/* REC should not be destroyed if refcount != 0 */
	g_rec = find_lock_unused_granule(rec_addr, GRANULE_STATE_REC);
	if (ptr_is_err(g_rec)) {
		return (unsigned long)ptr_status(g_rec);
	}

	rec = granule_map(g_rec, SLOT_REC);

	g_rd = rec->realm_info.g_rd;

	/* Free and scrub the auxiliary granules */
	free_rec_aux_granules(rec->g_aux, rec->num_rec_aux, true);

	granule_memzero_mapped(rec);
	buffer_unmap(rec);

	granule_unlock_transition(g_rec, GRANULE_STATE_DELEGATED);

	/*
	 * Decrement refcount. The refcount should be balanced before
	 * RMI_REC_DESTROY returns, and until this occurs a transient
	 * over-estimate of the refcount (in-between the unlock and decreasing
	 * the refcount) is legitimate. Also, since the granule is only used for
	 * refcount update, only an atomic operation will suffice and
	 * release/acquire semantics are not required.
	 */
	atomic_granule_put(g_rd);

	return RMI_SUCCESS;
}

void smc_rec_aux_count(unsigned long rd_addr, struct smc_result *ret_struct)
{
	unsigned int num_rec_aux;
	struct granule *g_rd;
	struct rd *rd;

	g_rd = find_lock_granule(rd_addr, GRANULE_STATE_RD);
	if (g_rd == NULL) {
		ret_struct->x[0] = RMI_ERROR_INPUT;
		return;
	}

	rd = granule_map(g_rd, SLOT_RD);
	num_rec_aux = rd->num_rec_aux;
	buffer_unmap(rd);
	granule_unlock(g_rd);

	ret_struct->x[0] = RMI_SUCCESS;
	ret_struct->x[1] = (unsigned long)num_rec_aux;
}

unsigned long smc_psci_complete(unsigned long calling_rec_addr,
				unsigned long target_rec_addr)
{
	struct granule *g_calling_rec, *g_target_rec;
	struct rec  *calling_rec, *target_rec;
	unsigned long ret;

	assert(calling_rec_addr != 0UL);
	assert(target_rec_addr != 0UL);

	if (!GRANULE_ALIGNED(calling_rec_addr)) {
		return RMI_ERROR_INPUT;
	}

	if (!GRANULE_ALIGNED(target_rec_addr)) {
		return RMI_ERROR_INPUT;
	}

	if (!find_lock_two_granules(calling_rec_addr,
					GRANULE_STATE_REC,
					&g_calling_rec,
					target_rec_addr,
					GRANULE_STATE_REC,
					&g_target_rec)) {
		return RMI_ERROR_INPUT;
	}

	/*
	 * The access to a REC from RMI_REC_ENTER is only protected by the
	 * reference counter. Here, we may access the volatile (non constant)
	 * members of REC structure (such as rec->running) only if the counter
	 * is zero.
	 */
	if (granule_refcount_read_acquire(g_calling_rec) != 0UL) {
		/*
		 * The `calling` REC is running on another PE and therefore it
		 * may not have a pending PSCI request.
		 */
		ret = RMI_ERROR_INPUT;
		goto out_unlock;
	}

	calling_rec = granule_map(g_calling_rec, SLOT_REC);
	target_rec = granule_map(g_target_rec, SLOT_REC2);

	ret = psci_complete_request(calling_rec, target_rec);

	buffer_unmap(target_rec);
	buffer_unmap(calling_rec);
out_unlock:
	granule_unlock(g_calling_rec);
	granule_unlock(g_target_rec);

	return ret;
}
