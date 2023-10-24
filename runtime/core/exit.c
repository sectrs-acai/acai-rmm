/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 */

#include <arch.h>
#include <arch_helpers.h>
#include <attestation_token.h>
#include <buffer.h>
#include <esr.h>
#include <exit.h>
#include <fpu_helpers.h>
#include <gic.h>
#include <granule.h>
#include <inject_exp.h>
#include <memory_alloc.h>
#include <psci.h>
#include <realm.h>
#include <realm_attest.h>
#include <rec.h>
#include <rsi-config.h>
#include <rsi-dev-mem.h>
#include <rsi-handler.h>
#include <rsi-host-call.h>
#include <rsi-logger.h>
#include <rsi-memory.h>
#include <rsi-walk.h>
#include <smc-rmi.h>
#include <smc-rsi.h>
#include <status.h>
#include <sve.h>
#include <sysreg_traps.h>
#include <table.h>

void save_fpu_state(struct fpu_state *fpu);
void restore_fpu_state(struct fpu_state *fpu);

static void system_abort(void)
{
	/*
	 * TODO: report the abort to the EL3.
	 * We need to establish the exact EL3 API first.
	 */
	assert(false);
}

static bool fixup_aarch32_data_abort(struct rec *rec, unsigned long *esr)
{
	unsigned long spsr = read_spsr_el2();

	if ((spsr & SPSR_EL2_nRW_AARCH32) != 0UL) {
		/*
		 * mmio emulation of AArch32 reads/writes is not supported.
		 */
		*esr &= ~ESR_EL2_ABORT_ISV_BIT;
		return true;
	}
	return false;
}

static unsigned long get_dabt_write_value(struct rec *rec, unsigned long esr)
{
	unsigned int rt = esr_srt(esr);

	/* Handle xzr */
	if (rt == 31U) {
		return 0UL;
	}
	return rec->regs[rt] & access_mask(esr);
}

/*
 * Returns 'true' if access from @rec to @addr is within the Protected IPA space.
 */
static bool access_in_rec_par(struct rec *rec, unsigned long addr)
{
	/*
	 * It is OK to check only the base address of the access because:
	 * - The Protected IPA space starts at address zero.
	 * - The IPA width is below 64 bits, therefore the access cannot
	 *   wrap around.
	 */
	return addr_in_rec_par(rec, addr);
}

/*
 * Returns 'true' if the @ipa is in PAR and its RIPAS is 'empty'.
 *
 * @ipa must be aligned to the granule size.
 */
static bool ipa_is_empty(unsigned long ipa, struct rec *rec)
{
	unsigned long s2tte, *ll_table;
	struct rtt_walk wi;
	enum ripas ripas;
	bool ret;

	assert(GRANULE_ALIGNED(ipa));

	if (!addr_in_rec_par(rec, ipa)) {
		return false;
	}
	granule_lock(rec->realm_info.g_rtt, GRANULE_STATE_RTT);

	rtt_walk_lock_unlock(rec->realm_info.g_rtt,
			     rec->realm_info.s2_starting_level,
			     rec->realm_info.ipa_bits,
			     ipa, RTT_PAGE_LEVEL, &wi);

	ll_table = granule_map(wi.g_llt, SLOT_RTT);
	s2tte = s2tte_read(&ll_table[wi.index]);

	if (s2tte_is_destroyed(s2tte)) {
		ret = false;
		goto out_unmap_ll_table;
	}
	ripas = s2tte_get_ripas(s2tte);
	ret = (ripas == RMI_EMPTY);

out_unmap_ll_table:
	buffer_unmap(ll_table);
	granule_unlock(wi.g_llt);
	return ret;
}

static bool fsc_is_external_abort(unsigned long fsc)
{
	if (fsc == ESR_EL2_ABORT_FSC_SEA) {
		return true;
	}

	if ((fsc >= ESR_EL2_ABORT_FSC_SEA_TTW_START) &&
	    (fsc <= ESR_EL2_ABORT_FSC_SEA_TTW_END)) {
		return true;
	}

	return false;
}

/*
 * Handles Data/Instruction Aborts at a lower EL with External Abort fault
 * status code (D/IFSC).
 * Returns 'true' if the exception is the external abort and the `rec_exit`
 * structure is populated, 'false' otherwise.
 */
