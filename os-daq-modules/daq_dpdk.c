#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <daq_api.h>
#include <sfbpf.h>
#include <sfbpf_dlt.h>

#include "daq_dpdk.h"

#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256

#define RX_RING_NUM 1
#define TX_RING_NUM 1

#define RX_RING_SIZE 256
#define TX_RING_SIZE 1024



static const struct rte_eth_conf port_conf_default = {
    .rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

typedef struct _dpdk_port
{
    #define DPDKINST_STARTED	0x1
    struct _dpdk_port* next;
    uint32_t flags;
    int rx_rings;
    int tx_rings;
    int port;
    int tx_start;
    int tx_end;
    int refcnt;
    struct rte_mempool *mbuf_pool;
    struct rte_mbuf *tx_burst[BURST_SIZE * RX_RING_NUM];
} DpdkPort;

#define FOR_EACH_PORTS(list,v) \
    DpdkPort* v; \
    for (v = list; v; v = v->next)

typedef struct _dpdk_instance
{
    struct _dpdk_instance *next;
    struct _dpdk_instance *peer;
    DpdkPort* port;
    int index;
    int queue;
} DpdkInstance;

typedef struct _dpdk_context
{
    char *device;
    char *filter;
    int snaplen;
    int timeout;
    int debug;
    DpdkInstance *instances;
    DpdkPort *ports;
    int intf_count;
    struct sfbpf_program fcode;
    volatile int break_loop;
    int promisc_flag;
    DAQ_Stats_t stats;
    DAQ_State state;
    char errbuf[256];
} Dpdk_Context_t;

static void dpdk_daq_reset_stats(void *handle);

static int start_port(Dpdk_Context_t *dpdkc, DpdkPort *instance)
{
    int rx_rings = RX_RING_NUM, tx_rings = TX_RING_NUM;
    int rx_ring_start = 0, tx_ring_start = 0;
    struct rte_eth_conf port_conf = port_conf_default;
    int port, queue, ret;

    port = instance->port;

    ret = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (ret != 0)
    {
        DPE(dpdkc->errbuf, "%s: Cannot configure port %d\n", __FUNCTION__, port);
        return DAQ_ERROR;
    }

    instance->rx_rings = rx_rings;
    instance->tx_rings = tx_rings;

    for (queue = rx_ring_start; queue < rx_ring_start + rx_rings; queue++)
    {
        ret = rte_eth_rx_queue_setup(port, queue, RX_RING_SIZE,
                rte_eth_dev_socket_id(port),
                NULL, instance->mbuf_pool);
        if (ret != 0)
        {
            DPE(dpdkc->errbuf, "%s: Cannot setup rx queue %d for port %d\n", __FUNCTION__, queue, port);
            return DAQ_ERROR;
        }
    }

    for (queue = tx_ring_start; queue < tx_ring_start + tx_rings; queue++)
    {
        ret = rte_eth_tx_queue_setup(port, queue, TX_RING_SIZE,
                rte_eth_dev_socket_id(port),
                NULL);
        if (ret != 0)
        {
            DPE(dpdkc->errbuf, "%s: Cannot setup tx queue %d for port %d\n", __FUNCTION__, queue, port);
            return DAQ_ERROR;
        }
    }

    ret = rte_eth_dev_start(instance->port);
    if (ret != 0)
    {
        DPE(dpdkc->errbuf, "%s: Cannot start device for port %d\n", __FUNCTION__, port);
        return DAQ_ERROR;
    }

    instance->flags |= DPDKINST_STARTED;

    if (dpdkc->promisc_flag)
        rte_eth_promiscuous_enable(instance->port);

    return DAQ_SUCCESS;
}

static void destroy_instance(DpdkInstance *instance)
{
    if (instance)
    {
        free(instance);
    }
}

static void destroy_port(DpdkPort *port)
{
    int i;

    if (port)
    {
        if (port->flags & DPDKINST_STARTED)
        {
            for (i = port->tx_start; i < port->tx_end; i++)
                rte_pktmbuf_free(port->tx_burst[i]);

            rte_eth_dev_stop(port->port);
            port->flags &= ~DPDKINST_STARTED;
        }

        free(port);
    }
}

static DpdkInstance *create_instance(const char *device, DpdkInstance **instances_ptr, DpdkPort **ports_ptr, char *errbuf, size_t errlen)
{
    DpdkInstance *instance = NULL;
    DpdkPort *dpdk_port;
    int port;
    char poolname[64];
    static int index = 0;

    if (strncmp(device, "dpdk", 4) != 0 || sscanf(&device[4], "%d", &port) != 1)
    {
        snprintf(errbuf, errlen, "%s: Invalid interface specification: '%s'!", __FUNCTION__, device);
        goto err;
    }

    instance = calloc(1, sizeof(DpdkInstance));
    if (!instance)
    {
        snprintf(errbuf, errlen, "%s: Cannot allocate a new instance structure.", __FUNCTION__);
        goto err;
    }

    instance->index = index;
    index++;

    //Search for a given queue index in the port def
    const char* queue_str = &device[4];
    int port_sz = port;
    while (port_sz > 0) {
        queue_str++;
        port_sz /= 10;
    }
    int queue;
    if (*queue_str == '-' && sscanf(queue_str + 1, "%d", &queue) == 1) {
        instance->queue = queue;
    } else {
        instance->queue = 0;
    }

    //Search if that DPDK port is already opened
    FOR_EACH_PORTS(*ports_ptr,existing_port)
    {
        if (existing_port->port == port) {
            instance->port = existing_port;
            instance->port->refcnt++;
            printf("Port %d already opened !\n",port);
            goto found;
        }
    }

    //The port does noti exist, create it
    dpdk_port = calloc(1, sizeof(DpdkPort));
    if (!dpdk_port)
    {
        snprintf(errbuf, errlen, "%s: Cannot allocate a new port structure.", __FUNCTION__);
        goto err;
    }

    dpdk_port->port = port;
    dpdk_port->refcnt = 1;

    snprintf(poolname, sizeof(poolname), "MBUF_POOL%d", port);
    dpdk_port->mbuf_pool = rte_pktmbuf_pool_create(poolname, NUM_MBUFS,
                MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (dpdk_port->mbuf_pool == NULL)
    {
        snprintf(errbuf, errlen, "%s: Cannot create mbuf pool!\n",
                __FUNCTION__);
        goto err;
    }

    dpdk_port->next = *ports_ptr;
    *ports_ptr = dpdk_port;
    instance->port = dpdk_port;

found:

    instance->next = *instances_ptr;
    *instances_ptr = instance;
    return instance;

err:
    destroy_instance(instance);
    return NULL;
}

static int create_bridge(Dpdk_Context_t *dpdkc, const int port1, const int port2)
{
    DpdkInstance *peer1, *peer2;

    peer1 = peer2 = NULL;
    FOR_EACH_INSTANCES(dpdkc->instances, instance) {
        printf("Instance id %d, port %d, port 1 %d, port2 %d",instance->index, instance->port->port, port1, port1);
        if (!peer1 && instance->port->port == port1)
            peer1 = instance;
        else if (instance->port->port == port2)
            peer2 = instance;
    }

    if (!peer1 || !peer2)
        return DAQ_ERROR_NODEV;

    peer1->peer = peer2;
    peer2->peer = peer1;

    return DAQ_SUCCESS;
}

static int dpdk_close(Dpdk_Context_t *dpdkc)
{
    DpdkInstance *instance;
    DpdkPort *port;

    if (!dpdkc)
        return -1;

    /* Free all of the device instances. */
    while ((instance = dpdkc->instances) != NULL)
    {
        dpdkc->instances = instance->next;
        destroy_instance(instance);
    }

    /* Free all of the device instances. */
    while ((port = dpdkc->ports) != NULL)
    {
        dpdkc->ports = port->next;
        destroy_port(port);
    }

    sfbpf_freecode(&dpdkc->fcode);
	
    dpdkc->state = DAQ_STATE_STOPPED;

    return 0;
}

static int parse_args(char *inputstring, char **argv)
{
    char **ap;

    for (ap = argv; (*ap = strsep(&inputstring, " \t")) != NULL;)
    {
        if (**ap != '\0')
            if (++ap >= &argv[MAX_ARGS])
                break;
    }

    return ap - argv;
}

static int dpdk_daq_initialize(const DAQ_Config_t *config, void **ctxt_ptr, char *errbuf, size_t errlen)
{
    Dpdk_Context_t *dpdkc;
    DpdkInstance *instance;
    DAQ_Dict *entry;
    char intf[IFNAMSIZ];
    int num_intfs = 0;
    int port1, port2, ports;
    size_t len;
    char *dev;
    int ret, rval = DAQ_ERROR;
    char *dpdk_args = NULL;
    char argv0[] = "fake";
    char *argv[MAX_ARGS + 1];
    int argc;

    dpdkc = calloc(1, sizeof(Dpdk_Context_t));
    if (!dpdkc)
    {
        snprintf(errbuf, errlen, "%s: Couldn't allocate memory for the new DPDK context!", __FUNCTION__);
        rval = DAQ_ERROR_NOMEM;
        goto err;
    }

    dpdkc->device = strdup(config->name);
    if (!dpdkc->device)
    {
        snprintf(errbuf, errlen, "%s: Couldn't allocate memory for the device string!", __FUNCTION__);
        rval = DAQ_ERROR_NOMEM;
        goto err;
    }

    dpdkc->snaplen = config->snaplen;
    dpdkc->timeout = (config->timeout > 0) ? (int) config->timeout : -1;
    dpdkc->promisc_flag = (config->flags & DAQ_CFG_PROMISC);

    /* Import the DPDK arguments */
    for (entry = config->values; entry; entry = entry->next)
    {
        if (!strcmp(entry->key, "dpdk_args"))
            dpdk_args = entry->value;
    }

    if (!dpdk_args)
    {
        snprintf(errbuf, errlen, "%s: Missing EAL arguments!", __FUNCTION__);
        rval = DAQ_ERROR_INVAL;
        goto err;
    }

    argv[0] = argv0;
    argc = parse_args(dpdk_args, &argv[1]) + 1;
    optind = 1;
    snprintf(errbuf, errlen, "%s: Hello!\n",
             __FUNCTION__);

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
    {
        snprintf(errbuf, errlen, "%s: Invalid EAL arguments!\n",
                __FUNCTION__);
        rval = DAQ_ERROR_INVAL;
        goto err;
    }

    ports = rte_eth_dev_count();
    if (ports == 0)
    {
        snprintf(errbuf, errlen, "%s: No Ethernet ports!\n", __FUNCTION__);
        rval = DAQ_ERROR_NODEV;
        goto err;
    }

    dev = dpdkc->device;
    if (*dev == ':' || ((len = strlen(dev)) > 0 && *(dev + len - 1) == ':') ||
            (config->mode == DAQ_MODE_PASSIVE && strstr(dev, "::")))
    {
        snprintf(errbuf, errlen, "%s: Invalid interface specification: '%s'!", __FUNCTION__, dpdkc->device);
        goto err;
    }

    while (*dev != '\0')
    {
        len = strcspn(dev, ":");
        if (len >= sizeof(intf))
        {
            snprintf(errbuf, errlen, "%s: Interface name too long! (%zu)", __FUNCTION__, len);
            goto err;
        }
        if (len != 0)
        {
            dpdkc->intf_count++;
            if (dpdkc->intf_count > ports)
            {
                snprintf(errbuf, errlen, "%s: Using more than %d interfaces is not valid!",
                        __FUNCTION__, ports);
                goto err;
            }
            snprintf(intf, len + 1, "%s", dev);
            instance = create_instance(intf, &dpdkc->instances, &dpdkc->ports, errbuf, errlen);
            if (!instance)
                goto err;

            num_intfs++;
            if (config->mode != DAQ_MODE_PASSIVE)
            {
                if (num_intfs == 2)
                {
                    port1 = dpdkc->instances->next->port->port;
                    port2 = dpdkc->instances->port->port;

                    if (create_bridge(dpdkc, port1, port2) != DAQ_SUCCESS)
                    {
                        snprintf(errbuf, errlen, "%s: Couldn't create the bridge between dpdk%d and dpdk%d!", __FUNCTION__, port1, port2);
                        goto err;
                    }
                    num_intfs = 0;
                } else if (num_intfs > 2)
                    break;
            }
        } else
            len = 1;
        dev += len;
    }

    /* If there are any leftover unbridged interfaces and we're not in Passive mode, error out. */
    if (!dpdkc->instances || (config->mode != DAQ_MODE_PASSIVE && num_intfs != 0))
    {
        snprintf(errbuf, errlen, "%s: Invalid interface specification: '%s'!",
                __FUNCTION__, dpdkc->device);
        goto err;
    }

    /* Initialize other default configuration values. */
    dpdkc->debug = 0;

    /* Import the configuration dictionary requests. */
    for (entry = config->values; entry; entry = entry->next)
    {
        if (!strcmp(entry->key, "debug"))
            dpdkc->debug = 1;
    }

    dpdkc->state = DAQ_STATE_INITIALIZED;

    *ctxt_ptr = dpdkc;
    return DAQ_SUCCESS;

err:
    if (dpdkc)
    {
        dpdk_close(dpdkc);
        if (dpdkc->device)
            free(dpdkc->device);
        free(dpdkc);
    }
    return rval;
}

static int dpdk_daq_set_filter(void *handle, const char *filter)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    struct sfbpf_program fcode;

    if (dpdkc->filter)
        free(dpdkc->filter);

    dpdkc->filter = strdup(filter);
    if (!dpdkc->filter)
    {
        DPE(dpdkc->errbuf, "%s: Couldn't allocate memory for the filter string!", __FUNCTION__);
        return DAQ_ERROR;
    }

    if (sfbpf_compile(dpdkc->snaplen, DLT_EN10MB, &fcode, dpdkc->filter, 1, 0) < 0)
    {
        DPE(dpdkc->errbuf, "%s: BPF state machine compilation failed!", __FUNCTION__);
        return DAQ_ERROR;
    }

    sfbpf_freecode(&dpdkc->fcode);

    dpdkc->fcode.bf_len = fcode.bf_len;
    dpdkc->fcode.bf_insns = fcode.bf_insns;

    return DAQ_SUCCESS;
}

static int dpdk_daq_start(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;

    FOR_EACH_PORTS(dpdkc->ports, port)
    {
        if (start_port(dpdkc, port) != DAQ_SUCCESS)
            return DAQ_ERROR;
    }

/*    FOR_EACH_INSTANCES(dpdkc->instances, instance)
    {
        if (start_instance(dpdkc, instance) != DAQ_SUCCESS)
            return DAQ_ERROR;
    }*/

    dpdk_daq_reset_stats(handle);

    dpdkc->state = DAQ_STATE_STARTED;

    return DAQ_SUCCESS;
}


static int dpdk_daq_acquire(void *handle, int cnt, DAQ_Analysis_Func_t callback, DAQ_Meta_Func_t metaback, void *user)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    DpdkInstance *peer;
    DpdkPort* port;
    DpdkPort* peer_port = NULL;
    DAQ_PktHdr_t daqhdr;
    DAQ_Verdict verdict;
    const uint8_t *data;
    uint16_t len;
    int c = 0, burst_size;
    int i, got_one, ignored_one, sent_one;
    int queue;
    struct timeval ts;

    gettimeofday(&ts, NULL);

    while (c < cnt || cnt <= 0)
    {
        struct rte_mbuf *bufs[BURST_SIZE];

        got_one = 0;
        ignored_one = 0;
        sent_one = 0;

        FOR_EACH_INSTANCES(dpdkc->instances, instance) {
            port = instance->port;


            /* Has breakloop() been called? */
            if (dpdkc->break_loop)
            {
                dpdkc->break_loop = 0;
                return 0;
            }

            peer = instance->peer;
            if (peer)
            {
                peer_port = peer->port;
                burst_size = peer_port->tx_end - peer_port->tx_start;
                if (burst_size > 0)
                    goto do_send_packet;
            }

            for (queue = 0; queue < port->rx_rings; queue++)
            {
                if (cnt <= 0 || cnt - c >= BURST_SIZE)
                    burst_size = BURST_SIZE;
                else
                    burst_size = cnt - c;

		//printf("2 port %d, queue %d, bufs %p, burst_size %d\n",instance->port,queue,bufs,burst_size);

                const uint16_t nb_rx =
                    rte_eth_rx_burst(port->port, instance->queue,
                            bufs, burst_size);

                if (unlikely(nb_rx == 0))
                    continue;

                for (i = 0; i < nb_rx; i++)
                {
                    verdict = DAQ_VERDICT_PASS;

                    data = rte_pktmbuf_mtod(bufs[i], void *);
                    len = rte_pktmbuf_data_len(bufs[i]);

                    dpdkc->stats.hw_packets_received++;

                    if (dpdkc->fcode.bf_insns &&
                            sfbpf_filter(dpdkc->fcode.bf_insns,
                                data, len, len) == 0)
                    {
                        ignored_one = 1;
                        dpdkc->stats.packets_filtered++;
                        goto send_packet;
                    }
                    got_one = 1;

                    daqhdr.ts = ts;
                    daqhdr.caplen = len;
                    daqhdr.pktlen = len;
                    daqhdr.ingress_index = instance->index;
                    daqhdr.egress_index = peer ? peer->index : DAQ_PKTHDR_UNKNOWN;
                    daqhdr.ingress_group = DAQ_PKTHDR_UNKNOWN;
                    daqhdr.egress_group = DAQ_PKTHDR_UNKNOWN;
                    daqhdr.flags = 0;
                    daqhdr.opaque = 0;
                    daqhdr.priv_ptr = NULL;
                    daqhdr.address_space_id = 0;

                    if (callback)
                    {
                        verdict = callback(user, &daqhdr, data);
                        if (verdict >= MAX_DAQ_VERDICT)
                            verdict = DAQ_VERDICT_PASS;
                        dpdkc->stats.verdicts[verdict]++;
                        verdict = verdict_translation_table[verdict];
                    }
                    dpdkc->stats.packets_received++;
                    c++;
send_packet:
                    if (verdict == DAQ_VERDICT_PASS && instance->peer)
                    {
                        peer_port->tx_burst[peer_port->tx_end] = bufs[i];
                        peer_port->tx_end++;
                    }
                    else
                    {
                        rte_pktmbuf_free(bufs[i]);
                    }
                }
            }

            if (peer)
            {
                burst_size = peer_port->tx_end - peer_port->tx_start;
                if (unlikely(burst_size == 0))
                    continue;
do_send_packet:
                for (queue = 0; burst_size != 0 && queue < peer_port->tx_rings; queue++)
                {
                    const uint16_t nb_tx = rte_eth_tx_burst(peer_port->port,
                            peer->queue,
                            &peer_port->tx_burst[peer_port->tx_start],
                            burst_size);

                    if (unlikely(nb_tx == 0))
                        continue;

                    sent_one = 1;
                    burst_size -= nb_tx;
                    peer_port->tx_start += nb_tx;
                }

                if (burst_size == 0)
                {
                    peer_port->tx_start = 0;
                    peer_port->tx_end = 0;
                }
            }
        }

        if ((!got_one && !ignored_one && !sent_one))
        {
            struct timeval now;

            if (dpdkc->timeout == -1)
                continue;

            /* If time out, return control to the caller. */
            gettimeofday(&now, NULL);
            if (now.tv_sec > ts.tv_sec ||
                    (now.tv_usec - ts.tv_usec) > dpdkc->timeout * 1000)
                return 0;
        }
    }

    return 0;
}

