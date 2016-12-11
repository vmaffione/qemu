/* Virtio prodcons driver.
 *
 * Copyright 2016 Vincenzo Maffione
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/wait.h>

#include "producer.h"

#define DBG
#undef DBG

/* Protected by a global lock. */
static int virtpc_devcnt = 0;
static LIST_HEAD(virtpc_devs);
DEFINE_MUTEX(lock);

struct virtpc_info {
    struct virtio_device        *vdev;
    struct list_head	        node;
    unsigned int		devid;
    bool			busy;

    wait_queue_head_t	        wqh;
    struct virtqueue	        *vq;
    unsigned int		wp;
    unsigned int		wc;
    unsigned int                yp;
    unsigned int                yc;
    unsigned int                psleep;
    unsigned int                csleep;
    unsigned int                incsp;
    unsigned int                incsc;
    unsigned int		duration;
    u64                         np_acc;
    u64                         np_cnt;
    u64                         wp_acc;
    u64                         wp_cnt;
    u64                         yp_acc;
    u64                         yp_cnt;
    u64                         next_dump;
    u64                         last_dump;
    struct scatterlist          out_sg;
    struct pcbuf                *bufs;
    unsigned int                nbufs;
    char			name[40];
};

struct virtpc_priv {
};

static u32 pkt_idx = 0;
static unsigned int event_idx = 0;
static struct pcevent events[VIRTIOPC_EVENTS];

/******************************* TSC support ***************************/

/* initialize to avoid a division by 0 */
static uint64_t ticks_per_second = 1000000000; /* set by calibrate_tsc */

#define NS2TSC(x) ((x)*ticks_per_second/1000000000UL)
#define TSC2NS(x) ((x)*1000000000UL/ticks_per_second)

/*
 * do an idle loop to compute the clock speed. We expect
 * a constant TSC rate and locked on all CPUs.
 * Returns ticks per second
 */
static uint64_t
calibrate_tsc(void)
{
    uint64_t a, b;
    uint64_t ta_0, ta_1, tb_0, tb_1, dmax = ~0;
    uint64_t da, db, cy = 0;
    int i;
    for (i=0; i < 3; i++) {
	ta_0 = rdtsc();
        a = ktime_get_ns();
	ta_1 = rdtsc();
	usleep_range(20000, 20000);
	tb_0 = rdtsc();
        b = ktime_get_ns();
	tb_1 = rdtsc();
	da = ta_1 - ta_0;
	db = tb_1 - tb_0;
	if (da + db < dmax) {
            cy = b - a;
	    cy = ((tb_0 - ta_1)*1000000000)/cy;
	    dmax = da + db;
	}
    }
    ticks_per_second = cy;
    return cy;
}

/***********************************************************************/

static void
virtio_pc_stats_reset(struct virtpc_info *vi)
{
    vi->np_acc = vi->np_cnt = vi->wp_acc = vi->wp_cnt =
        vi->yp_acc = vi->yp_cnt = 0;
    vi->last_dump = rdtsc();
    vi->next_dump = vi->last_dump + NS2TSC(5000000000);
}

static void
items_consumed(struct virtqueue *vq)
{
    struct virtpc_info *vi = vq->vdev->priv;

    /* Suppress further interrupts and wake up the producer. */
    virtqueue_disable_cb(vq);
    wake_up_interruptible(&vi->wqh);
}

static void
cleanup_items(struct virtpc_info *vi, int num)
{
    struct pcbuf *buf;
    unsigned int len;

    while (num && (buf = virtqueue_get_buf(vi->vq, &len)) != NULL) {
        --num;
#ifdef DBG
        printk("virtpc: virtqueue_get_buf --> %p\n", cookie);
#endif
    }
}

#define THR	3