static bool handle_sync_external_abort(struct rec *rec,
				       struct rmi_rec_exit *rec_exit,
				       unsigned long esr)
{
	unsigned long fsc = esr & ESR_EL2_ABORT_FSC_MASK;
	unsigned long set = esr & ESR_EL2_ABORT_SET_MASK;

	if (!fsc_is_external_abort(fsc)) {
		return false;
	}

	switch (set) {
	case ESR_EL2_ABORT_SET_UER:
		/*
		 * The recoverable SEA.
		 * Inject the sync. abort into the Realm.
		 * Report the exception to the host.
		 */
		inject_sync_idabort(ESR_EL2_ABORT_FSC_SEA);
		/*
		 * Fall through.
		 */
	case ESR_EL2_ABORT_SET_UEO:
		/*
		 * The restartable SEA.
		 * Report the exception to the host.
		 * The REC restarts the same instruction.
		 */
		rec_exit->esr = esr & ESR_NONEMULATED_ABORT_MASK;

		/*
		 * The value of the HPFAR_EL2 is not provided to the host as
		 * it is undefined for external aborts.
		 *
		 * We also don't provide the content of FAR_EL2 because it
		 * has no practical value to the host without the HPFAR_EL2.
		 */
		break;
	case ESR_EL2_ABORT_SET_UC:
		/*
		 * The uncontainable SEA.
		 * Fatal to the system.
		 */
		system_abort();
		break;
	default:
		assert(false);
	}

	return true;
}

void emulate_stage2_data_abort(struct rec *rec,
			       struct rmi_rec_exit *rec_exit,
			       unsigned long rtt_level)
{
	unsigned long fipa = rec->regs[1];

	assert(rtt_level <= RTT_PAGE_LEVEL);

	/*
	 * Setup Exception Syndrom Register to emulate a real data abort
	 * and return to NS host to handle it.
	 */
	rec_exit->esr = (ESR_EL2_EC_DATA_ABORT |
			(ESR_EL2_ABORT_FSC_TRANSLATION_FAULT_L0 + rtt_level));
	rec_exit->far = 0UL;
	rec_exit->hpfar = fipa >> HPFAR_EL2_FIPA_OFFSET;
	rec_exit->exit_reason = RMI_EXIT_SYNC;
}

/*
 * Returns 'true' if the abort is handled and the RMM should return to the Realm,
 * and returns 'false' if the exception should be reported to the HS host.
 */
static bool handle_data_abort(struct rec *rec, struct rmi_rec_exit *rec_exit,
			      unsigned long esr)
{
	unsigned long far = 0UL;
	unsigned long hpfar = read_hpfar_el2();
	unsigned long fipa = (hpfar & HPFAR_EL2_FIPA_MASK) << HPFAR_EL2_FIPA_OFFSET;
	unsigned long write_val = 0UL;

	if (handle_sync_external_abort(rec, rec_exit, esr)) {
		/*
		 * All external aborts are immediately reported to the host.
		 */
		return false;
	}

	/*
	 * The memory access that crosses a page boundary may cause two aborts
	 * with `hpfar_el2` values referring to two consecutive pages.
	 *
	 * Insert the SEA and return to the Realm if the granule's RIPAS is EMPTY.
	 */
	if (ipa_is_empty(fipa, rec)) {
		inject_sync_idabort(ESR_EL2_ABORT_FSC_SEA);
		return true;
	}

	if (fixup_aarch32_data_abort(rec, &esr) ||
	    access_in_rec_par(rec, fipa)) {
		esr &= ESR_NONEMULATED_ABORT_MASK;
		goto end;
	}

	if (esr_is_write(esr)) {
		write_val = get_dabt_write_value(rec, esr);
	}

	far = read_far_el2() & ~GRANULE_MASK;
	esr &= ESR_EMULATED_ABORT_MASK;

end:
	rec_exit->esr = esr;
	rec_exit->far = far;
	rec_exit->hpfar = hpfar;
	rec_exit->gprs[0] = write_val;

	return false;
}

/*
 * Returns 'true' if the abort is handled and the RMM should return to the Realm,
 * and returns 'false' if the exception should be reported to the NS host.
 */