static int dpdk_daq_inject(void *handle, const DAQ_PktHdr_t *hdr, const uint8_t *packet_data, uint32_t len, int reverse)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    DpdkInstance *instance;

    struct rte_mbuf *m;

    /* Find the instance that the packet was received on. */
    for (instance = dpdkc->instances; instance; instance = instance->next)
    {
        if (instance->index == hdr->ingress_index)
            break;
    }

    if (!instance)
    {
        DPE(dpdkc->errbuf, "%s: Unrecognized ingress interface specified: %u",
                __FUNCTION__, hdr->ingress_index);
        return DAQ_ERROR_NODEV;
    }

    if (!reverse && !(instance = instance->peer))
    {
        DPE(dpdkc->errbuf, "%s: Specified ingress interface (%u) has no peer for forward injection.",
                __FUNCTION__, hdr->ingress_index);
        return DAQ_ERROR_NODEV;
    }

    m = rte_pktmbuf_alloc(instance->port->mbuf_pool);
    if (!m)
    {
        DPE(dpdkc->errbuf, "%s: Cannot allocate memory for packet.",
                __FUNCTION__);
        return DAQ_ERROR_NOMEM;
    }

    rte_memcpy(rte_pktmbuf_mtod(m, void *), packet_data, len);

    const uint16_t nb_tx = rte_eth_tx_burst(instance->port->port, instance->queue, &m, 1);

    if (unlikely(nb_tx == 0))
    {
        DPE(dpdkc->errbuf, "%s: Cannot send packet. Try again.", __FUNCTION__);
        rte_pktmbuf_free(m);
        return DAQ_ERROR_AGAIN;
    }

    return DAQ_SUCCESS;
}

