#include <buffer.h>
#include <granule.h>
#include <realm.h>
#include <rsi-dev-mem.h>
#include <smc-rsi.h>
#include <status.h>
#include <string.h>
#include <debug.h>

/*
    reg[1] : IPA 
    reg[2] : 1 for delegate (NS -> Realm) 0 for undelegate (Realm -> NS)
*/
struct rsi_delegate_dev_mem_result handle_rsi_dev_mem(struct rec *rec, struct rmi_rec_exit *rec_exit){
	    struct rsi_delegate_dev_mem_result res = { { false, 0UL } };
        unsigned long ipa = rec->regs[1];
        unsigned long delegate_flag = rec->regs[2];
		//int sid = 31;

		struct rd *rd;
		enum s2_walk_status walk_status;
		struct s2_walk_result walk_res;
		struct granule *gr;

		ipa = ipa & GRANULE_MASK;

		if (!GRANULE_ALIGNED(ipa) || !addr_in_rec_par(rec, ipa)) {
			res.smc_result = RSI_ERROR_INPUT;
			ERROR("[SMC_RSI_DEV_MEM] IPA is invalid \n\n");
		}

		granule_lock(rec->realm_info.g_rd, GRANULE_STATE_RD);
		rd = granule_map(rec->realm_info.g_rd, SLOT_RD);


		walk_status = realm_ipa_to_pa(rd, ipa, &walk_res);

		if (walk_status == WALK_FAIL) {
			if (s2_walk_result_match_ripas(&walk_res, RMI_EMPTY)) {
				res.smc_result = RSI_ERROR_INPUT;
			} else {
				/* Exit to Host */
				res.walk_result.abort = true;
				res.walk_result.rtt_level = walk_res.rtt_level;
			}
			ERROR("Walk failed in RSI deleagate dev PAS");
			goto out_unmap_rd;
		}

		if (walk_status == WALK_INVALID_PARAMS) {
			/* Return error to Realm */
			res.smc_result = RSI_ERROR_INPUT;
			ERROR("Walk failed : invalid params");
			goto out_unmap_rd;
		}
		gr = find_granule(walk_res.pa);
		// ERROR("DEV PAS: %lu\n\n\n", walk_res.pa);
		granule_lock(gr, GRANULE_STATE_DATA);

		//Make SMC call to delegate dev pas on the granule now
		WARN("calling smc_granule_delegate_dev\n");
		res.smc_result = smc_granule_delegate_dev(gr, walk_res.pa, delegate_flag);
		if (res.smc_result != RSI_SUCCESS){
			ERROR("smc_granule_delegate_dev failed\n");
		}
		rec->regs[1] = walk_res.pa;
        //TODO[Supraja, Benedict] : add smc call to create S2 table entry for SMMU.
		// * BENE: done through the exit to HV.
		granule_unlock(gr);
		granule_unlock(walk_res.llt);

		out_unmap_rd:
			buffer_unmap(rd);
			granule_unlock(rec->realm_info.g_rd);
        return res;
}