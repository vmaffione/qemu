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

#define WITH_D	/* include debugging macros from qemu-common.h */

#include "config-host.h"

/* note paths are different for -head and 1.3 */
#include "net/net.h"
#include "clients.h"
#include "sysemu/sysemu.h"
#include "qemu-common.h"
#include "qemu/error-report.h"

#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/mman.h>
#include <net/netmap.h>
#include <net/netmap_user.h>
#include <net/checksum.h>
#include <qemu/iov.h>


/* XXX Use at your own risk: a synchronization problem in the netmap module
   can freeze your (host) machine. */
//#define USE_INDIRECT_BUFFERS

/*
 * private netmap device info
 */
struct netmap_state {
    int                 fd;
    int                 memsize;
    void                *mem;
    struct netmap_if    *nifp;
    struct netmap_ring  *rx;
    struct netmap_ring  *tx;
    char                fdname[128];        /* normally /dev/netmap */
    char                ifname[128];        /* maybe the nmreq here ? */
};

struct nm_state {
    NetClientState      nc;
    struct netmap_state me;
    unsigned int        read_poll;
    unsigned int        write_poll;
    PeerAsyncCallback	*txsync_callback;
    void		*txsync_callback_arg;
};

#ifndef __FreeBSD__
#define pkt_copy bcopy
#else
/* a fast copy routine only for multiples of 64 bytes, non overlapped. */
static inline void
pkt_copy(const void *_src, void *_dst, int l)
{
    const uint64_t *src = _src;
    uint64_t *dst = _dst;
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)       __builtin_expect(!!(x), 0)
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
 * open a netmap device. We assume there is only one queue
 * (which is the case for the VALE bridge).
 */
