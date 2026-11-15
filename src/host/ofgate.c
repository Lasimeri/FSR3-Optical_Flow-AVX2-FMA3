/* ofgate: the gate of the port. A luma with known motion goes through the kernels twice on the same team of threads: once as built for this CPU (AVX2 and FMA3, src/common/vx.h's native body) and once as the lane model of the
 * same source (linked beside it under the prefix model_). Every vector of every field must be the same, a control proves the comparison can fail, the field is held against the motion the scene was made with, and each stage's
 * time is printed for both, taken in turn frame by frame, so the port's speed is measured against the plain form on the same threads, the same frames, the same minute.
 *
 *   ofgate --size 1920x1080 --count 24               8 threads, one per core
 *   ofgate --size 1920x1080 --threads 7              what the live program gives the flow (its own thread is the eighth)
 *   ofgate --size 1280x720 --no-model                the native body alone: its times */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpus.h"
#include "engine.h"
#include "flow.h"
#include "team.h"
#include "vt_util.h"

size_t model_flow_bytes(int w, int h, int form);
int model_flow_init(struct flow *f, void *mem, int w, int h, int form, flow_run_fn run, void *ctx);
void model_flow_land(struct flow *f);
int model_flow_make(struct flow *f);

#define TEX 1024
static uint8_t tex[2][TEX * TEX];

static inline uint32_t hash3(uint32_t x, uint32_t y, uint32_t s)
{
    uint32_t h = x * 0x9e3779b1u ^ y * 0x85ebca6bu ^ s * 0xc2b2ae35u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    return h ^ h >> 15;
}

/* Value noise that wraps at TEX, one octave: cells of `cell` pixels, bilinear between the lattice's bytes. */
static uint32_t octave(int x, int y, int cell, uint32_t seed)
{
    const uint32_t per = (uint32_t)(TEX / cell), gx = (uint32_t)(x / cell), gy = (uint32_t)(y / cell), fx = (uint32_t)(x % cell), fy = (uint32_t)(y % cell), c = (uint32_t)cell;
    const uint32_t c00 = hash3(gx % per, gy % per, seed) & 255u, c10 = hash3((gx + 1) % per, gy % per, seed) & 255u, c01 = hash3(gx % per, (gy + 1) % per, seed) & 255u, c11 = hash3((gx + 1) % per, (gy + 1) % per, seed) & 255u;
    return ((c00 * (c - fx) + c10 * fx) * (c - fy) + (c01 * (c - fx) + c11 * fx) * fy) / (c * c);
}

static void tex_make(void)
{
    for (int t = 0; t < 2; t++)
        for (int y = 0; y < TEX; y++)
            for (int x = 0; x < TEX; x++)
                tex[t][y * TEX + x] = (uint8_t)((octave(x, y, 32, 11u + (uint32_t)t) * 5u + octave(x, y, 8, 23u + (uint32_t)t) * 3u) / 8u);
}

static int pan_x = 6, pan_y = 2, box_x = -9, box_y = 4;

static inline int box_inside(int x, int y, int k, int w, int h)
{
    const int x0 = w / 3 + k * box_x, y0 = h / 3 + k * box_y;
    return x >= x0 && x < x0 + w / 6 && y >= y0 && y < y0 + h / 5;
}

/* Frame k: a ground that moves by the pan each frame, and on it a box that moves its own way. */
static void scene(int k, uint8_t *dst, int stride, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[(size_t)y * (size_t)stride + (size_t)x] = box_inside(x, y, k, w, h) ? tex[1][((y - k * box_y) & (TEX - 1)) * TEX + ((x - k * box_x) & (TEX - 1))] : tex[0][((y - k * pan_y) & (TEX - 1)) * TEX + ((x - k * pan_x) & (TEX - 1))];
}

static void team_runner(void *ctx, flow_fn fn, void *arg) { team_run(ctx, fn, arg); }

enum { T_LAND, T_PYR, T_HIST, T_SEARCH, T_FILTER, T_UP, T_MID, T_TOTAL, T_COUNT };
static const char *const t_names[T_COUNT] = {"land", "pyr", "hist", "search", "filter", "up", "mid", "total"};
static const int t_stage[T_TOTAL] = {FLOW_LAND, FLOW_PYR, FLOW_HIST, FLOW_SEARCH, FLOW_FILTER, FLOW_UP, FLOW_MID};

struct side {
    const char *name;
    struct flow *f;
    void (*land)(struct flow *);
    int (*make)(struct flow *);
    uint64_t *cycles[T_COUNT];
    int made;
};

static void run_side(struct side *s, const uint8_t *in, int stride, int i, int field)
{
    s->f->u8 = in;
    s->f->u8_stride = stride;
    const uint64_t t0 = vt_tsc();
    s->land(s->f);
    s->made = i ? s->make(s->f) : -1;
    const uint64_t t1 = vt_tsc();
    if (s->made != 1)
        return;
    for (int t = 0; t < T_TOTAL; t++)
        s->cycles[t][field] = s->f->cycles[t_stage[t]];
    s->cycles[T_TOTAL][field] = t1 - t0;
}