static int
produce(struct virtpc_info *vi)
{
    struct virtqueue *vq = vi->vq;
    unsigned int idx = 0;
    u64 guard_ofs;
    u64 next;
    u64 finish;
    bool kick;
    struct pcbuf *buf;
    int err;
    u64 tsa, tsb, tsc, tsd;

    guard_ofs = NS2TSC(1000000);

    /* Compute finish time in stages, to avoid overflow of
     * the vi->duration and finish variable. */
    finish = vi->duration;
    finish = NS2TSC(finish);
    finish *= 1000000000;
    finish += rdtsc();

    cleanup_items(vi, ~0U);
    virtqueue_enable_cb(vq);

    tsb = rdtsc();
    next = tsb + vi->wp;

    for (;;) {

        if (unlikely(signal_pending(current) || next > finish)) {
            if (next > finish) {
                printk("virtpc: producer stops\n");
                return 0;
            }
            printk("signal received, returning\n");
            return -EAGAIN;
        }

        tsa = rdtsc();

        cleanup_items(vi, THR);

        /* Prepare the SG lists. */
        buf = vi->bufs + idx;
        buf->lat = tsa;
        sg_init_table(&vi->out_sg, 1);
        sg_set_buf(&vi->out_sg, buf, 2 * sizeof(u64));
        if (++idx >= vi->nbufs) {
            idx = 0;
        }

        while ((tsa = rdtsc()) < next) barrier();
        /* It may happen that we are preempted while we are
         * busy waiting. We detect this case by looking at the
         * clock when the busy waiting finishes. */
        if (unlikely(tsa - next > 3000)) {
            /* We were preempted while busy waiting. We need to
             * reset next, otherwise P would produce a burst which
             * may cause large bursts on the consumer, specially
             * when Wp is close to Wc (but fast consumer). We also
             * need to adjust tsb to fix Wp estimation. Note that
             * this "gap" causes a little offset between Tavg and
             * Tbatch. */
            tsb += tsa - next;
            next = tsa;
        }
        next += vi->wp;

        buf->sc = tsa - guard_ofs; /* We subtract guard_ofs (1 ms) to
                                    * give C a way to understand
                                    * that it didn't see the correct
                                    * timestamp set below */
        err = virtqueue_add_outbuf(vq, &vi->out_sg, 1, buf, GFP_ATOMIC);
        tsc = rdtsc();

        kick = virtqueue_kick_prepare(vq);
        if (kick) {
            virtqueue_notify(vq);
            tsd = rdtsc();
            buf->sc = tsd; /* ignore C double-check, assume C was blocked,
                           * and assume C starts after this point */
        }

        if (unlikely(err)) {
            printk("virtpc: add_outbuf() failed %d\n", err);
#ifdef DBG
        } else {
            printk("virtpc: virtqueue_add_outbuf --> %p\n", buf);
#endif
        }

        events[event_idx].ts = tsc;
        events[event_idx].id = pkt_idx;
        events[event_idx].type = VIRTIOPC_PKTPUB;
        VIRTIOPC_EVNEXT(event_idx);

        vi->wp_acc += tsc - tsb;
        tsb = tsc;
        vi->wp_cnt ++;

        if (kick) {
            vi->np_acc += tsd - tsc;
            vi->np_cnt ++;
            /* When the costly notification routine returns, we need to
             * reset next to correctly emulate the production of the
             * next item. */
            next += tsd - tsc;
            //next = tsd + vi->wp; /* alternative */
            events[event_idx].ts = tsd;
            events[event_idx].id = pkt_idx;
            events[event_idx].type = VIRTIOPC_P_NOTIFY_DONE;
            VIRTIOPC_EVNEXT(event_idx);
            tsb = tsd;
        }

        if (vq->num_free < THR) {
            events[event_idx].ts = rdtsc();
            events[event_idx].id = pkt_idx;
            events[event_idx].type = VIRTIOPC_P_STOPS;
            VIRTIOPC_EVNEXT(event_idx);

            if (vi->psleep) {
                do {
                    /* Taken from usleep_range */
                    ktime_t to;

                    tsa = rdtsc();
                    to = ktime_set(0, vi->yp);
                    __set_current_state(TASK_UNINTERRUPTIBLE);
                    schedule_hrtimeout_range(&to, 0, HRTIMER_MODE_REL);
                    tsb = rdtsc();
                    cleanup_items(vi, THR);
                    vi->yp_acc += tsb - tsa;
                    vi->yp_cnt ++;
                } while (vq->num_free < THR);

                next = tsb + vi->wp;

            } else {
                set_current_state(TASK_INTERRUPTIBLE);
                if (!virtqueue_enable_cb_delayed(vq)) {
                    /* More just got used, free them then recheck. */
                    cleanup_items(vi, THR);
                }
                if (vq->num_free >= THR) {
                    virtqueue_disable_cb(vq);
                    set_current_state(TASK_RUNNING);
                } else {
                    schedule();
                    /* We assume that after the wake up here at
                     * last one item will be recovered by the call to
                     * cleanup_items(). */
                    if (vi->incsp) {
                        next = rdtsc() + vi->incsp;
                        while (rdtsc() < next) barrier();
                    }

                    tsb = rdtsc();
                    next = tsb + vi->wp;
                }
            }
        }

        pkt_idx ++;

        if (unlikely(next > vi->next_dump)) {
            u64 ndiff = TSC2NS(rdtsc() - vi->last_dump);

            printk("PC: %llu np %llu wp %llu yp %llu sleeps/s\n",
                    TSC2NS(vi->np_cnt ? vi->np_acc / vi->np_cnt : 0),
                    TSC2NS(vi->wp_cnt ? vi->wp_acc / vi->wp_cnt : 0),
                    TSC2NS(vi->yp_cnt ? vi->yp_acc / vi->yp_cnt : 0),
                    vi->yp_cnt * 1000000000 / ndiff);

            virtio_pc_stats_reset(vi);
            tsb = rdtsc();
            next = tsb + vi->wp;
        }
    }

    cleanup_items(vi, ~0U);
    virtqueue_enable_cb(vq);

    return 0;
}