static int netmap_open(struct netmap_state *me)
{
    int fd, err;
    size_t l;
    struct nmreq req;

    me->fd = fd = open(me->fdname, O_RDWR);
    if (fd < 0) {
        error_report("Unable to open netmap device '%s'", me->fdname);
        return -1;
    }
    bzero(&req, sizeof(req));
    pstrcpy(req.nr_name, sizeof(req.nr_name), me->ifname);
    req.nr_ringid = NETMAP_NO_TX_POLL;
    req.nr_version = NETMAP_API;
    err = ioctl(fd, NIOCGINFO, &req);
    if (err) {
        error_report("cannot get info on %s", me->ifname);
        goto error;
    }
    l = me->memsize = req.nr_memsize;
    err = ioctl(fd, NIOCREGIF, &req);
    if (err) {
        error_report("Unable to register %s", me->ifname);
        goto error;
    }

    me->mem = mmap(0, l, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
    if (me->mem == MAP_FAILED) {
        error_report("Unable to mmap");
        me->mem = NULL;
        goto error;
    }

    me->nifp = NETMAP_IF(me->mem, req.nr_offset);
    me->tx = NETMAP_TXRING(me->nifp, 0);
    me->rx = NETMAP_RXRING(me->nifp, 0);
    return 0;

error:
    close(me->fd);
    return -1;
}

/* XXX do we need the can-send routine ? */
static int netmap_can_send(void *opaque)
{
    struct nm_state *s = opaque;

    return qemu_can_send_packet(&s->nc);
}

static void netmap_send(void *opaque);
static void netmap_writable(void *opaque);

/*
 * set the handlers for the device
 */
static void netmap_update_fd_handler(struct nm_state *s)
{
    qemu_set_fd_handler2(s->me.fd,
                         s->read_poll  ? netmap_can_send : NULL,
                         s->read_poll  ? netmap_send     : NULL,
                         s->write_poll ? netmap_writable : NULL,
                         s);
}

/* update the read handler */
static void netmap_read_poll(struct nm_state *s, bool enable)
{
    if (s->read_poll != enable) { /* do nothing if not changed */
        s->read_poll = enable;
        netmap_update_fd_handler(s);
    }
}

/* update the write handler */
static void netmap_write_poll(struct nm_state *s, bool enable)
{
    if (s->write_poll != enable) {
        s->write_poll = enable;
        netmap_update_fd_handler(s);
    }
}

static void netmap_poll(NetClientState *nc, bool enable)
{
    struct nm_state *s = DO_UPCAST(struct nm_state, nc, nc);

    if (s->read_poll != enable || s->write_poll != enable) {
        s->read_poll = enable;
        s->read_poll = enable;
        netmap_update_fd_handler(s);
    }
}

/*
 * the fd_write() callback, invoked if the fd is marked as
 * writable after a poll. Reset the handler and flush any
 * buffered packets.
 */
static void netmap_writable(void *opaque)
{
    struct nm_state *s = opaque;

    netmap_write_poll(s, false);
    qemu_flush_queued_packets(&s->nc);
}

/*
 * new data guest --> backend
 */
static ssize_t netmap_receive_flags(NetClientState *nc,
      const uint8_t *buf, size_t size, unsigned flags)
{
    struct nm_state *s = DO_UPCAST(struct nm_state, nc, nc);
    struct netmap_ring *ring = s->me.tx;

    if (size > ring->nr_buf_size) {
        RD(5, "drop packet of size %d > %d", (int)size, ring->nr_buf_size);
        return size;
    }

    if (ring) {
        /* request an early notification to avoid running dry */
        if (ring->avail < ring->num_slots / 2 && s->write_poll == false) {
            netmap_write_poll(s, true);
        }
        if (ring->avail == 0) { /* cannot write */
            return 0;
        }
        uint32_t i = ring->cur;
        uint32_t idx = ring->slot[i].buf_idx;
        uint8_t *dst = (uint8_t *)NETMAP_BUF(ring, idx);

        ring->slot[i].len = size;

#ifdef USE_INDIRECT_BUFFERS
	ring->slot[i].flags = NS_INDIRECT;
	*((const uint8_t **)dst) = buf;
#else
	ring->slot[i].flags = 0;    /* XXX useless? */
        pkt_copy(buf, dst, size);
#endif
        ring->cur = NETMAP_RING_NEXT(ring, i);
        ring->avail--;
        if (ring->avail == 0 || !(flags & QEMU_NET_PACKET_FLAG_MORE)) {
	    /* XXX should we require s->txsync_callback != NULL when
		QEMU_NET_PACKET_FLAG_MORE is set? There could be semantic
		problems, because the frontend expects the packet to be gone?
	    */
            ioctl(s->me.fd, NIOCTXSYNC, NULL);
	    if (s->txsync_callback) {
		s->txsync_callback(s->txsync_callback_arg);
	    }
	}
    }
    return size;
}

static ssize_t netmap_receive_iov_flags(NetClientState * nc,
	    const struct iovec * iov, int iovcnt, unsigned flags)
{
    struct nm_state *s = DO_UPCAST(struct nm_state, nc, nc);
    struct netmap_ring *ring = s->me.tx;
    size_t size = iov_size(iov, iovcnt);

    if (ring) {
        /* request an early notification to avoid running dry */
        if (ring->avail < ring->num_slots / 2 && s->write_poll == false) {
            netmap_write_poll(s, true);
        }
        if (ring->avail < iovcnt) { /* cannot write */
            return 0;
        }
        uint32_t i = 0;
        uint32_t idx;
        uint8_t *dst;
	int j;
	int frag_size;

	for (j=0; j<iovcnt; j++) {
	    frag_size = iov[j].iov_len;

	    if (frag_size > ring->nr_buf_size) {
		/* TODO recover descriptors (if possible) */
		RD(5, "drop packet of size %d > %d", (int)size, ring->nr_buf_size);
		return size;
	    }

	    i = ring->cur;
	    idx = ring->slot[i].buf_idx;
	    dst = (uint8_t *)NETMAP_BUF(ring, idx);

	    ring->slot[i].len = frag_size;
#ifdef USE_INDIRECT_BUFFERS
	    ring->slot[i].flags = NS_MOREFRAG | NS_INDIRECT;
	    *((const uint8_t **)dst) = iov[j].iov_base;
#else	/* !MAP_RING */
	    ring->slot[i].flags = NS_MOREFRAG;
	    pkt_copy(iov[j].iov_base, dst, frag_size);
#endif	/* !MAP_RING */

	    ring->cur = NETMAP_RING_NEXT(ring, i);
	    ring->avail--;
	}
	/* The last slot must not have NS_MOREFRAG set. */
	ring->slot[i].flags &= ~NS_MOREFRAG;

	/* Compute the checksum if necessary.
	if (flags & QEMU_NET_PACKET_FLAG_NEED_CHECKSUM)
	    net_checksum_calculate(dst, size); */

        if (ring->avail == 0 || !(flags & QEMU_NET_PACKET_FLAG_MORE)) {
            ioctl(s->me.fd, NIOCTXSYNC, NULL);
	    if (s->txsync_callback) {
		s->txsync_callback(s->txsync_callback_arg);
	    }
	}
    }
    return size;
}

static ssize_t netmap_receive_raw(NetClientState *nc,
      const uint8_t *buf, size_t size)
{
	return netmap_receive_flags(nc, buf, size, 0);
}

/* complete a previous send (backend --> guest), enable the fd_read callback */
static void netmap_send_completed(NetClientState *nc, ssize_t len)
{
    struct nm_state *s = DO_UPCAST(struct nm_state, nc, nc);

    netmap_read_poll(s, true);
}

/*
 * netmap_send: backend -> guest
 * there is traffic available from the network, try to send it up.
 */
static void netmap_send(void *opaque)
{
    struct nm_state *s = opaque;
    struct netmap_ring *ring = s->me.rx;
    struct iovec iov[64]; /* XXX max size? */

    /* only check ring->avail, let the packet be queued
     * with qemu_send_packet_async() if needed
     * XXX until we fix the propagation on the bridge we need to stop early
     */
    while (ring->avail > 0 && qemu_can_send_packet(&s->nc)) {
        uint32_t i;
        uint32_t idx;
	bool morefrag;
	int iovcnt = 0;
        int iovsize = 0;

	do {
	    i = ring->cur;
	    idx = ring->slot[i].buf_idx;
	    morefrag = (ring->slot[i].flags & NS_MOREFRAG);
	    iov[iovcnt].iov_base = (u_char *)NETMAP_BUF(ring, idx);
	    iov[iovcnt].iov_len = ring->slot[i].len;
	    iovsize += iov[iovcnt].iov_len;
	    iovcnt++;

	    ring->cur = NETMAP_RING_NEXT(ring, i);
	    ring->avail--;
	} while (ring->avail && morefrag);

	if (unlikely(!ring->avail && morefrag)) {
	    D("no more slot, but incomplete packet\n");
	}

	iovsize = qemu_sendv_packet_async(&s->nc, iov, iovcnt,
						    netmap_send_completed);

        if (iovsize == 0) {
            /* the guest does not receive anymore. Packet is queued, stop
             * reading from the backend until netmap_send_completed()
             */
            netmap_read_poll(s, false);
            return;
        }
    }
    netmap_read_poll(s, true); /* probably useless. */
}


static void netmap_register_peer_async_callback(NetClientState *nc,
		    PeerAsyncCallback *cb, void *opaque)
{
    struct nm_state *s = DO_UPCAST(struct nm_state, nc, nc);

    s->txsync_callback = cb;
    s->txsync_callback_arg = opaque;
}

/* flush and close */
static void netmap_cleanup(NetClientState *nc)
{
    struct nm_state *s = DO_UPCAST(struct nm_state, nc, nc);

    qemu_purge_queued_packets(nc);

    netmap_poll(nc, false);
    munmap(s->me.mem, s->me.memsize);
    close(s->me.fd);

    s->me.fd = -1;
}



/* fd support */

static NetClientInfo net_netmap_info = {
    .type = NET_CLIENT_OPTIONS_KIND_NETMAP,
    .size = sizeof(struct nm_state),
    .receive_flags = netmap_receive_flags,
    .receive = netmap_receive_raw,
    .receive_iov_flags = netmap_receive_iov_flags,
#if 0 /* not implemented */
    .receive_raw = netmap_receive_raw,
    .receive_iov = netmap_receive_iov,
#endif
    .poll = netmap_poll,
    .register_peer_async_callback = netmap_register_peer_async_callback,
    .cleanup = netmap_cleanup,
};

/* the external calls */

/*
 * ... -net netmap,ifname="..."
 */
int net_init_netmap(const NetClientOptions *opts,
        const char *name, NetClientState *peer)
{
    const NetdevNetmapOptions *netmap_opts = opts->netmap;
    NetClientState *nc;
    struct netmap_state me;
    struct nm_state *s;

    pstrcpy(me.fdname, sizeof(me.fdname), 
        netmap_opts->has_devname ? netmap_opts->devname : "/dev/netmap");
    /* set default name for the port if not supplied */
    pstrcpy(me.ifname, sizeof(me.ifname),
        netmap_opts->has_ifname ? netmap_opts->ifname : "vale0");
    if (netmap_open(&me)) {
        return -1;
    }
    /* create the object -- XXX use name or ifname ? */
    nc = qemu_new_net_client(&net_netmap_info, peer, "netmap", name);
    s = DO_UPCAST(struct nm_state, nc, nc);
    s->me = me;
    netmap_read_poll(s, true); /* initially only poll for reads. */
    s->txsync_callback = s->txsync_callback_arg = NULL;

    return 0;
}

