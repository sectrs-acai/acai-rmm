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
	reg[3] : size in number of granules 
*/
struct rsi_delegate_dev_mem_result handle_rsi_dev_mem(struct rec *rec, struct rmi_rec_exit *rec_exit){
	    struct rsi_delegate_dev_mem_result res = { { false, 0UL } };
        unsigned long ipa = rec->regs[1];
        unsigned long delegate_flag = rec->regs[2];
		unsigned long size = rec->regs[3];

		unsigned long pas[size]; 
		unsigned long ipas[size];
		struct granule *grs[size];
		//int sid = 31;

		struct rd *rd;
		

		ipa = ipa & GRANULE_MASK;

		if (!GRANULE_ALIGNED(ipa) || !addr_in_rec_par(rec, ipa)) {
			res.smc_result = RSI_ERROR_INPUT;
			ERROR("[SMC_RSI_DEV_MEM] IPA is invalid \n\n");
		}

		granule_lock(rec->realm_info.g_rd, GRANULE_STATE_RD);
		rd = granule_map(rec->realm_info.g_rd, SLOT_RD);
		int size_del = 0;
		for(int i = 0; i < size; i++){
			struct s2_walk_result walk_res;
			enum s2_walk_status walk_status;
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
				granule_unlock(walk_res.llt);
				continue;
			}

			if (walk_status == WALK_INVALID_PARAMS) {
				/* Return error to Realm */
				res.smc_result = RSI_ERROR_INPUT;
				ERROR("Walk failed : invalid params");
				granule_unlock(walk_res.llt);
				continue;
			}
			grs[i] = find_granule(walk_res.pa);
			pas[i] = walk_res.pa;
			ipas[i] = ipa; 
			size_del = size_del + 1;
			ipa += GRANULE_SIZE;
			// ERROR("DEV PAS: %lu\n\n\n", walk_res.pa);
			granule_lock(grs[i], GRANULE_STATE_DATA);
			granule_unlock(walk_res.llt);
		}
		//Make SMC call to delegate dev pas on the granule now
		for(int i = 0; i < size_del; i++){
			INFO("calling smc_granule_delegate_dev ipa: %lx | delegate_flag: %lx\n",ipas[i], delegate_flag);
			res.smc_result = smc_granule_delegate_dev(pas[i], delegate_flag, ipas[i]);
			//res.smc_result = RSI_SUCCESS;
			if (res.smc_result != RSI_SUCCESS){
				ERROR("smc_granule_delegate_dev failed\n");
			}else{
				INFO("Success delegating dev mem at pa %lx ", pas[i]);
			}
			if(delegate_flag){
				break;
			}
		}
		// Overwrite ipa with real PA
		// rec->regs[3] = walk_res.pa;
        //TODO[Supraja, Benedict] : add smc call to create S2 table entry for SMMU.
		// * BENE: done through the exit to HV.
		for(int i = 0; i < size_del; i++){
			granule_unlock(grs[i]);
		}
		
		// out_unmap_rd:
		buffer_unmap(rd);
		granule_unlock(rec->realm_info.g_rd);
        return res;
}