static int
virtpc_open(struct inode *inode, struct file *f)
{
    struct virtpc_priv *pc = kmalloc(sizeof(*pc), GFP_KERNEL);
    if (!pc) {
        return -ENOMEM;
    }
    f->private_data = pc;
    return 0;
}

static int
virtpc_release(struct inode *inode, struct file *f)
{
    struct virtpc_priv *pc = f->private_data;
    if (pc) {
        kfree(pc);
    }
    return 0;
}

static long
virtpc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct virtpc_priv *pc = f->private_data;
    void __user *argp = (void __user *)arg;
    struct virtpc_info *vi = NULL, *tmp;
    DECLARE_WAITQUEUE(wait, current);
    struct virtpc_ioctl_data pcio;
    int ret = 0;

    (void)cmd;
    (void)pc;

    if (copy_from_user(&pcio, argp, sizeof(pcio))) {
        return -EFAULT;
    }

    mutex_lock(&lock);
    list_for_each_entry(tmp, &virtpc_devs, node) {
        if (tmp->devid == pcio.devid) {
            vi = tmp;
            break;
        }
    }

    if (vi == NULL || vi->busy) {
        mutex_unlock(&lock);
        return vi ? -EBUSY : -ENXIO;
    }

    calibrate_tsc();

    vi->busy = true;
    vi->wp = NS2TSC(pcio.wp);
    vi->wc = NS2TSC(pcio.wc);
    vi->yp = pcio.yp;
    vi->yc = pcio.yc;
    vi->psleep = pcio.psleep;
    vi->csleep = pcio.csleep;
    vi->incsp = NS2TSC(pcio.incsp);
    vi->incsc = NS2TSC(pcio.incsc);
    vi->duration = pcio.duration;

    /* cf. with include/hw/virtio/virtio-prodcons.h */
    virtio_cwrite32(vi->vdev, 0 /* offset */, (uint32_t)vi->wp);
    virtio_cwrite32(vi->vdev, 4 /* offset */, (uint32_t)vi->wc);
    virtio_cwrite32(vi->vdev, 8 /* offset */, (uint32_t)vi->yp);
    virtio_cwrite32(vi->vdev, 12 /* offset */, (uint32_t)vi->yc);
    virtio_cwrite32(vi->vdev, 16 /* offset */, (uint32_t)vi->psleep);
    virtio_cwrite32(vi->vdev, 20 /* offset */, (uint32_t)vi->csleep);
    virtio_cwrite32(vi->vdev, 24 /* offset */, (uint32_t)vi->incsp);
    virtio_cwrite32(vi->vdev, 28 /* offset */, (uint32_t)vi->incsc);
    virtio_cwrite32(vi->vdev, 32 /* offset */, (uint32_t)0);

    printk("virtpc: set Wp=%uns\n", pcio.wp);
    printk("virtpc: set Wc=%uns\n", pcio.wc);
    printk("virtpc: set Yp=%uns\n", pcio.yp);
    printk("virtpc: set Yc=%uns\n", pcio.yc);
    printk("virtpc: set D=%uns\n", pcio.duration);

    virtio_pc_stats_reset(vi);
    pkt_idx = 0;

    mutex_unlock(&lock);

    /* We keep ourself in the wait queue all the time; there is no
     * point in paying the cost of dynamically adding/removing us from
     * the waitqueue, since we already suppress interrupts using the
     * virtqueue (and the waitqueue wakeup is called in the interrupt
     * routine). */
    add_wait_queue(&vi->wqh, &wait);
    ret = produce(vi);
    remove_wait_queue(&vi->wqh, &wait);

    mutex_lock(&lock);
    vi->busy = false;
    mutex_unlock(&lock);

    virtio_cwrite32(vi->vdev, 32 /* offset */, (uint32_t)1); /* stop */

    {
        unsigned int i;

        for (i = 0; i < VIRTIOPC_EVENTS; i ++) {
            trace_printk("%llu %u %u\n", events[i].ts,
                         events[i].id, events[i].type);
        }
    }

    return ret;
}

