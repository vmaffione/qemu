#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <sys/poll.h>
#include <stropts.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>


#define barrier() __sync_synchronize()
#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))
#define CACHELINE_ALIGN __attribute__((aligned(64)))

/******************************* TSC support ***************************/

/* initialize to avoid a division by 0 */
static double ticks_per_second = 1000000000.0; /* set by calibrate_tsc */

static inline uint64_t
rdtsc(void)
{
    uint32_t hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/*
 * do an idle loop to compute the clock speed. We expect
 * a constant TSC rate and locked on all CPUs.
 * Returns ticks per second
 */
static void
calibrate_tsc(void)
{
    struct timeval a, b;
    uint64_t ta_0, ta_1, tb_0, tb_1, dmax = ~0;
    uint64_t da, db, cy = 0;
    int i;
    for (i=0; i < 3; i++) {
	ta_0 = rdtsc();
	gettimeofday(&a, NULL);
	ta_1 = rdtsc();
	usleep(20000);
	tb_0 = rdtsc();
	gettimeofday(&b, NULL);
	tb_1 = rdtsc();
	da = ta_1 - ta_0;
	db = tb_1 - tb_0;
	if (da + db < dmax) {
	    cy = (b.tv_sec - a.tv_sec)*1000000 + b.tv_usec - a.tv_usec;
	    cy = (double)(tb_0 - ta_1)*1000000/(double)cy;
	    dmax = da + db;
	}
    }
    ticks_per_second = (double)cy;
}

#define NS2TSC(x) ((x)*ticks_per_second/1000000000.0)
#define TSC2NS(x) ((x)*1000000000.0/ticks_per_second)

static inline void
tsc_sleep_till(uint64_t when)
{
    while (rdtsc() < when)
        barrier();
}

static void
run_on_cpu(unsigned int cpuid)
{
    cpu_set_t cpuset;
    int ret;

    CPU_ZERO(&cpuset);
    CPU_SET(cpuid, &cpuset);

    ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (ret) {
        errno = ret;
        perror("pthread_setaffinity_np()");
    }
}
/*************************************************************************/

/* QLEN must be a power of two */
#define QLEN 512

#define USE_EVENTFDS

static struct global {
    /* Variables read by both P and C */
    CACHELINE_ALIGN
    unsigned int duration;
    unsigned int stop;
    unsigned int l;
    unsigned int wp;
    unsigned int wc;
    unsigned int yp;
    unsigned int yc;
    unsigned int psleep;
    unsigned int csleep;
#ifdef USE_EVENTFDS
    int pnotify;
    int cnotify;
    int pstop;
    int cstop;
#else
    CACHELINE_ALIGN
    pthread_mutex_t notfull_lock;
    pthread_cond_t notfull;
    unsigned int notfull_flag;

    CACHELINE_ALIGN
    pthread_mutex_t notempty_lock;
    pthread_cond_t notempty;
    unsigned int notempty_flag;
#endif

    /* Variables written by P */
    CACHELINE_ALIGN
    unsigned int p;
    unsigned int ce;
    uint64_t pnotifs;
    uint64_t pstarts;
    uint64_t psleeps;
    uint64_t q[QLEN];

    /* Variables written by C */
    CACHELINE_ALIGN
    unsigned int c;
    unsigned int pe;
    uint64_t items;
    uint64_t cnotifs;
    uint64_t cstarts;
    uint64_t csleeps;

    /* Miscellaneous, cache awareness not important. */
    CACHELINE_ALIGN
    uint64_t test_start;
    uint64_t test_end;
    unsigned int cpufirst;
    int quiet;
} _g;

static inline unsigned int
qidx(unsigned int idx)
{
    return idx & (QLEN - 1);
}

static inline int
queue_empty(struct global *g)
{
    return qidx(ACCESS_ONCE(g->p)) == qidx(g->c);
}

static inline int
queue_full(struct global *g)
{
    return qidx(g->p + 1) == qidx(ACCESS_ONCE(g->c));
}

static void *
producer(void *opaque)
{
    struct global *g = opaque;
    int need_notify;
    uint64_t next;
#ifdef USE_EVENTFDS
    struct pollfd fds[2];
    uint64_t x;
    int ret;

    fds[0].fd = g->cnotify;
    fds[1].fd = g->pstop;
#endif

    run_on_cpu(g->cpufirst);

    ACCESS_ONCE(g->ce) = ~0U; /* start with notification disabled */

    g->test_start = rdtsc();
    next = g->test_start + g->wp;

    while (!g->stop) {
        while (queue_full(g)) {
            if (g->psleep) {
                /* Sleeping strategy */
                g->psleeps ++;
                usleep(g->yp);
                if (g->stop) {
                    goto out;
                }

            } else {
                /* Notification strategy */
                ACCESS_ONCE(g->ce) = g->c + QLEN * 3 / 4;
                /* barrier and double-check */
                barrier();
                if (queue_full(g)) {
#ifdef USE_EVENTFDS
                    fds[0].events = POLLIN;
                    fds[1].events = POLLIN;
                    ret = poll(fds, 2, 1000);
                    if (ret <= 0 || fds[1].revents) {
                        if (ret < 0 || (fds[1].revents & ~POLLIN)) {
                            perror("poll()");
                        } else if (ret == 0) {
                            printf("Warning: timeout\n");
                        }
                        goto out;
                    }
                    ret = read(g->cnotify, &x, sizeof(x));
                    assert(ret == 8);
                    g->pstarts ++;
#else
                    pthread_mutex_lock(&g->notfull_lock);
                    if (!g->notfull_flag) {
                        pthread_cond_wait(&g->notfull, &g->notfull_lock);
                        g->pstarts ++;
                    }
                    g->notfull_flag = 0;
                    pthread_mutex_unlock(&g->notfull_lock);
#endif
                    next = rdtsc() + g->wp;
                }
            }
        }
        tsc_sleep_till(next);
        next += g->wp;
        ACCESS_ONCE(g->p) = g->p + 1;
        barrier();
        need_notify = (g->p - 1 == ACCESS_ONCE(g->pe));
        if (need_notify) {
#ifdef USE_EVENTFDS
            x = 1;
            ret = write(g->pnotify, &x, sizeof(x));
            assert(ret == 8);
#else
            pthread_mutex_lock(&g->notempty_lock);
            if (!g->notempty_flag) {
                g->notempty_flag = 1;
                pthread_cond_signal(&g->notempty);
            }
            pthread_mutex_unlock(&g->notempty_lock);
#endif
            g->pnotifs ++;
            next = rdtsc() + g->wp;
        }
        //printf("P %u ntfy %u\n", g->p, need_notify);
    }
out:
    return NULL;
}

static void *
consumer(void *opaque)
{
    struct global *g = opaque;
    int need_notify;
    uint64_t next;
#ifdef USE_EVENTFDS
    struct pollfd fds[2];
    uint64_t x;
    int ret;

    fds[0].fd = g->pnotify;
    fds[1].fd = g->cstop;
#endif

    run_on_cpu(g->cpufirst + 1);

    if (g->csleep) {
        ACCESS_ONCE(g->pe) = ~0U; /* notifications disabled */
    } else {
        ACCESS_ONCE(g->pe) = 0; /* wake me up on the first packet */
    }

    next = rdtsc() + g->wc; /* just in case */

    while (!g->stop) {
        while (queue_empty(g)) {
            if (g->csleep) {
                /* Sleeping strategy */
                g->csleeps ++;
                usleep(g->yc);
                if (g->stop) {
                    goto out;
                }

            } else {
                /* Notification strategy */
                ACCESS_ONCE(g->pe) = ACCESS_ONCE(g->p);
                /* barrier and double-check */
                barrier();
                if (queue_empty(g)) {
#ifdef USE_EVENTFDS
                    fds[0].events = POLLIN;
                    fds[1].events = POLLIN;
                    ret = poll(fds, 2, 1000);
                    if (ret <= 0 || fds[1].revents) {
                        if (ret < 0 || (fds[1].revents & ~POLLIN)) {
                            perror("poll()");
                        } else if (ret == 0) {
                            printf("Warning: timeout\n");
                        }
                        goto out;
                    }
                    ret = read(g->pnotify, &x, sizeof(x));
                    assert(ret == 8);
                    g->cstarts ++;
#else
                    pthread_mutex_lock(&g->notempty_lock);
                    if (!g->notempty_flag) {
                        pthread_cond_wait(&g->notempty, &g->notempty_lock);
                        g->cstarts ++;
                    }
                    g->notempty_flag = 0;
                    pthread_mutex_unlock(&g->notempty_lock);
#endif
                    next = rdtsc() + g->wc;
                }
            }
        }
        tsc_sleep_till(next);
        next += g->wc;
        ACCESS_ONCE(g->c) = g->c + 1;
        barrier();
        need_notify = (g->c - 1 == ACCESS_ONCE(g->ce));
        if (need_notify) {
#ifdef USE_EVENTFDS
            x = 1;
            ret = write(g->cnotify, &x, sizeof(x));
            assert(ret == 8);
#else
            pthread_mutex_lock(&g->notfull_lock);
            if (!g->notfull_flag) {
                g->notfull_flag = 1;
                pthread_cond_signal(&g->notfull);
            }
            pthread_mutex_unlock(&g->notfull_lock);
#endif
            g->cnotifs ++;
            next = rdtsc() + g->wc;
        }
        //printf("C %u ntfy %u\n", g->c, need_notify);
        g->items ++;
    }
out:
    g->test_end = rdtsc();

    return NULL;
}

static void
pc_stop(struct global *g)
{
#ifdef USE_EVENTFDS
    uint64_t x = 1;

#endif
    g->stop = 1;
#ifdef USE_EVENTFDS
    write(g->pstop, &x, sizeof(x));
    write(g->cstop, &x, sizeof(x));
#else
    pthread_mutex_lock(&g->notfull_lock);
    pthread_cond_signal(&g->notfull);
    pthread_mutex_unlock(&g->notfull_lock);
    pthread_mutex_lock(&g->notempty_lock);
    pthread_cond_signal(&g->notempty);
    pthread_mutex_unlock(&g->notempty_lock);
#endif
}

static void
csb_dump(struct global *g)
{
    /* CSB dump */
    printf("p=%u pe=%u c=%u ce=%u\n",
            g->p, g->pe, g->c, g->ce);
}

static int sigint_cnt = 0;

static void
sigint_handler(int sig)
{
    struct global *g = &_g;

    csb_dump(g);
    /* Stop. */
    pc_stop(g);
    if (++ sigint_cnt > 2) {
        printf("aborting...\n");
        exit(EXIT_FAILURE);
    }
}

static void
print_header(void)
{
    printf("%7s %7s %7s %7s %7s %10s %9s %9s %9s %9s %9s %9s\n",
            "Wp", "Wc", "Yp", "Yc", "L", "items/s", "pnotifs/s",
            "cnotifs/s", "pstarts/s", "cstarts/s", "psleeps/s",
            "csleeps/s");
}

static void
usage(void)
{
    printf("test [-p WP_NANOSEC] [-c WC_NANOSEC]\n"
            "[-y YP_NANOSEC] [-Y YC_NANOSEC]\n"
            "[-s <producer sleeps>] [-S <consumer sleeps>]\n"
            "[-q <be quiet>]\n"
            "[-d DURATION_SEC]\n"
            "[-l QUEUE_LEN]\n"
            "[-H <print header only>]");
}

static unsigned int
parseuint(const char *s)
{
    int x;

    x = atoi(optarg);
    if (x < 1) {
        printf("Invalid -p option argument\n");
        usage();
        exit(EXIT_FAILURE);
    }

    return (unsigned int)x;
}

int main(int argc, char **argv)
{
    struct global *g = &_g;
    pthread_t thp, thc;
    int ret;
    int ch;

    memset(g, 0, sizeof(*g));

#ifdef USE_EVENTFDS
    g->pnotify = eventfd(0, EFD_NONBLOCK);
    g->cnotify = eventfd(0, EFD_NONBLOCK);
    g->pstop = eventfd(0, EFD_NONBLOCK);
    g->cstop = eventfd(0, EFD_NONBLOCK);
    if (g->pnotify < 0 || g->cnotify < 0 || g->pstop < 0 || g->cstop < 0) {
        perror("eventfd()");
        return -1;
    }
#else
    pthread_mutex_init(&g->notfull_lock, NULL);
    pthread_mutex_init(&g->notempty_lock, NULL);
    pthread_cond_init(&g->notfull, NULL);
    pthread_cond_init(&g->notempty, NULL);
#endif

    g->wp = 2100;
    g->wc = 2000;
    g->yp = 5000;
    g->yc = 5000;
    g->psleep = 0;
    g->csleep = 0;
    g->l = QLEN;
    g->duration = 5;

    while ((ch = getopt(argc, argv, "Hhc:p:sSy:Y:d:ql:")) != -1) {
        switch (ch) {
            default:
            case 'h':
                usage();
                return 0;

            case 'H':
                print_header();
                return 0;

            case 's':
                g->psleep = 1;
                break;

            case 'S':
                g->csleep = 1;
                break;

            case 'p':
                g->wp = parseuint(optarg);
                break;

            case 'c':
                g->wc = parseuint(optarg);
                break;

            case 'y':
                g->yp = parseuint(optarg);
                break;

            case 'Y':
                g->yc = parseuint(optarg);
                break;

            case 'd':
                g->duration = parseuint(optarg);
                break;

            case 'l':
                /* Ignored for now */
                g->l = QLEN;
                break;

            case 'q':
                g->quiet = 1;
                break;
        }
    }

    if (signal(SIGINT, sigint_handler)) {
        perror("signal()");
        exit(EXIT_FAILURE);
    }

    if (signal(SIGTERM, sigint_handler)) {
        perror("signal()");
        exit(EXIT_FAILURE);
    }

    calibrate_tsc();
    g->wp = NS2TSC(g->wp);
    g->wc = NS2TSC(g->wc);
    g->yp = g->yp/1000;
    g->yc = g->yc/1000;

    {
        int pfd = open("/proc/self/timerslack_ns", O_RDWR);
        if (pfd < 0) {
            perror("open(timerslack)");
        } else {
            const char buf[] = {'1', '\0'};
            char buf2[20];

            if (write(pfd, buf, sizeof(buf)) != sizeof(buf)) {
                perror("write(0)");
            }

            if (read(pfd, buf2, sizeof(buf2)) < 0) {
                perror("read(timerslack_ns)");
            } else if (!g->quiet) {
                printf("timerslack: %s\n", buf);
            }
        }
        close(pfd);
    }

    ret = pthread_create(&thp, NULL, producer, g);
    if (ret) {
        perror("pthread_create(P)");
        exit(EXIT_FAILURE);
    }

    ret = pthread_create(&thc, NULL, consumer, g);
    if (ret) {
        perror("pthread_create(C)");
        exit(EXIT_FAILURE);
    }

    sleep(g->duration);
    pc_stop(g);

    ret = pthread_join(thp, NULL);
    if (ret) {
        perror("pthread_join(P)");
        exit(EXIT_FAILURE);
    }

    ret = pthread_join(thc, NULL);
    if (ret) {
        perror("pthread_join(C)");
        exit(EXIT_FAILURE);
    }

    /* statistics */
    {
        double test_len = TSC2NS(g->test_end - g->test_start) / 1000000000.0;

        if (!g->quiet) {
            printf("#items: %lu, testlen: %3.4f\n", g->items, test_len);
            print_header();
        }
        printf("%7.1f %7.1f %7u %7u %7u %10.0f %9.0f %9.0f %9.0f %9.0f "
                "%9.0f %9.0f\n",
                TSC2NS(g->wp), TSC2NS(g->wc), g->yp * 1000, g->yc * 1000, g->l,
                g->items / test_len,
                g->pnotifs / test_len,
                g->cnotifs / test_len,
                g->pstarts / test_len,
                g->cstarts / test_len,
                g->psleeps / test_len,
                g->csleeps / test_len);
    }

    return 0;
}