/* engine.c: see engine.h. One thread waits for frames; it is the team's first, so the flow's threads are exactly the number asked for. */
#define _GNU_SOURCE
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include "team.h"
#include "vt_util.h"

static void team_runner(void *ctx, flow_fn fn, void *arg) { team_run(ctx, fn, arg); }

static void *engine_main(void *p)
{
    struct of_engine *e = p;
    static struct flow f __attribute__((aligned(64)));
    struct team *team = team_create(e->cpus, e->threads);
    const int bad = !team || flow_init(&f, e->mem, e->w, e->h, FLOW_VEC, team_runner, team) || f.fg.mid.w != e->bw || f.fg.mid.h != e->bh;
    if (!bad && e->enough >= 0)
        f.fg.enough = (float)e->enough;
    pthread_mutex_lock(&e->lock);
    e->failed = bad;
    e->levels = f.levels;
    e->ready = 1;
    pthread_cond_broadcast(&e->wake);
    pthread_mutex_unlock(&e->lock);
    if (bad)
        return NULL;
    const uint64_t khz = of_tsc_khz();
    const size_t vec_bytes = (size_t)e->bw * (size_t)e->bh * sizeof(struct fg_vec);
    for (uint64_t seen = 0;;) {
        pthread_mutex_lock(&e->lock);
        while (e->frames_pub == seen && !e->quit)
            pthread_cond_wait(&e->wake, &e->lock);
        const int quit = e->quit;
        pthread_mutex_unlock(&e->lock);
        if (quit)
            break;
        const uint64_t seq = ++seen;
        const struct of_msg m = e->msg[seq % OF_RING];
        const uint64_t t0 = vt_tsc(), waited = of_now_ns() - e->t_submit[seq % OF_RING];
        f.u8 = e->luma[m.in_slot % OF_SLOTS];
        f.u8_stride = e->stride;
        flow_land(&f);
        const int made = m.flags & OF_NO_MID ? -1 : flow_make(&f);
        if (made == 1)
            memcpy(e->vec[m.out_slot % OF_SLOTS], f.fg.mid.v, vec_bytes);
        const uint64_t t1 = vt_tsc();
#define US(c) ((uint32_t)((c) * 1000ull / khz))
        struct of_gen *g = &e->gen[seq % OF_RING];
        *g = (struct of_gen){.seq = seq, .out_slot = m.out_slot, .made = made, .moved = made == 1 ? f.moved : 0};
        g->us[OF_T_WAIT] = (uint32_t)(waited / 1000u);
        g->us[OF_T_LAND] = US(f.cycles[FLOW_LAND]);
        g->us[OF_T_PYR] = US(f.cycles[FLOW_PYR]);
        g->us[OF_T_HIST] = US(f.cycles[FLOW_HIST]);
        g->us[OF_T_SEARCH] = US(f.cycles[FLOW_SEARCH]);
        g->us[OF_T_FILTER] = US(f.cycles[FLOW_FILTER]);
        g->us[OF_T_UP] = US(f.cycles[FLOW_UP]);
        g->us[OF_T_MID] = US(f.cycles[FLOW_MID]);
        g->us[OF_T_TOTAL] = US(t1 - t0);
#undef US
        __atomic_store_n(&e->gen_pub, seq, __ATOMIC_SEQ_CST);
        const uint64_t one = 1;
        if (write(e->event_fd, &one, sizeof one) != (ssize_t)sizeof one)
            perror("engine: eventfd");
    }
    team_destroy(team);
    return NULL;
}

int of_engine_open(struct of_engine *e, int w, int h, const int *cpus, int threads, int enough)
{
    memset(e, 0, sizeof *e);
    if (w < 64 || h < 64 || w > 8192 || h > 8192 || (w & 15) || threads < 1 || threads > OF_MAX_THREADS) {
        fprintf(stderr, "engine: a luma of %d x %d (the width a multiple of 16, 64 to 8192 each way) on %d threads (1 to %d)\n", w, h, threads, OF_MAX_THREADS);
        return -1;
    }
    e->w = w;
    e->h = h;
    e->stride = of_luma_stride(w);
    e->bw = of_field_w(w);
    e->bh = of_field_w(h);
    e->threads = threads;
    e->enough = enough;
    memcpy(e->cpus, cpus, (size_t)threads * sizeof(int));
    const size_t luma = ((size_t)e->stride * (size_t)h + 4095u) & ~(size_t)4095u, vec = ((size_t)e->bw * (size_t)e->bh * sizeof(struct fg_vec) + 4095u) & ~(size_t)4095u;
    const size_t flow = (flow_bytes(w, h, FLOW_VEC) + 4095u) & ~(size_t)4095u, all = flow + OF_SLOTS * (luma + vec);
    char *m = mmap(NULL, all, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (m == MAP_FAILED) {
        perror("engine: memory");
        return -1;
    }
    madvise(m, all, MADV_HUGEPAGE);
    e->mem = m;
    m += flow;
    for (int i = 0; i < OF_SLOTS; i++, m += luma)
        e->luma[i] = (uint8_t *)m;
    for (int i = 0; i < OF_SLOTS; i++, m += vec)
        e->vec[i] = (struct fg_vec *)m;
    e->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->event_fd < 0 || pthread_mutex_init(&e->lock, NULL) || pthread_cond_init(&e->wake, NULL) || pthread_create(&e->thread, NULL, engine_main, e)) {
        perror("engine: thread");
        return -1;
    }
    pthread_mutex_lock(&e->lock);
    while (!e->ready)
        pthread_cond_wait(&e->wake, &e->lock);
    pthread_mutex_unlock(&e->lock);
    if (e->failed) {
        fprintf(stderr, "engine: no team of %d threads, or no flow for %d x %d\n", threads, w, h);
        pthread_join(e->thread, NULL);
        return -1;
    }
    return 0;
}

void of_engine_submit(struct of_engine *e, uint64_t seq)
{
    e->t_submit[seq % OF_RING] = of_now_ns();
    pthread_mutex_lock(&e->lock);
    e->frames_pub = seq;
    pthread_cond_signal(&e->wake);
    pthread_mutex_unlock(&e->lock);
}

void of_engine_close(struct of_engine *e)
{
    if (!e->ready || e->failed)
        return;
    pthread_mutex_lock(&e->lock);
    e->quit = 1;
    pthread_cond_signal(&e->wake);
    pthread_mutex_unlock(&e->lock);
    pthread_join(e->thread, NULL);
    close(e->event_fd);
}
