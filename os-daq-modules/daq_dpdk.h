#include <rte_config.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_version.h>

#define DAQ_DPDK_VERSION 3

#define MAX_ARGS 64

#define BURST_SIZE 32


#define FOR_EACH_INSTANCES(list,v) \
    DpdkInstance* v; \
    for (v = list; v; v = v->next)

static const DAQ_Verdict verdict_translation_table[MAX_DAQ_VERDICT] = {
    DAQ_VERDICT_PASS,       /* DAQ_VERDICT_PASS */
    DAQ_VERDICT_BLOCK,      /* DAQ_VERDICT_BLOCK */
    DAQ_VERDICT_PASS,       /* DAQ_VERDICT_REPLACE */
    DAQ_VERDICT_PASS,       /* DAQ_VERDICT_WHITELIST */
    DAQ_VERDICT_BLOCK,      /* DAQ_VERDICT_BLACKLIST */
    DAQ_VERDICT_PASS,       /* DAQ_VERDICT_IGNORE */
    DAQ_VERDICT_BLOCK       /* DAQ_VERDICT_RETRY */
};
