/* engine.h: the optical flow as a service inside the program: where MVEX-OpticalFLow sent a luma over the card's link and read the vectors back from it, here a thread of this process (the team's first) takes the luma from
 * memory and leaves the vectors in memory. The words are the card protocol's, so the program that shows the pictures is the same program: a ring of frames given (msg, frames_pub), a ring of answers (gen, gen_pub),
 * six luma slots and six vector slots. What the link added is gone: no DMA, no staging copy, no shared region, no huge pages.
 *
 * The form is the vectors-only one (flow_form FLOW_VEC): w x h is the luma at the field's own size, 8 bits a sample, rows of of_luma_stride(w) bytes; an answer is the field at the middle instant between this frame and the
 * one before, of_field_w(w) x of_field_w(h) vectors, row by row. */
#ifndef OF_ENGINE_H
#define OF_ENGINE_H

#include <pthread.h>
#include <stdint.h>

#include "cpus.h"
#include "flow.h"

#define OF_SLOTS 6
#define OF_RING 16

enum of_flags { OF_NO_MID = 1 };
enum of_times { OF_T_WAIT, OF_T_LAND, OF_T_PYR, OF_T_HIST, OF_T_SEARCH, OF_T_FILTER, OF_T_UP, OF_T_MID, OF_T_TOTAL, OF_T_COUNT };

static inline int of_luma_stride(int w) { return (w + 63) & ~63; }
static inline int of_field_w(int w) { return (w + 7) / 8; }

/* A frame is ready: its luma is in luma[in_slot]; the vectors, if a mid is wanted, go to vec[out_slot]. */
struct of_msg {
    uint64_t seq;
    uint32_t in_slot, out_slot, flags;
};

/* Done with that frame. made 1: the field between the frame before and this one is in vec[out_slot]; 0: a scene change, no field; -1: none asked. us: microseconds, OF_T_WAIT from the submit to the engine's start. */
struct of_gen {
    uint64_t seq;
    uint32_t out_slot;
    int32_t made;
    uint32_t moved;
    uint32_t us[OF_T_COUNT];
};

struct of_engine {
    int w, h, stride, bw, bh, threads, levels;
    uint8_t *luma[OF_SLOTS];
    struct fg_vec *vec[OF_SLOTS];
    /* The caller writes msg[seq % OF_RING], then of_engine_submit(seq). Sequence numbers start at 1 and rise by 1. */
    struct of_msg msg[OF_RING];
    uint64_t t_submit[OF_RING];
    /* The engine writes gen[seq % OF_RING], then gen_pub = seq, then makes event_fd readable. */
    struct of_gen gen[OF_RING];
    uint64_t gen_pub __attribute__((aligned(64)));
    int event_fd;
    /* The engine's own. */
    uint64_t frames_pub;
    int quit, failed, ready;
    int cpus[OF_MAX_THREADS];
    int enough;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    void *mem;
};

/* threads: 1 .. OF_MAX_THREADS flow threads on cpus[0 .. threads-1], all made here (the caller is none of them). enough: Q6 counts over a block under which the finest level keeps its estimate, -1 the default. 0, or -1 with a message. */
int of_engine_open(struct of_engine *e, int w, int h, const int *cpus, int threads, int enough);
void of_engine_submit(struct of_engine *e, uint64_t seq);
void of_engine_close(struct of_engine *e);

#endif
