/* team.h: the threads that run one function together: at most OF_MAX_THREADS, each pinned to a core of its own. Between the rounds of one frame (microseconds apart) a waiting thread spins; left alone longer it sleeps in
 * the kernel (a futex), so a team with no frame to work on costs the machine nothing: this runs on laptops, beside the program whose pictures it doubles. */
#ifndef OF_TEAM_H
#define OF_TEAM_H

struct team;

/* tid in 0 .. n-1. */
typedef void (*team_fn)(void *arg, int tid, int n);

/* n threads in all, 1 .. OF_MAX_THREADS, pinned to cpus[tid]. The caller is tid 0 and is pinned too: n - 1 threads are made. NULL on failure. */
struct team *team_create(const int *cpus, int n);
void team_destroy(struct team *t);
/* fn on every thread of the team, the caller's included; returns when all have returned. Called from the thread that made the team. */
void team_run(struct team *t, team_fn fn, void *arg);
int team_size(const struct team *t);
/* How long a waiting thread spins before it sleeps, microseconds (the default 50). */
void team_set_spin_us(struct team *t, unsigned us);

#endif
