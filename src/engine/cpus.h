/* cpus.h: which CPUs the flow may use, and the law they are held to: at most OF_MAX_THREADS threads, each on a physical core of its own, never two on the hardware threads of one core. */
#ifndef OF_CPUS_H
#define OF_CPUS_H

#include <stdint.h>

#define OF_MAX_THREADS 8

/* Up to max (at most OF_MAX_THREADS) CPUs out of those this process may run on, one per physical core: the fastest cores first (cpufreq's cpuinfo_max_freq, which tells a hybrid CPU's performance cores from its efficiency
 * cores), then the lowest number. How many were found, at least 1. */
int of_cpus_pick(int *cpus, int max);
/* The whole process onto those CPUs. Called before any other thread exists, every thread made later (this program's and the libraries') inherits it: nothing of the process runs outside the n cores. 0, or -1. */
int of_cpus_confine(const int *cpus, int n);
/* The calling thread onto one CPU. */
int of_cpus_pin(int cpu);
/* Threads in this process now, by /proc/self/task: the number the law is checked against, not assumed. -1 if unreadable. */
int of_threads_now(void);
/* The CPU's features this build needs (AVX2 and FMA3): 1 if both are there. */
int of_cpu_ok(void);

uint64_t of_now_ns(void);
/* The time-stamp counter's rate, measured once over 10 ms against CLOCK_MONOTONIC. */
uint64_t of_tsc_khz(void);

#endif
