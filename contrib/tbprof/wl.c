/*
 * Same-ISA fast-path study: guest workloads.
 *  - as PID 1 (/init in an initramfs): runs every workload listed on the
 *    kernel command line (wl=a,b,c) and powers off;
 *  - as a normal program: `wl <mode> [reps]` runs one workload (native baseline).
 * Every workload prints "WL <mode> <ns>".
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static volatile uint64_t sink;

/* STREAM-like triad over 3 x 8 MiB. */
static void w_mem(int reps)
{
    size_t n = 1 << 20;
    double *a = malloc(n * 8), *b = malloc(n * 8), *c = malloc(n * 8);
    for (size_t i = 0; i < n; i++) { a[i] = 0; b[i] = i; c[i] = 2 * i; }
    for (int r = 0; r < reps * 8; r++)
        for (size_t i = 0; i < n; i++) a[i] = b[i] + 3.0 * c[i];
    sink = (uint64_t)a[n / 2];
    free(a); free(b); free(c);
}

/* Random pointer chase over 64 MiB: one dependent load per step, mostly new pages. */
static void w_chase(int reps)
{
    size_t n = (64u << 20) / sizeof(size_t);
    size_t *p = malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) p[i] = i;
    uint64_t x = 88172645463325252ull;
    for (size_t i = n - 1; i > 0; i--) { /* Sattolo: one big cycle */
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        size_t j = x % i, t = p[i]; p[i] = p[j]; p[j] = t;
    }
    size_t k = 0;
    for (long s = 0; s < (long)reps * 4000000; s++) k = p[k];
    sink = k;
    free(p);
}

/* Null syscalls: exception entry/exit through the guest kernel. */
static void w_sys(int reps)
{
    for (long i = 0; i < (long)reps * 400000; i++) sink += syscall(SYS_getppid);
}

/* Page faults: map, touch every page, unmap. */
static void w_pf(int reps)
{
    for (int r = 0; r < reps * 8; r++) {
        size_t sz = 16u << 20;
        char *m = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        for (size_t o = 0; o < sz; o += 4096) m[o] = (char)o;
        munmap(m, sz);
    }
}

/* Auto-vectorised integer/float kernels (NEON). */
static void w_simd(int reps)
{
    size_t n = 1 << 14;
    float *x = malloc(n * 4), *y = malloc(n * 4);
    int32_t *u = malloc(n * 4), *v = malloc(n * 4);
    for (size_t i = 0; i < n; i++) { x[i] = i; y[i] = 1; u[i] = i; v[i] = 3 * i; }
    for (int r = 0; r < reps * 800; r++) {
        for (size_t i = 0; i < n; i++) y[i] = 1.0001f * x[i] + y[i];
        for (size_t i = 0; i < n; i++) u[i] = (u[i] ^ v[i]) + (v[i] >> 3);
    }
    sink = (uint64_t)y[7] + u[9];
    free(x); free(y); free(u); free(v);
}

/* Branchy integer code: a small interpreter-like switch loop + hashing. */
static void w_int(int reps)
{
    uint32_t h = 2166136261u, acc = 0;
    for (long i = 0; i < (long)reps * 20000000; i++) {
        switch ((h >> 7) & 7) {
        case 0: acc += h; break;
        case 1: acc ^= h << 3; break;
        case 2: acc -= h >> 2; break;
        case 3: acc = acc * 31 + 7; break;
        case 4: acc |= 1; break;
        case 5: acc = (acc >> 1) | (acc << 31); break;
        default: acc += i; break;
        }
        h = (h ^ (uint32_t)i) * 16777619u;
    }
    sink = acc;
}

struct wl { const char *name; void (*fn)(int); };
static const struct wl wls[] = {
    {"int", w_int}, {"mem", w_mem}, {"chase", w_chase}, {"sys", w_sys},
    {"pf", w_pf}, {"simd", w_simd}, {0, 0},
};

static int run_one(const char *name, int reps)
{
    if (!strcmp(name, "cm")) { /* CoreMark: separate binary */
        char it[32];
        snprintf(it, sizeof(it), "%d", 4000 * reps);
        uint64_t t0 = now();
        pid_t pid = fork();
        if (pid == 0) {
            char *av[] = {"/cm", "0x0", "0x0", "0x66", it, 0};
            execv(av[0], av);
            _exit(127);
        }
        int st;
        waitpid(pid, &st, 0);
        printf("WL cm %llu\n", (unsigned long long)(now() - t0));
        return 0;
    }
    for (const struct wl *w = wls; w->name; w++) {
        if (!strcmp(w->name, name)) {
            uint64_t t0 = now();
            w->fn(reps);
            printf("WL %s %llu\n", name, (unsigned long long)(now() - t0));
            fflush(stdout);
            return 0;
        }
    }
    printf("WL %s unknown\n", name);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    if (getpid() != 1) {
        if (argc < 2) { fprintf(stderr, "usage: wl <mode> [reps]\n"); return 2; }
        return run_one(argv[1], argc > 2 ? atoi(argv[2]) : 1);
    }
    /* PID 1: console is already set up by the kernel (console=ttyAMA0). */
    mount("proc", "/proc", "proc", 0, 0);
    char cmd[512] = "";
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) { int n = read(fd, cmd, sizeof(cmd) - 1); if (n > 0) cmd[n] = 0; close(fd); }
    char *list = strstr(cmd, "wl="), *rp = strstr(cmd, "reps=");
    int reps = rp ? atoi(rp + 5) : 1;
    printf("INIT start reps=%d\n", reps);
    if (list) {
        list += 3;
        char *end = strpbrk(list, " \n");
        if (end) *end = 0;
        for (char *tok = strtok(list, ","); tok; tok = strtok(0, ",")) {
            char *colon = strchr(tok, ':');
            int r = reps;
            if (colon) { *colon = 0; r = atoi(colon + 1); }
            run_one(tok, r);
        }
    }
    printf("INIT done\n");
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