static int dpdk_daq_breakloop(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;

    dpdkc->break_loop = 1;

    return DAQ_SUCCESS;
}

static int dpdk_daq_stop(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    dpdk_close(dpdkc);
    return DAQ_SUCCESS;
}

static void dpdk_daq_shutdown(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;

    dpdk_close(dpdkc);

    if (dpdkc->device)
        free(dpdkc->device);

    if (dpdkc->filter)
        free(dpdkc->filter);

    free(dpdkc);
}

static DAQ_State dpdk_daq_check_status(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    return dpdkc->state;
}

static int dpdk_daq_get_stats(void *handle, DAQ_Stats_t *stats)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    rte_memcpy(stats, &dpdkc->stats, sizeof(DAQ_Stats_t));
    return DAQ_SUCCESS;
}

static void dpdk_daq_reset_stats(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    memset(&dpdkc->stats, 0, sizeof(DAQ_Stats_t));
}

static int dpdk_daq_get_snaplen(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    return dpdkc->snaplen;
}

static uint32_t dpdk_daq_get_capabilities(void *handle)
{
    return DAQ_CAPA_BLOCK | DAQ_CAPA_REPLACE | DAQ_CAPA_INJECT |
        DAQ_CAPA_UNPRIV_START | DAQ_CAPA_BREAKLOOP | DAQ_CAPA_BPF |
        DAQ_CAPA_DEVICE_INDEX;
}