static bool handle_instruction_abort(struct rec *rec, struct rmi_rec_exit *rec_exit,
				     unsigned long esr)
{
	unsigned long fsc = esr & ESR_EL2_ABORT_FSC_MASK;
	unsigned long fsc_type = fsc & ~ESR_EL2_ABORT_FSC_LEVEL_MASK;
	unsigned long hpfar = read_hpfar_el2();
	unsigned long fipa = (hpfar & HPFAR_EL2_FIPA_MASK) << HPFAR_EL2_FIPA_OFFSET;

	if (handle_sync_external_abort(rec, rec_exit, esr)) {
		/*
		 * All external aborts are immediately reported to the host.
		 */
		return false;
	}

	/*
	 * Insert the SEA and return to the Realm if:
	 * - The instruction abort is at an Unprotected IPA, or
	 * - The granule's RIPAS is EMPTY
	 */
	if (!access_in_rec_par(rec, fipa) || ipa_is_empty(fipa, rec)) {
		inject_sync_idabort(ESR_EL2_ABORT_FSC_SEA);
		return true;
	}

	if (fsc_type != ESR_EL2_ABORT_FSC_TRANSLATION_FAULT) {
		unsigned long far = read_far_el2();

		/*
		 * TODO: Should this ever happen, or is it an indication of an
		 * internal consistency failure in the RMM which should lead
		 * to a panic instead?
		 */

		ERROR("Unhandled instruction abort:\n");
		ERROR("    FSC: %12s0x%02lx\n", " ", fsc);
		ERROR("    FAR: %16lx\n", far);
		ERROR("  HPFAR: %16lx\n", hpfar);
		return false;
	}

	rec_exit->hpfar = hpfar;
	rec_exit->esr = esr & ESR_NONEMULATED_ABORT_MASK;

	return false;
}

/*
 * Return 'false' if no IRQ is pending,
 * return 'true' if there is an IRQ pending, and need to return to host.
 */
static bool check_pending_irq(void)
{
	unsigned long pending_irq;

	pending_irq = read_isr_el1();

	return (pending_irq != 0UL);
}

static void advance_pc(void)
{
	unsigned long pc = read_elr_el2();

	write_elr_el2(pc + 4UL);
}

static void return_result_to_realm(struct rec *rec, struct smc_result result)
{
	rec->regs[0] = result.x[0];
	rec->regs[1] = result.x[1];
	rec->regs[2] = result.x[2];
	rec->regs[3] = result.x[3];
}

/*
 * Return 'true' if execution should continue in the REC, otherwise return
 * 'false' to go back to the NS caller of REC.Enter.
 */
