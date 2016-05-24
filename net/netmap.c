/*
 * netmap access for qemu
 *
 * Copyright (c) 2012-2013 Luigi Rizzo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/mman.h>
#define NETMAP_WITH_LIBS
#include <net/netmap.h>
#include <net/netmap_user.h>

#include "net/net.h"
#include "net/tap.h"
#include "clients.h"
#include "sysemu/sysemu.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"

#ifdef CONFIG_NETMAP_PASSTHROUGH
#include "hw/net/ptnetmap.h"
#endif /* CONFIG_NETMAP_PASSTHROUGH */

typedef struct NetmapState {
    NetClientState      nc;
    struct nm_desc      *nmd;
    char                ifname[IFNAMSIZ];
    struct netmap_ring  *tx;
    struct netmap_ring  *rx;
    bool                read_poll;
    bool                write_poll;
    struct iovec        iov[IOV_MAX];
    int                 vnet_hdr_len;  /* Current virtio-net header length. */
    QTAILQ_ENTRY(NetmapState) next;
#ifdef CONFIG_NETMAP_PASSTHROUGH
    PTNetmapState       ptnetmap;
#endif /* CONFIG_NETMAP_PASSTHROUGH */
} NetmapState;

static QTAILQ_HEAD(, NetmapState) netmap_clients =
                   QTAILQ_HEAD_INITIALIZER(netmap_clients);

#ifndef __FreeBSD__
#define pkt_copy bcopy
#else
/* A fast copy routine only for multiples of 64 bytes, non overlapped. */
static inline void
pkt_copy(const void *_src, void *_dst, int l)
{
    const uint64_t *src = _src;
    uint64_t *dst = _dst;
    if (unlikely(l >= 1024)) {
        bcopy(src, dst, l);
        return;
    }
    for (; l > 0; l -= 64) {
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
    }
}
#endif /* __FreeBSD__ */

/*
 * find nm_desc parent with same allocator
 */
static struct nm_desc *
netmap_find_memory(struct nm_desc *nmd)
{
    NetmapState *s;

    QTAILQ_FOREACH(s, &netmap_clients, next) {
        if (nmd->req.nr_arg2 == s->nmd->req.nr_arg2) {
            D("Found parent: ifname: %s mem_id: %d", s->ifname,
              s->nmd->req.nr_arg2);
            return s->nmd;
        }
    }

    return NULL;
}

/*
 * Open a netmap device. We assume there is only one queue
 * (which is the case for the VALE bridge).
 */
static struct nm_desc *netmap_open(const NetdevNetmapOptions *nm_opts,
                                   Error **errp)
{
    struct nm_desc *nmd;
    struct nmreq req;
    int ret;

    memset(&req, 0, sizeof(req));
#ifdef CONFIG_NETMAP_PASSTHROUGH
    if (nm_opts->passthrough) {
        req.nr_flags |= NR_PTNETMAP_HOST;
    }
#endif /* CONFIG_NETMAP_PASSTHROUGH */

    nmd = nm_open(nm_opts->ifname, &req, NETMAP_NO_TX_POLL | NM_OPEN_NO_MMAP,
                  NULL);
    if (nmd == NULL) {
        error_setg_errno(errp, errno, "Failed to nm_open() %s",
                         nm_opts->ifname);
        return NULL;
    }

    /* Check if we already have a nm_desc that uses the same memory as the one
     * just opened, so that nm_mmap() can skip mmap() and inherit from parent.
     */
    ret = nm_mmap(nmd, netmap_find_memory(nmd));
    if (ret) {
        error_setg_errno(errp, errno, "Failed to nm_mmap() %s",
                         nm_opts->ifname);
        nm_close(nmd);
        return NULL;
    }

    return nmd;
}

static void netmap_send(void *opaque);
static void netmap_writable(void *opaque);

/* Set the event-loop handlers for the netmap backend. */
static void netmap_update_fd_handler(NetmapState *s)
{
    qemu_set_fd_handler(s->nmd->fd,
                        s->read_poll ? netmap_send : NULL,
                        s->write_poll ? netmap_writable : NULL,
                        s);
}

/* Update the read handler. */
static void netmap_read_poll(NetmapState *s, bool enable)
{
    if (s->read_poll != enable) { /* Do nothing if not changed. */
        s->read_poll = enable;
        netmap_update_fd_handler(s);
    }
}

/* Update the write handler. */
static void netmap_write_poll(NetmapState *s, bool enable)
{
    if (s->write_poll != enable) {
        s->write_poll = enable;
        netmap_update_fd_handler(s);
    }
}

static void netmap_poll(NetClientState *nc, bool enable)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);

    if (s->read_poll != enable || s->write_poll != enable) {
        s->write_poll = enable;
        s->read_poll  = enable;
        netmap_update_fd_handler(s);
    }
}

