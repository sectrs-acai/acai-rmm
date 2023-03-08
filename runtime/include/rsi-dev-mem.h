#include <rsi-walk.h>
#include <smc-rmi.h>

struct rsi_delegate_dev_mem_result {
	/*
	 * Result of RTT walk performed by RSI command.
	 */
	struct rsi_walk_result walk_result;

	/*
	 * If @walk_result.abort is false,
	 * @smc_result contains X0 value to be returned to the Realm.
	 */
	unsigned long smc_result;
};

struct rsi_delegate_dev_mem_result handle_rsi_dev_mem(struct rec *rec, struct rmi_rec_exit *rec_exit);