static bool handle_realm_rsi(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	bool ret_to_rec = true;	/* Return to Realm */
	unsigned int function_id = rec->regs[0];

	RSI_LOG_SET(rec->regs[1], rec->regs[2],
		    rec->regs[3], rec->regs[4], rec->regs[5]);

	if (!IS_SMC32_PSCI_FID(function_id) && !IS_SMC64_PSCI_FID(function_id)
	    && !IS_SMC64_RSI_FID(function_id)) {

		ERROR("Invalid RSI function_id = %x\n", function_id);
		rec->regs[0] = SMC_UNKNOWN;
		return true;
	}

	switch (function_id) {
	case SMCCC_VERSION:
		rec->regs[0] = SMCCC_VERSION_NUMBER;
		break;
	case SMC_RSI_ABI_VERSION:
		rec->regs[0] = system_rsi_abi_version();
		break;
	case SMC32_PSCI_FID_MIN ... SMC32_PSCI_FID_MAX:
	case SMC64_PSCI_FID_MIN ... SMC64_PSCI_FID_MAX: {
		struct psci_result res;

		res = psci_rsi(rec,
			       function_id,
			       rec->regs[1],
			       rec->regs[2],
			       rec->regs[3]);

		if (!rec->psci_info.pending) {
			rec->regs[0] = res.smc_res.x[0];
			rec->regs[1] = res.smc_res.x[1];
			rec->regs[2] = res.smc_res.x[2];
			rec->regs[3] = res.smc_res.x[3];
		}

		if (res.hvc_forward.forward_psci_call) {
			unsigned int i;

			rec_exit->exit_reason = RMI_EXIT_PSCI;
			rec_exit->gprs[0] = function_id;
			rec_exit->gprs[1] = res.hvc_forward.x1;
			rec_exit->gprs[2] = res.hvc_forward.x2;
			rec_exit->gprs[3] = res.hvc_forward.x3;

			for (i = 4U; i < REC_EXIT_NR_GPRS; i++) {
				rec_exit->gprs[i] = 0UL;
			}

			advance_pc();
			ret_to_rec = false;
		}
		break;
	}
	case SMC_RSI_ATTEST_TOKEN_INIT:
		rec->regs[0] = handle_rsi_attest_token_init(rec);
		break;
	case SMC_RSI_ATTEST_TOKEN_CONTINUE: {
		struct attest_result res;
		attest_realm_token_sign_continue_start();
		while (true) {
			/*
			 * Possible outcomes:
			 *     if res.incomplete is true
			 *         if IRQ pending
			 *             check for pending IRQ and return to host
			 *         else try a new iteration
			 *     else
			 *         if RTT table walk has failed,
			 *             emulate data abort back to host
			 *         otherwise
			 *             return to realm because the token
			 *             creation is complete or input parameter
			 *             validation failed.
			 */
			handle_rsi_attest_token_continue(rec, &res);

			if (res.incomplete) {
				if (check_pending_irq()) {
					rec_exit->exit_reason = RMI_EXIT_IRQ;
					/* Return to NS host to handle IRQ. */
					ret_to_rec = false;
					break;
				}
			} else {
				if (res.walk_result.abort) {
					emulate_stage2_data_abort(
						rec, rec_exit,
						res.walk_result.rtt_level);
					ret_to_rec = false; /* Exit to Host */
					break;
				}

				/* Return to Realm */
				return_result_to_realm(rec, res.smc_res);
				break;
			}
		}
		attest_realm_token_sign_continue_finish();
		break;
	}
	case SMC_RSI_MEASUREMENT_READ:
		rec->regs[0] = handle_rsi_read_measurement(rec);
		break;
	case SMC_RSI_MEASUREMENT_EXTEND:
		rec->regs[0] = handle_rsi_extend_measurement(rec);
		break;
	case SMC_RSI_REALM_CONFIG: {
		struct rsi_walk_smc_result res;

		res = handle_rsi_realm_config(rec);
		if (res.walk_result.abort) {
			emulate_stage2_data_abort(rec, rec_exit,
						  res.walk_result.rtt_level);
			ret_to_rec = false; /* Exit to Host */
		} else {
			/* Return to Realm */
			return_result_to_realm(rec, res.smc_res);
		}
		break;
	}
	case SMC_RSI_IPA_STATE_SET:
		if (handle_rsi_ipa_state_set(rec, rec_exit)) {
			rec->regs[0] = RSI_ERROR_INPUT;
		} else {
			advance_pc();
			ret_to_rec = false; /* Return to Host */
		}
		break;
	case SMC_RSI_IPA_STATE_GET: {
		struct rsi_walk_smc_result res;

		res = handle_rsi_ipa_state_get(rec);
		if (res.walk_result.abort) {
			emulate_stage2_data_abort(rec, rec_exit,
						  res.walk_result.rtt_level);
			/* Exit to Host */
			ret_to_rec = false;
		} else {
			/* Exit to Realm */
			return_result_to_realm(rec, res.smc_res);
		}
		break;
	}
	case SMC_RSI_HOST_CALL: {
		struct rsi_host_call_result res;
		res = handle_rsi_host_call(rec, rec_exit);

		if (res.walk_result.abort) {

			emulate_stage2_data_abort(rec, rec_exit,
						  res.walk_result.rtt_level);
			/* Exit to Host */
			ret_to_rec = false;
		} else {
			rec->regs[0] = res.smc_result;

			/*
			 * Return to Realm in case of error,
			 * parent function calls advance_pc()
			 */
			if (rec->regs[0] == RSI_SUCCESS) {
				advance_pc();

				/* Exit to Host */
				rec->host_call = true;
				rec_exit->exit_reason = RMI_EXIT_HOST_CALL;
				ret_to_rec = false;
			}
		}
		break;
	}
	case SMC_RSI_DEV_MEM: {
		struct rsi_delegate_dev_mem_result res;
		WARN("handle_rsi_dev_mem \n");
		WARN("IPA %lx\n",rec->regs[1]);
		res = handle_rsi_dev_mem(rec, rec_exit);
		WARN("PA %lx\n",rec->regs[1]);
		
		rec_exit->exit_reason = RMI_EXIT_DEV_MEM;
		ret_to_rec = false;
		rec->regs[0] = res.smc_result;

		// Do we need it ???
		// Probably yes, without it we get "invalid RSI function_id = 0" in the RMM log
		advance_pc();
		// reg[1] PA
		// reg[2] IOVA
		// reg[3] Stream ID
		rec_exit->gprs[1] = rec->regs[1]; 
		rec_exit->gprs[2] = rec->regs[1]; 
		rec_exit->gprs[3] = 31;

		break;
	}
	case _SMC_REQUEST_DEVICE_OWNERSHIP: {
		struct rsi_delegate_dev_mem_result res;
		WARN("handle_rsi_dev_mem \n");

		// TODO: get the vmid, rec_idx is NOT the vmid.
		res.smc_result = monitor_call(SMC_REQUEST_DEVICE_OWNERSHIP, rec->regs[1], rec->rec_idx, 0, 0, 0, 0);
		
		ret_to_rec = true;
		rec->regs[0] = res.smc_result;

		break;
	}
	default:
		rec->regs[0] = SMC_UNKNOWN;
		break;
	}

	/* Log RSI call */
	RSI_LOG_EXIT(function_id, rec->regs[0], ret_to_rec);
	return ret_to_rec;
}