/*
 * The fd_write() callback, invoked if the fd is marked as
 * writable after a poll. Unregister the handler and flush any
 * buffered packets.
 */
static void netmap_writable(void *opaque)
{
    NetmapState *s = opaque;

    netmap_write_poll(s, false);
    qemu_flush_queued_packets(&s->nc);
}

static ssize_t netmap_receive(NetClientState *nc,
      const uint8_t *buf, size_t size)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);
    struct netmap_ring *ring = s->tx;
    uint32_t i;
    uint32_t idx;
    uint8_t *dst;

    if (unlikely(!ring)) {
        /* Drop. */
        return size;
    }

    if (unlikely(size > ring->nr_buf_size)) {
        RD(5, "[netmap_receive] drop packet of size %d > %d\n",
                                    (int)size, ring->nr_buf_size);
        return size;
    }

    if (nm_ring_empty(ring)) {
        /* No available slots in the netmap TX ring. */
        netmap_write_poll(s, true);
        return 0;
    }

    i = ring->cur;
    idx = ring->slot[i].buf_idx;
    dst = (uint8_t *)NETMAP_BUF(ring, idx);

    ring->slot[i].len = size;
    ring->slot[i].flags = 0;
    pkt_copy(buf, dst, size);
    ring->cur = ring->head = nm_ring_next(ring, i);
    ioctl(s->nmd->fd, NIOCTXSYNC, NULL);

    return size;
}

static ssize_t netmap_receive_iov(NetClientState *nc,
                    const struct iovec *iov, int iovcnt)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);
    struct netmap_ring *ring = s->tx;
    uint32_t last;
    uint32_t idx;
    uint8_t *dst;
    int j;
    uint32_t i;

    if (unlikely(!ring)) {
        /* Drop the packet. */
        return iov_size(iov, iovcnt);
    }

    last = i = ring->cur;

    if (nm_ring_space(ring) < iovcnt) {
        /* Not enough netmap slots. */
        netmap_write_poll(s, true);
        return 0;
    }

    for (j = 0; j < iovcnt; j++) {
        int iov_frag_size = iov[j].iov_len;
        int offset = 0;
        int nm_frag_size;

        /* Split each iovec fragment over more netmap slots, if
           necessary. */
        while (iov_frag_size) {
            nm_frag_size = MIN(iov_frag_size, ring->nr_buf_size);

            if (unlikely(nm_ring_empty(ring))) {
                /* We run out of netmap slots while splitting the
                   iovec fragments. */
                netmap_write_poll(s, true);
                return 0;
            }

            idx = ring->slot[i].buf_idx;
            dst = (uint8_t *)NETMAP_BUF(ring, idx);

            ring->slot[i].len = nm_frag_size;
            ring->slot[i].flags = NS_MOREFRAG;
            pkt_copy(iov[j].iov_base + offset, dst, nm_frag_size);

            last = i;
            i = nm_ring_next(ring, i);

            offset += nm_frag_size;
            iov_frag_size -= nm_frag_size;
        }
    }
    /* The last slot must not have NS_MOREFRAG set. */
    ring->slot[last].flags &= ~NS_MOREFRAG;

    /* Now update ring->cur and ring->head. */
    ring->cur = ring->head = i;

    ioctl(s->nmd->fd, NIOCTXSYNC, NULL);

    return iov_size(iov, iovcnt);
}

/* Complete a previous send (backend --> guest) and enable the
   fd_read callback. */
static void netmap_send_completed(NetClientState *nc, ssize_t len)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);

    netmap_read_poll(s, true);
}

static void netmap_send(void *opaque)
{
    NetmapState *s = opaque;
    struct netmap_ring *ring = s->rx;

    /* Keep sending while there are available packets into the netmap
       RX ring and the forwarding path towards the peer is open. */
    while (!nm_ring_empty(ring)) {
        uint32_t i;
        uint32_t idx;
        bool morefrag;
        int iovcnt = 0;
        int iovsize;

        do {
            i = ring->cur;
            idx = ring->slot[i].buf_idx;
            morefrag = (ring->slot[i].flags & NS_MOREFRAG);
            s->iov[iovcnt].iov_base = (u_char *)NETMAP_BUF(ring, idx);
            s->iov[iovcnt].iov_len = ring->slot[i].len;
            iovcnt++;

            ring->cur = ring->head = nm_ring_next(ring, i);
        } while (!nm_ring_empty(ring) && morefrag);

        if (unlikely(nm_ring_empty(ring) && morefrag)) {
            RD(5, "[netmap_send] ran out of slots, with a pending"
                   "incomplete packet\n");
        }

        iovsize = qemu_sendv_packet_async(&s->nc, s->iov, iovcnt,
                                            netmap_send_completed);

        if (iovsize == 0) {
            /* The peer does not receive anymore. Packet is queued, stop
             * reading from the backend until netmap_send_completed()
             */
            netmap_read_poll(s, false);
            break;
        }
    }
}