static void print_side(const struct side *s, int fields, uint64_t khz, uint64_t *median_total)
{
    printf("%-6s us, median:", s->name);
    for (int t = 0; t < T_COUNT; t++) {
        uint64_t mx = 0;
        for (int n = 0; n < fields; n++)
            mx = s->cycles[t][n] > mx ? s->cycles[t][n] : mx;
        const uint64_t med = vt_median_u64(s->cycles[t], fields);
        printf(" %s %llu", t_names[t], (unsigned long long)(med * 1000u / khz));
        if (t == T_TOTAL) {
            printf(" (least %llu, max %llu)", (unsigned long long)(s->cycles[t][0] * 1000u / khz), (unsigned long long)(mx * 1000u / khz));
            *median_total = med;
        }
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    int w = 1920, h = 1080, count = 24, threads = OF_MAX_THREADS, enough = -1, no_model = 0, spin = -1, search_all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &w, &h) == 2)
            i++;
        else if (!strcmp(argv[i], "--pan") && i + 1 < argc && sscanf(argv[i + 1], "%d,%d", &pan_x, &pan_y) == 2)
            i++;
        else if (!strcmp(argv[i], "--count") && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--enough") && i + 1 < argc)
            enough = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spin") && i + 1 < argc)
            spin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--search-all"))
            search_all = 1;
        else if (!strcmp(argv[i], "--no-model"))
            no_model = 1;
        else {
            fprintf(stderr, "usage: ofgate [--size WxH: the luma, W a multiple of 16] [--count N: frames] [--pan X,Y] [--threads T: 1 to %d] [--enough Q6] [--search-all: every block of the finest level searches] [--spin US] [--no-model: the native body alone]\n", OF_MAX_THREADS);
            return 2;
        }
    }
    if (w < 64 || h < 64 || (w & 15) || count < 2 || count > 4096 || threads < 1 || threads > OF_MAX_THREADS) {
        fprintf(stderr, "ofgate: a luma of at least 64 x 64 with a width that is a multiple of 16, 2 to 4096 frames, 1 to %d threads\n", OF_MAX_THREADS);
        return 2;
    }
    if (!of_cpu_ok()) {
        fprintf(stderr, "ofgate: this CPU has no AVX2 or no FMA3: the native body cannot run here\n");
        return 1;
    }
    int cpus[OF_MAX_THREADS];
    const int cores = of_cpus_pick(cpus, threads);
    if (cores < threads) {
        fprintf(stderr, "ofgate: %d threads asked, %d cores to give them one each: taking %d\n", threads, cores, cores);
        threads = cores;
    }
    if (of_cpus_confine(cpus, threads))
        perror("ofgate: affinity");
    struct team *team = team_create(cpus, threads);
    if (!team) {
        fprintf(stderr, "ofgate: no team\n");
        return 1;
    }
    if (spin >= 0)
        team_set_spin_us(team, (unsigned)spin);
    const int stride = of_luma_stride(w), bw = of_field_w(w), bh = of_field_w(h), fields = count - 1;
    const size_t vec_count = (size_t)bw * (size_t)bh;
    static struct flow fn_, fm_;
    struct side side[2] = {{.name = "native", .f = &fn_, .land = flow_land, .make = flow_make}, {.name = "model", .f = &fm_, .land = model_flow_land, .make = model_flow_make}};
    void *mem[2], *in_mem;
    const size_t bytes[2] = {flow_bytes(w, h, FLOW_VEC), model_flow_bytes(w, h, FLOW_VEC)};
    if (posix_memalign(&in_mem, 4096, (size_t)stride * (size_t)h) || posix_memalign(&mem[0], 4096, bytes[0]) || posix_memalign(&mem[1], 4096, bytes[1]))
        return 1;
    memset(mem[0], 0, bytes[0]);
    memset(mem[1], 0, bytes[1]);
    memset(in_mem, 0, (size_t)stride * (size_t)h);
    if (flow_init(&fn_, mem[0], w, h, FLOW_VEC, team_runner, team) || model_flow_init(&fm_, mem[1], w, h, FLOW_VEC, team_runner, team))
        return 1;
    /* --search-all: no block of the finest level is let off by its estimate, so every one of them searches its 256 places: the most a frame can cost. */
    if (search_all)
        fn_.fg.enough = fm_.fg.enough = -1.0f;
    else if (enough >= 0)
        fn_.fg.enough = fm_.fg.enough = (float)enough;
    for (int s = 0; s < 2; s++)
        for (int t = 0; t < T_COUNT; t++)
            if (!(side[s].cycles[t] = calloc((size_t)fields, sizeof(uint64_t))))
                return 1;
    const uint64_t khz = of_tsc_khz();
    const int in_process = of_threads_now();
    printf("ofgate: luma %d x %d, field %d x %d, %d levels; %d threads, one a core, on cpus", w, h, bw, bh, fn_.levels, threads);
    for (int i = 0; i < threads; i++)
        printf(" %d", cpus[i]);
    printf("; threads in the process %d; tsc %llu MHz\n", in_process, (unsigned long long)(khz / 1000u));
    tex_make();
    uint8_t *in = in_mem;
    uint64_t differ = 0, compared = 0, control = 0, moved_bad = 0, moved_sum = 0, made_bad = 0;
    uint64_t ground = 0, ground_ok = 0, box = 0, box_ok = 0;
    int made_n = 0;
    for (int i = 0; i < count; i++) {
        scene(i, in, stride, w, h);
        /* In turn: the one that goes second finds the luma in the cache, so neither is always second. */
        const int first = no_model ? 0 : i & 1;
        run_side(&side[first], in, stride, i, made_n);
        if (!no_model)
            run_side(&side[!first], in, stride, i, made_n);
        if (!no_model && side[0].made != side[1].made) {
            printf("frame %d: native made %d, model %d\n", i, side[0].made, side[1].made);
            made_bad++;
        }
        if (side[0].made != 1)
            continue;
        const struct fg_vec *field = fn_.fg.mid.v;
        if (!no_model && side[1].made == 1) {
            for (size_t q = 0; q < vec_count; q++)
                differ += field[q].x != fm_.fg.mid.v[q].x || field[q].y != fm_.fg.mid.v[q].y;
            compared += vec_count;
            moved_bad += fn_.moved != fm_.moved;
            /* The control: the same comparison with one vector of the model's changed must see it. */
            struct fg_vec *const probe = &fm_.fg.mid.v[vec_count / 2];
            probe->x ^= 1;
            control += field[vec_count / 2].x != probe->x;
            probe->x ^= 1;
        }
        made_n++;
        moved_sum += fn_.moved;
        /* Against the truth: a thing at x in the newer frame was at x - step in the older, so its vector is minus the step. Blocks wholly on the ground or wholly in the box, in both frames; the rim has no older place to come from. */
        for (int by = 4; by < bh - 4; by++)
            for (int bx = 4; bx < bw - 4; bx++) {
                int in_box = 0, on_ground = 0;
                for (int k = i - 1; k <= i; k++)
                    for (int cy = 0; cy < 2; cy++)
                        for (int cx = 0; cx < 2; cx++) {
                            const int inside = box_inside(bx * 8 + cx * 7, by * 8 + cy * 7, k, w, h);
                            in_box += inside;
                            on_ground += !inside;
                        }
                const struct fg_vec v = field[(size_t)by * (size_t)bw + (size_t)bx];
                if (on_ground == 8) {
                    ground++;
                    ground_ok += v.x == -pan_x && v.y == -pan_y;
                } else if (in_box == 8) {
                    box++;
                    box_ok += v.x == -box_x && v.y == -box_y;
                }
            }
    }
    printf("ground %d,%d and box %d,%d a frame: %d frames, %d fields, %.1f %% of blocks moved\n", pan_x, pan_y, box_x, box_y, count, made_n, made_n ? 100.0 * (double)moved_sum / (double)made_n / (double)vec_count : 0.0);
    printf("truth: ground %llu of %llu blocks exact (%.2f %%), box %llu of %llu (%.2f %%)\n", (unsigned long long)ground_ok, (unsigned long long)ground, ground ? 100.0 * (double)ground_ok / (double)ground : 0.0, (unsigned long long)box_ok, (unsigned long long)box, box ? 100.0 * (double)box_ok / (double)box : 0.0);
    int rc = made_n != fields || (ground && ground_ok * 10 < ground * 9) || in_process > OF_MAX_THREADS;
    uint64_t med[2] = {0, 0};
    if (made_n) {
        print_side(&side[0], made_n, khz, &med[0]);
        if (!no_model)
            print_side(&side[1], made_n, khz, &med[1]);
    }
    if (!no_model) {
        printf("native = lane model: %llu of %llu vectors differ, moved mismatches %llu, made mismatches %llu; control caught %llu of %d\n", (unsigned long long)differ, (unsigned long long)compared, (unsigned long long)moved_bad, (unsigned long long)made_bad, (unsigned long long)control, made_n);
        if (med[0])
            printf("the native body against the plain form of the same kernels on the same %d threads: %.2f times as fast (median total %llu us against %llu)\n", threads, (double)med[1] / (double)med[0], (unsigned long long)(med[0] * 1000u / khz), (unsigned long long)(med[1] * 1000u / khz));
        rc |= differ || moved_bad || made_bad || (int)control != made_n || med[0] > med[1];
    }
    team_destroy(team);
    printf("ofgate: %s\n", rc ? "FAIL" : "PASS");
    return rc;
}