/*
 * Return 'true' if the RMM handled the exception,
 * 'false' to return to the Non-secure host.
 */
static bool handle_exception_sync(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	const unsigned long esr = read_esr_el2();

	switch (esr & ESR_EL2_EC_MASK) {
	case ESR_EL2_EC_WFX:
		rec_exit->esr = esr & (ESR_EL2_EC_MASK | ESR_EL2_WFx_TI_BIT);
		advance_pc();
		return false;
	case ESR_EL2_EC_HVC:
		realm_inject_undef_abort();
		return true;
	case ESR_EL2_EC_SMC:
		if (!handle_realm_rsi(rec, rec_exit)) {
			return false;
		}
		/*
		 * Advance PC.
		 * HCR_EL2.TSC traps execution of the SMC instruction.
		 * It is not a routing control for the SMC exception.
		 * Trap exceptions and SMC exceptions have different
		 * preferred return addresses.
		 */
		advance_pc();
		return true;
	case ESR_EL2_EC_SYSREG: {
		bool ret = handle_sysreg_access_trap(rec, rec_exit, esr);

		advance_pc();
		return ret;
	}
	case ESR_EL2_EC_INST_ABORT:
		return handle_instruction_abort(rec, rec_exit, esr);
	case ESR_EL2_EC_DATA_ABORT:
		return handle_data_abort(rec, rec_exit, esr);
	case ESR_EL2_EC_FPU: {
		unsigned long cptr;

		/*
		 * Realm has requested FPU/SIMD access, so save NS state and
		 * load realm state.  Start by disabling traps so we can save
		 * the NS state and load the realm state.
		 */
		cptr = read_cptr_el2();
		cptr &= ~(CPTR_EL2_FPEN_MASK << CPTR_EL2_FPEN_SHIFT);
		cptr |= (CPTR_EL2_FPEN_NO_TRAP_11 << CPTR_EL2_FPEN_SHIFT);
		cptr &= ~(CPTR_EL2_ZEN_MASK << CPTR_EL2_ZEN_SHIFT);
		cptr |= (CPTR_EL2_ZEN_NO_TRAP_11 << CPTR_EL2_ZEN_SHIFT);
		write_cptr_el2(cptr);

		/*
		 * Save NS state, restore realm state, and set flag indicating
		 * realm has used FPU so we know to save and restore NS state at
		 * realm exit.
		 */
		if (rec->ns->sve != NULL) {
			save_sve_state(rec->ns->sve);
		} else {
			assert(rec->ns->fpu != NULL);
			fpu_save_state(rec->ns->fpu);
		}
		fpu_restore_state(&rec->fpu_ctx.fpu);
		rec->fpu_ctx.used = true;

		/*
		 * Disable SVE for now, until per rec save/restore is
		 * implemented
		 */
		cptr = read_cptr_el2();
		cptr &= ~(CPTR_EL2_ZEN_MASK << CPTR_EL2_ZEN_SHIFT);
		cptr |= (CPTR_EL2_ZEN_TRAP_ALL_00 << CPTR_EL2_ZEN_SHIFT);
		write_cptr_el2(cptr);

		/*
		 * Return 'true' indicating that this exception
		 * has been handled and execution can continue.
		 */
		return true;
	}
	default:
		/*
		 * TODO: Check if there are other exit reasons we could
		 * encounter here and handle them appropriately
		 */
		break;
	}

	VERBOSE("Unhandled sync exit ESR: %08lx (EC: %lx ISS: %lx)\n",
		esr,
		(esr & ESR_EL2_EC_MASK) >> ESR_EL2_EC_SHIFT,
		(esr & ESR_EL2_ISS_MASK) >> ESR_EL2_ISS_SHIFT);

	/*
	 * Zero values in esr, far & hpfar of 'rec_exit' structure
	 * will be returned to the NS host.
	 * The only information that may leak is when there was
	 * some unhandled/unknown reason for the exception.
	 */
	return false;
}