/* Flush and close. */
static void netmap_cleanup(NetClientState *nc)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);

    qemu_purge_queued_packets(nc);

#ifdef CONFIG_NETMAP_PASSTHROUGH
    if (s->ptnetmap.running) {
        ptnetmap_delete(&s->ptnetmap);
    }
#endif /* CONFIG_NETMAP_PASSTHROUGH */

    netmap_poll(nc, false);
    nm_close(s->nmd);
    s->nmd = NULL;

    QTAILQ_REMOVE(&netmap_clients, s, next);
}

/* Offloading manipulation support callbacks. */
static bool netmap_has_ufo(NetClientState *nc)
{
    return true;
}

static bool netmap_has_vnet_hdr(NetClientState *nc)
{
    return true;
}

static int netmap_do_set_vnet_hdr_len(NetClientState *nc, int len)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);
    int err;
    struct nmreq req;

    /* Issue a NETMAP_BDG_VNET_HDR command to change the virtio-net header
     * length for the netmap adapter associated to 's->ifname'.
     */
    memset(&req, 0, sizeof(req));
    pstrcpy(req.nr_name, sizeof(req.nr_name), s->ifname);
    req.nr_version = NETMAP_API;
    req.nr_cmd = NETMAP_BDG_VNET_HDR;
    req.nr_arg1 = len;
    err = ioctl(s->nmd->fd, NIOCREGIF, &req);
    if (err) {
        error_report("Unable to execute NETMAP_BDG_VNET_HDR on %s: %s",
                     s->ifname, strerror(errno));
        return -1;
    }

    /* Keep track of the current length. */
    s->vnet_hdr_len = len;

    return 0;
}

static bool netmap_has_vnet_hdr_len(NetClientState *nc, int len)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);
    int prev_len = s->vnet_hdr_len;

    if (netmap_do_set_vnet_hdr_len(nc, len)) {
        return false;
    }

    netmap_do_set_vnet_hdr_len(nc, prev_len);

    return true;
}

static void netmap_using_vnet_hdr(NetClientState *nc, bool enable)
{
}

static void netmap_set_vnet_hdr_len(NetClientState *nc, int len)
{
    netmap_do_set_vnet_hdr_len(nc, len);
}

static void netmap_set_offload(NetClientState *nc, int csum, int tso4, int tso6,
                               int ecn, int ufo)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);

    /* Setting a virtio-net header length greater than zero automatically
     * enables the offloadings.
     */
    if (!s->vnet_hdr_len) {
        netmap_set_vnet_hdr_len(nc, sizeof(struct virtio_net_hdr));
    }
}

/* NetClientInfo methods */
static NetClientInfo net_netmap_info = {
    .type = NET_CLIENT_OPTIONS_KIND_NETMAP,
    .size = sizeof(NetmapState),
    .receive = netmap_receive,
    .receive_iov = netmap_receive_iov,
    .poll = netmap_poll,
    .cleanup = netmap_cleanup,
    .has_ufo = netmap_has_ufo,
    .has_vnet_hdr = netmap_has_vnet_hdr,
    .has_vnet_hdr_len = netmap_has_vnet_hdr_len,
    .using_vnet_hdr = netmap_using_vnet_hdr,
    .set_offload = netmap_set_offload,
    .set_vnet_hdr_len = netmap_set_vnet_hdr_len,
};

#ifdef CONFIG_NETMAP_PASSTHROUGH
/*
 * ptnetmap routines
 */

PTNetmapState *
get_ptnetmap(NetClientState *nc)
{
    NetmapState *s = DO_UPCAST(NetmapState, nc, nc);

    if (nc->info->type != NET_CLIENT_OPTIONS_KIND_NETMAP
                            || !(s->nmd->req.nr_flags & NR_PTNETMAP_HOST)) {
        return NULL;
    }

    ptnetmap_memdev_create(s->nmd->mem, s->nmd->memsize,
                           s->nmd->req.nr_arg2);

    return &s->ptnetmap;
}

/* Store and return the features we agree upon. */
uint32_t
ptnetmap_ack_features(PTNetmapState *ptn, uint32_t wanted_features)
{
    ptn->acked_features = ptn->features & wanted_features;

    return ptn->acked_features;
}

