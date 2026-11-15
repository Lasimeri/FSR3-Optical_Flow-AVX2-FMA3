/* team.c: see team.h. Two futex words: go (the round's number, on which the workers wait) and left (the workers still in the round, on which the caller waits). Each side announces that it will sleep before it looks at
 * the word one last time, and the other side changes the word before it looks for sleepers: one of the two always sees the other (sequentially consistent stores and loads), so no wake is lost and none is paid for nothing. */
#define _GNU_SOURCE
#include "team.h"

#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cpus.h"
#include "vt_util.h"

#define LINE __attribute__((aligned(64)))

struct tthread {
    struct team *t;
    int tid, cpu;
    pthread_t thread;
};

struct team {
    int n;
    uint64_t spin; /* cycles */
    team_fn fn;
    void *arg;
    uint32_t go LINE;
    uint32_t left LINE;
    uint32_t sleepers LINE;
    uint32_t caller_sleeps;
    struct tthread thr[OF_MAX_THREADS] LINE;
};

static void futex_wait(uint32_t *word, uint32_t seen) { syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, seen, NULL, NULL, 0); }
static void futex_wake(uint32_t *word, int many) { syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, many, NULL, NULL, 0); }

static void *worker_main(void *p)
{
    struct tthread *me = p;
    struct team *t = me->t;
    of_cpus_pin(me->cpu);
    uint32_t seen = 0;
    for (;;) {
        uint32_t g;
        const uint64_t t0 = vt_tsc();
        while ((g = __atomic_load_n(&t->go, __ATOMIC_SEQ_CST)) == seen) {
            if (vt_tsc() - t0 < t->spin) {
                __builtin_ia32_pause();
                continue;
            }
            __atomic_add_fetch(&t->sleepers, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&t->go, __ATOMIC_SEQ_CST) == seen)
                futex_wait(&t->go, seen);
            __atomic_sub_fetch(&t->sleepers, 1, __ATOMIC_SEQ_CST);
        }
        seen = g;
        const team_fn fn = t->fn;
        if (!fn)
            return NULL;
        fn(t->arg, me->tid, t->n);
        if (__atomic_sub_fetch(&t->left, 1, __ATOMIC_SEQ_CST) == 0 && __atomic_load_n(&t->caller_sleeps, __ATOMIC_SEQ_CST))
            futex_wake(&t->left, 1);
    }
}

struct team *team_create(const int *cpus, int n)
{
    struct team *t;
    if (n < 1 || n > OF_MAX_THREADS || posix_memalign((void **)&t, 64, sizeof *t))
        return NULL;
    *t = (struct team){.n = n, .spin = of_tsc_khz() * 50u / 1000u};
    of_cpus_pin(cpus[0]);
    t->thr[0] = (struct tthread){.t = t, .tid = 0, .cpu = cpus[0]};
    for (int i = 1; i < n; i++) {
        t->thr[i] = (struct tthread){.t = t, .tid = i, .cpu = cpus[i]};
        if (pthread_create(&t->thr[i].thread, NULL, worker_main, &t->thr[i])) {
            t->n = i;
            team_destroy(t);
            return NULL;
        }
    }
    return t;
}

int team_size(const struct team *t) { return t->n; }

void team_set_spin_us(struct team *t, unsigned us) { t->spin = of_tsc_khz() * us / 1000u; }

static void start_round(struct team *t, team_fn fn, void *arg)
{
    t->fn = fn;
    t->arg = arg;
    __atomic_store_n(&t->left, (uint32_t)(t->n - 1), __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&t->go, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t->sleepers, __ATOMIC_SEQ_CST))
        futex_wake(&t->go, INT_MAX);
}

void team_run(struct team *t, team_fn fn, void *arg)
{
    if (t->n > 1)
        start_round(t, fn, arg);
    fn(arg, 0, t->n);
    if (t->n == 1)
        return;
    const uint64_t t0 = vt_tsc();
    uint32_t v;
    while ((v = __atomic_load_n(&t->left, __ATOMIC_SEQ_CST)) != 0) {
        if (vt_tsc() - t0 < t->spin) {
            __builtin_ia32_pause();
            continue;
        }
        __atomic_store_n(&t->caller_sleeps, 1, __ATOMIC_SEQ_CST);
        if ((v = __atomic_load_n(&t->left, __ATOMIC_SEQ_CST)) != 0)
            futex_wait(&t->left, v);
        __atomic_store_n(&t->caller_sleeps, 0, __ATOMIC_SEQ_CST);
    }
}

void team_destroy(struct team *t)
{
    if (t->n > 1) {
        start_round(t, NULL, NULL);
        for (int i = 1; i < t->n; i++)
            pthread_join(t->thr[i].thread, NULL);
    }
    free(t);
}
