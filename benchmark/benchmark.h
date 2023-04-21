#ifndef RMM_CCA_BENCHMARK_H_
#define RMM_CCA_BENCHMARK_H_

#define STR(s) #s
#define CCA_MARKER(marker) __asm__ volatile("MOV XZR, " STR(marker))

#define CCA_RSI_DEV_MEM() \
CCA_MARKER(0x105); \

#define CCA_RMI_DEV_ATTACH() \
CCA_MARKER(0x106); \

#endif