int
ptnetmap_get_netmap_if(PTNetmapState *ptn, NetmapIf *nif)
{
    NetmapState *s = ptn->netmap;

    memset(nif, 0, sizeof(*nif));

    if (s->nmd == NULL) {
        return EINVAL;
    }

    nif->nifp_offset = s->nmd->req.nr_offset;
    nif->num_tx_rings = s->nmd->req.nr_tx_rings;
    nif->num_rx_rings = s->nmd->req.nr_rx_rings;
    nif->num_tx_slots = s->nmd->req.nr_tx_slots;
    nif->num_rx_slots = s->nmd->req.nr_rx_slots;

    return 0;
}

int
ptnetmap_get_hostmemid(PTNetmapState *ptn)
{
    NetmapState *s = ptn->netmap;

    if (s->nmd == NULL) {
        return EINVAL;
    }

    return s->nmd->req.nr_arg2;
}

int
ptnetmap_create(PTNetmapState *ptn, struct ptnetmap_cfg *cfg)
{
    NetmapState *s = ptn->netmap;
    struct nmreq req;
    int err;

    if (!(ptn->acked_features & NET_PTN_FEATURES_BASE)) {
        error_report("ptnetmap features not acked");
        return EINVAL;
    }

    if (ptn->running) {
        return 0;
    }

    /* Tell QEMU not to poll the netmap fd. */
    netmap_poll(&s->nc, false);
    qemu_purge_queued_packets(&s->nc);

    /* Ask host netmap to create ptnetmap kthreads. */
    memset(&req, 0, sizeof(req));
    pstrcpy(req.nr_name, sizeof(req.nr_name), s->ifname);
    req.nr_version = NETMAP_API;
    ptnetmap_write_cfg(&req, cfg);
    req.nr_cmd = NETMAP_PT_HOST_CREATE;
    err = ioctl(s->nmd->fd, NIOCREGIF, &req);
    if (err) {
        error_report("Unable to execute NETMAP_PT_HOST_CREATE on %s: %s",
                     s->ifname, strerror(errno));
        netmap_poll(&s->nc, true);
        return err;
    }

    ptn->running = true;

    return 0;
}

int
ptnetmap_delete(PTNetmapState *ptn)
{
    NetmapState *s = ptn->netmap;
    int err;
    struct nmreq req;

    if (!(ptn->acked_features & NET_PTN_FEATURES_BASE)) {
        error_report("ptnetmap features not acked");
        return EFAULT;
    }

    if (!ptn->running) {
        return 0;
    }

    /* Ask host netmap to delete ptnetmap kthreads. */
    memset(&req, 0, sizeof(req));
    pstrcpy(req.nr_name, sizeof(req.nr_name), s->ifname);
    req.nr_version = NETMAP_API;
    req.nr_cmd = NETMAP_PT_HOST_DELETE;
    err = ioctl(s->nmd->fd, NIOCREGIF, &req);
    if (err) {
        error_report("Unable to execute NETMAP_PT_HOST_DELETE on %s: %s",
                s->ifname, strerror(errno));
    }

    /* Restore QEMU polling. */
    ptn->running = false;
    netmap_poll(&s->nc, true);

    return err;
}
#endif /* CONFIG_NETMAP_PASSTHROUGH */

/* The exported init function
 *
 * ... -net netmap,ifname="..."
 */
int net_init_netmap(const NetClientOptions *opts,
                    const char *name, NetClientState *peer, Error **errp)
{
    const NetdevNetmapOptions *netmap_opts = opts->u.netmap;
    struct nm_desc *nmd;
    NetClientState *nc;
    Error *err = NULL;
    NetmapState *s;

    nmd = netmap_open(netmap_opts, &err);
    if (err) {
        error_propagate(errp, err);
        return -1;
    }
    /* Create the object. */
    nc = qemu_new_net_client(&net_netmap_info, peer, "netmap", name);
    s = DO_UPCAST(NetmapState, nc, nc);
    s->nmd = nmd;
    s->tx = NETMAP_TXRING(nmd->nifp, 0);
    s->rx = NETMAP_RXRING(nmd->nifp, 0);
    s->vnet_hdr_len = 0;
    pstrcpy(s->ifname, sizeof(s->ifname), netmap_opts->ifname);
    QTAILQ_INSERT_TAIL(&netmap_clients, s, next);
#ifdef CONFIG_NETMAP_PASSTHROUGH
    if (netmap_opts->passthrough) {
        s->ptnetmap.netmap = s;
        s->ptnetmap.features = NET_PTN_FEATURES_BASE;
        s->ptnetmap.acked_features = 0;
        s->ptnetmap.running = false;

        if (netmap_has_vnet_hdr_len(nc, sizeof(struct virtio_net_hdr_v1))) {
            s->ptnetmap.features |= NET_PTN_FEATURES_VNET_HDR;
        }
    }
#endif /* CONFIG_NETMAP_PASSTHROUGH */
    netmap_read_poll(s, true); /* Initially only poll for reads. */


    return 0;
}

