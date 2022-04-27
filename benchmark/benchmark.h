#ifndef RMM_CCA_BENCHMARK_H_
#define RMM_CCA_BENCHMARK_H_

#define STR(s) #s
#define CCA_MARKER(marker) __asm__ volatile("MOV XZR, " STR(marker))

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

#endif

