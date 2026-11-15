/* cpus.c: see cpus.h. */
#define _GNU_SOURCE
#include "cpus.h"

#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

#include "vt_util.h"

static long read_long(const char *fmt, int cpu, long missing)
{
    char path[128];
    snprintf(path, sizeof path, fmt, cpu);
    FILE *f = fopen(path, "r");
    if (!f)
        return missing;
    long v = missing;
    if (fscanf(f, "%ld", &v) != 1)
        v = missing;
    fclose(f);
    return v;
}

int of_cpus_pick(int *cpus, int max)
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed)) {
        cpus[0] = 0;
        return 1;
    }
    if (max > OF_MAX_THREADS)
        max = OF_MAX_THREADS;
    /* One CPU for each core: a core is named by the first number of its thread_siblings_list (the same string on each of its hardware threads) and its package. No sysfs: every CPU counts as a core. */
    enum { CAP = 1024 };
    static int rep[CAP];
    static long core_key[CAP], khz[CAP];
    int n = 0;
    for (int c = 0; c < CPU_SETSIZE && n < CAP; c++) {
        if (!CPU_ISSET(c, &allowed))
            continue;
        const long key = read_long("/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c, c) | read_long("/sys/devices/system/cpu/cpu%d/topology/physical_package_id", c, 0) << 20;
        int seen = 0;
        for (int i = 0; i < n && !seen; i++)
            seen = core_key[i] == key;
        if (seen)
            continue;
        core_key[n] = key;
        khz[n] = read_long("/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c, 0);
        rep[n++] = c;
    }
    /* The fastest first; equal ones keep their order (insertion sort, tens of cores). */
    for (int i = 1; i < n; i++) {
        const int r = rep[i];
        const long k = khz[i];
        int j = i;
        for (; j > 0 && khz[j - 1] < k; j--) {
            rep[j] = rep[j - 1];
            khz[j] = khz[j - 1];
        }
        rep[j] = r;
        khz[j] = k;
    }
    const int take = n < max ? n : max;
    for (int i = 0; i < take; i++)
        cpus[i] = rep[i];
    if (!take)
        cpus[0] = 0;
    return take ? take : 1;
}

int of_cpus_confine(const int *cpus, int n)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < n; i++)
        CPU_SET(cpus[i], &set);
    return sched_setaffinity(0, sizeof set, &set) ? -1 : 0;
}

int of_cpus_pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) ? -1 : 0;
}

int of_threads_now(void)
{
    DIR *d = opendir("/proc/self/task");
    if (!d)
        return -1;
    int n = 0;
    for (const struct dirent *e; (e = readdir(d));)
        n += e->d_name[0] != '.';
    closedir(d);
    return n;
}

int of_cpu_ok(void)
{
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
}

uint64_t of_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

uint64_t of_tsc_khz(void)
{
    static uint64_t khz;
    if (khz)
        return khz;
    const uint64_t n0 = of_now_ns(), c0 = vt_tsc();
    const struct timespec nap = {0, 10000000};
    nanosleep(&nap, NULL);
    const uint64_t n1 = of_now_ns(), c1 = vt_tsc();
    khz = (c1 - c0) * 1000000ull / (n1 - n0);
    return khz ? khz : 1;
}