static void
virtpc_config_changed(struct virtio_device *vdev)
{
    struct virtpc_info *vi = vdev->priv;
    (void)vi;
}

static void
detach_unused_bufs(struct virtpc_info *vi)
{
    void *cookie;

    while ((cookie = virtqueue_detach_unused_buf(vi->vq)) != NULL) {
    }
}

static void
virtpc_del_vqs(struct virtpc_info *vi)
{
    struct virtio_device *vdev = vi->vdev;

    vdev->config->del_vqs(vdev);
}

static int
virtpc_find_vqs(struct virtpc_info *vi)
{
    vq_callback_t **callbacks;
    struct virtqueue **vqs;
    const char **names;
    int ret = -ENOMEM;
    int num_vqs;

    num_vqs = 1;

    /* Allocate space for find_vqs parameters. */
    vqs = kzalloc(num_vqs * sizeof(*vqs), GFP_KERNEL);
    if (!vqs)
        goto err_vq;
    callbacks = kmalloc(num_vqs * sizeof(*callbacks), GFP_KERNEL);
    if (!callbacks)
        goto err_callback;
    names = kmalloc(num_vqs * sizeof(*names), GFP_KERNEL);
    if (!names)
        goto err_names;

    /* Allocate/initialize parameters for virtqueues. */
    callbacks[0] = items_consumed;
    names[0] = vi->name;

    ret = vi->vdev->config->find_vqs(vi->vdev, num_vqs, vqs, callbacks,
            names);
    if (ret)
        goto err_find;

    vi->vq = vqs[0];

    kfree(names);
    kfree(callbacks);
    kfree(vqs);

    return 0;

err_find:
    kfree(names);
err_names:
    kfree(callbacks);
err_callback:
    kfree(vqs);
err_vq:
    return ret;
}

static void
remove_vq_common(struct virtpc_info *vi)
{
    vi->vdev->config->reset(vi->vdev);
    detach_unused_bufs(vi);
    virtpc_del_vqs(vi);
}

static const struct file_operations virtpc_fops = {
    .owner		= THIS_MODULE,
    .release	= virtpc_release,
    .open		= virtpc_open,
    .unlocked_ioctl	= virtpc_ioctl,
    .llseek		= noop_llseek,
};

static struct miscdevice virtpc_misc = {
    .minor		= MISC_DYNAMIC_MINOR,
    .name		= "virtio-pc",
    .fops		= &virtpc_fops,
};