static int dpdk_daq_get_datalink_type(void *handle)
{
    return DLT_EN10MB;
}

static const char *dpdk_daq_get_errbuf(void *handle)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;

    return dpdkc->errbuf;
}

static void dpdk_daq_set_errbuf(void *handle, const char *string)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;

    if (!string)
        return;

    DPE(dpdkc->errbuf, "%s", string);
}

static int dpdk_daq_get_device_index(void *handle, const char *device)
{
    Dpdk_Context_t *dpdkc = (Dpdk_Context_t *) handle;
    int port;

    if (strncmp(device, "dpdk", 4) != 0 || sscanf(&device[4], "%d", &port) != 1)
        return DAQ_ERROR_NODEV;

    FOR_EACH_INSTANCES(dpdkc->instances, instance)
    {
        if (instance->port->port == port)
            return instance->index;
    }

    return DAQ_ERROR_NODEV;
}

#ifdef BUILDING_SO
DAQ_SO_PUBLIC const DAQ_Module_t DAQ_MODULE_DATA =
#else
const DAQ_Module_t dpdk_daq_module_data =
#endif
{
    /* .api_version = */ DAQ_API_VERSION,
    /* .module_version = */ DAQ_DPDK_VERSION,
    /* .name = */ "dpdk",
    /* .type = */ DAQ_TYPE_INLINE_CAPABLE | DAQ_TYPE_INTF_CAPABLE | DAQ_TYPE_MULTI_INSTANCE,
    /* .initialize = */ dpdk_daq_initialize,
    /* .set_filter = */ dpdk_daq_set_filter,
    /* .start = */ dpdk_daq_start,
    /* .acquire = */ dpdk_daq_acquire,
    /* .inject = */ dpdk_daq_inject,
    /* .breakloop = */ dpdk_daq_breakloop,
    /* .stop = */ dpdk_daq_stop,
    /* .shutdown = */ dpdk_daq_shutdown,
    /* .check_status = */ dpdk_daq_check_status,
    /* .get_stats = */ dpdk_daq_get_stats,
    /* .reset_stats = */ dpdk_daq_reset_stats,
    /* .get_snaplen = */ dpdk_daq_get_snaplen,
    /* .get_capabilities = */ dpdk_daq_get_capabilities,
    /* .get_datalink_type = */ dpdk_daq_get_datalink_type,
    /* .get_errbuf = */ dpdk_daq_get_errbuf,
    /* .set_errbuf = */ dpdk_daq_set_errbuf,
    /* .get_device_index = */ dpdk_daq_get_device_index,
    /* .modify_flow = */ NULL,
    /* .hup_prep = */ NULL,
    /* .hup_apply = */ NULL,
    /* .hup_post = */ NULL,
    /* .dp_add_dc = */ NULL
};
