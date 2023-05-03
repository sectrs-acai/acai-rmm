#ifndef RMM_CCA_BENCHMARK_H_
#define RMM_CCA_BENCHMARK_H_

#define STR(s) #s

// #define MICRO_BENCH 1

#ifndef MICRO_BENCH
#define CCA_MARKER(marker) __asm__ volatile("MOV XZR, " STR(marker))
#define CCA_BENCHMARK_START
#define CCA_BENCHMARK_STOP

#else
#define CCA_FLUSH __asm__ volatile("ISB");
#define CCA_MARKER(marker) CCA_FLUSH __asm__ volatile("MOV XZR, " STR(marker))
#define CCA_TRACE_START  __asm__ volatile("HLT 0x1337");
#define CCA_TRACE_STOP __asm__ volatile("HLT 0x1337");

#define CCA_BENCHMARK_START                                             \
  CCA_TRACE_START;                                                             \
  CCA_FLUSH;                                                                             \
  CCA_MARKER(0x1)

#define CCA_BENCHMARK_STOP                                                     \
  CCA_MARKER(0x2);                                                             \
  CCA_FLUSH;                                                                             \
  CCA_TRACE_STOP
#endif

#define CCA_RSI_DEV_MEM() \
CCA_MARKER(0x105); \

#define CCA_RMI_DEV_ATTACH() \
CCA_MARKER(0x106); \

#define CCA_RMI_DEV_ATTACH_ATTEST() \
CCA_MARKER(0x107); \

#define CCA_SMC_FROM_NS() \
CCA_MARKER(0x108); \

#define CCA_RSI_FROM_REALM() \
CCA_MARKER(0x109); \

#define CCA_RSI_HOST_CALL() \
CCA_MARKER(0x10A); \

//This marker is added directly in assembly as mov XZR, #0x10B
#define CCA_SMC_MONITOR_CALL() \
CCA_MARKER(0x10B); \

#define CCA_RTT_WALK()\
CCA_MARKER(0x200); \

#define smc_version_cca_marker() CCA_MARKER(0x125)
#define smc_read_feature_register_cca_marker() CCA_MARKER(0x126)
#define smc_granule_delegate_cca_marker() CCA_MARKER(0x127)
#define smc_granule_undelegate_cca_marker() CCA_MARKER(0x128)
#define smc_realm_create_cca_marker() CCA_MARKER(0x129)
#define smc_realm_destroy_cca_marker() CCA_MARKER(0x130)
#define smc_realm_activate_cca_marker() CCA_MARKER(0x131)
#define smc_rec_create_cca_marker() CCA_MARKER(0x132)
#define smc_rec_destroy_cca_marker() CCA_MARKER(0x133)
#define smc_rec_enter_cca_marker() CCA_MARKER(0x134)
#define smc_data_create_cca_marker() CCA_MARKER(0x135)
#define smc_data_create_unknown_cca_marker() CCA_MARKER(0x136)
#define smc_data_destroy_cca_marker() CCA_MARKER(0x137)
#define smc_rtt_create_cca_marker() CCA_MARKER(0x138)
#define smc_rtt_destroy_cca_marker() CCA_MARKER(0x139)
#define smc_rtt_fold_cca_marker() CCA_MARKER(0x140)
#define smc_rtt_map_unprotected_cca_marker() CCA_MARKER(0x141)
#define smc_rtt_unmap_unprotected_cca_marker() CCA_MARKER(0x142)
#define smc_rtt_read_entry_cca_marker() CCA_MARKER(0x143)
#define smc_psci_complete_cca_marker() CCA_MARKER(0x144)
#define smc_rec_aux_count_cca_marker() CCA_MARKER(0x145)
#define smc_rtt_init_ripas_cca_marker() CCA_MARKER(0x146)
#define smc_rtt_set_ripas_cca_marker() CCA_MARKER(0x147)

#ifdef MICRO_BENCH
#define RMI_REALM_CREATE_START() CCA_MARKER(0x1040)
#define RMI_REALM_CREATE_STOP() CCA_MARKER(0x1041)

//TODO : Not used now. Add it later for single benchmark. 
#define RSI_DEL_DEV_MEM_START() CCA_MARKER(0x1041)
#define RSI_DEL_DEV_MEM_STOP() CCA_MARKER(0x1041)
#else
#define RMI_REALM_CREATE_START() 
#define RMI_REALM_CREATE_STOP() 
#endif

#endif