/*
 * Return 'true' if the RMM handled the exception, 'false' to return to the
 * Non-secure host.
 */
static bool handle_exception_serror_lel(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	const unsigned long esr = read_esr_el2();

	if (esr & ESR_EL2_SERROR_IDS_BIT) {
		/*
		 * Implementation defined content of the esr.
		 */
		system_abort();
	}

	if ((esr & ESR_EL2_SERROR_DFSC_MASK) != ESR_EL2_SERROR_DFSC_ASYNC) {
		/*
		 * Either Uncategorized or Reserved fault status code.
		 */
		system_abort();
	}

	switch (esr & ESR_EL2_SERROR_AET_MASK) {
	case ESR_EL2_SERROR_AET_UEU:	/* Unrecoverable RAS Error */
	case ESR_EL2_SERROR_AET_UER:	/* Recoverable RAS Error */
		/*
		 * The abort is fatal to the current S/W. Inject the SError into
		 * the Realm so it can e.g. shut down gracefully or localize the
		 * problem at the specific EL0 application.
		 *
		 * Note: Consider shutting down the Realm here to avoid
		 * the host's attack on unstable Realms.
		 */
		inject_serror(rec, esr);
		/*
		 * Fall through.
		 */
	case ESR_EL2_SERROR_AET_CE:	/* Corrected RAS Error */
	case ESR_EL2_SERROR_AET_UEO:	/* Restartable RAS Error */
		/*
		 * Report the exception to the host.
		 */
		rec_exit->esr = esr & ESR_SERROR_MASK;
		break;
	case ESR_EL2_SERROR_AET_UC:	/* Uncontainable RAS Error */
		system_abort();
		break;
	default:
		/*
		 * Unrecognized Asynchronous Error Type
		 */
		assert(false);
	}

	return false;
}

static bool handle_exception_irq_lel(struct rec *rec, struct rmi_rec_exit *rec_exit)
{
	(void)rec;

	rec_exit->exit_reason = RMI_EXIT_IRQ;

	/*
	 * With GIC all virtual interrupt programming
	 * must go via the NS hypervisor.
	 */
	return false;
}

/* Returns 'true' when returning to Realm (S) and false when to NS */
bool handle_realm_exit(struct rec *rec, struct rmi_rec_exit *rec_exit, int exception)
{
	switch (exception) {
	case ARM_EXCEPTION_SYNC_LEL: {
		bool ret;

		/*
		 * TODO: Sanitize ESR to ensure it doesn't leak sensitive
		 * information.
		 */
		rec_exit->exit_reason = RMI_EXIT_SYNC;
		ret = handle_exception_sync(rec, rec_exit);
		if (!ret) {
			rec->last_run_info.esr = read_esr_el2();
			rec->last_run_info.far = read_far_el2();
			rec->last_run_info.hpfar = read_hpfar_el2();
		}
		return ret;

		/*
		 * TODO: Much more detailed handling of exit reasons.
		 */
	}
	case ARM_EXCEPTION_IRQ_LEL:
		return handle_exception_irq_lel(rec, rec_exit);
	case ARM_EXCEPTION_FIQ_LEL:
		rec_exit->exit_reason = RMI_EXIT_FIQ;
		break;
	case ARM_EXCEPTION_SERROR_LEL: {
		const unsigned long esr = read_esr_el2();
		bool ret;

		/*
		 * TODO: Sanitize ESR to ensure it doesn't leak sensitive
		 * information.
		 */
		rec_exit->exit_reason = RMI_EXIT_SERROR;
		ret = handle_exception_serror_lel(rec, rec_exit);
		if (!ret) {
			rec->last_run_info.esr = esr;
			rec->last_run_info.far = read_far_el2();
			rec->last_run_info.hpfar = read_hpfar_el2();
		}
		return ret;
	}
	default:
		INFO("Unrecognized exit reason: %d\n", exception);
		break;
	};

	return false;
}