static int
virtpc_probe(struct virtio_device *vdev)
{
    struct virtpc_info *vi;
    unsigned int devcnt;
    int err;

    if (!vdev->config->get) {
        dev_err(&vdev->dev, "%s failure: config access disabled\n",
                __func__);
        return -EINVAL;
    }

    mutex_lock(&lock);
    devcnt = virtpc_devcnt ++;
    mutex_unlock(&lock);

    if (devcnt == 0) {
        err = misc_register(&virtpc_misc);
        if (err) {
            printk("Failed to register miscdevice\n");
            return err;
        }
        printk("virtio-prodcons miscdevice registered\n");
    }

    vi = kzalloc(sizeof(*vi), GFP_KERNEL);
    if (!vi) {
        err = -ENOMEM;
        goto free_misc;
    }

    vi->vdev = vdev;
    vdev->priv = vi;
    vi->devid = devcnt;
    init_waitqueue_head(&vi->wqh);
    sprintf(vi->name, "virtio-pc-%d", vi->devid);

    err = virtpc_find_vqs(vi);
    if (err)
        goto free;

    vi->nbufs = virtqueue_get_vring_size(vi->vq);
    vi->bufs = kzalloc(sizeof(vi->bufs[0]) * vi->nbufs, GFP_KERNEL);
    if (!vi->bufs) {
        goto delvq;
    }

    virtio_device_ready(vdev);

    mutex_lock(&lock);
    list_add_tail(&vi->node, &virtpc_devs);
    mutex_unlock(&lock);

    printk("virtpc: added device %s\n", vi->name);

    return 0;
delvq:
    virtpc_del_vqs(vi);
free:
    kfree(vi);
free_misc:
    mutex_lock(&lock);
    -- virtpc_devcnt;
    mutex_unlock(&lock);
    if (--devcnt == 0) {
        misc_deregister(&virtpc_misc);
    }
    return err;
}

static void
virtpc_remove(struct virtio_device *vdev)
{
    struct virtpc_info *vi = vdev->priv;
    unsigned int devcnt;

    mutex_lock(&lock);
    printk("virtpc: removed device %s\n", vi->name);
    list_del(&vi->node);
    mutex_unlock(&lock);
    remove_vq_common(vi);
    kfree(vi->bufs);
    kfree(vi);

    mutex_lock(&lock);
    devcnt = -- virtpc_devcnt;
    mutex_unlock(&lock);
    if (devcnt <= 0) {
        misc_deregister(&virtpc_misc);
        printk("virtio-prodcons miscdevice deregistered\n");
    }
}

#ifdef CONFIG_PM_SLEEP
static int
virtpc_freeze(struct virtio_device *vdev)
{
    struct virtpc_info *vi = vdev->priv;

    remove_vq_common(vi);

    return 0;
}

static int
virtpc_restore(struct virtio_device *vdev)
{
    struct virtpc_info *vi = vdev->priv;
    int err;

    err = virtpc_find_vqs(vi);
    if (err)
        return err;

    virtio_device_ready(vdev);

    return 0;
}
#endif

/* ID must be consistent with include/standard-headers/linux/virtio_ids.h */
#define VIRTIO_ID_PRODCONS	20

static struct virtio_device_id id_table[] = {
    { VIRTIO_ID_PRODCONS, VIRTIO_DEV_ANY_ID },
    { 0 },
};

static unsigned int features[] = {
    VIRTIO_F_ANY_LAYOUT,
};

static struct virtio_driver virtio_pc_driver = {
    .feature_table		= features,
    .feature_table_size	= ARRAY_SIZE(features),
    .driver.name		= KBUILD_MODNAME,
    .driver.owner		= THIS_MODULE,
    .id_table		= id_table,
    .probe			= virtpc_probe,
    .remove			= virtpc_remove,
    .config_changed		= virtpc_config_changed,
#ifdef CONFIG_PM_SLEEP
    .freeze			= virtpc_freeze,
    .restore		= virtpc_restore,
#endif
};

module_virtio_driver(virtio_pc_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio prodcons driver");
MODULE_LICENSE("GPL");