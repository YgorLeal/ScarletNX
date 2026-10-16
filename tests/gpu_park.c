/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu.c's queue with a channel whose stream waits (gpu_park): another channel's submission queued after it runs and
   releases the semaphore it waits for, the waiting channel's later jobs and a gpu_call stay behind it, a release the
   CPU writes is seen, and a syncpoint wait ends on the increment. The engines are stand-ins that record the 3D
   methods each channel runs; the platform's locks are pthreads */
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "hle/services/nvdrv.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------ what gpu.c reaches outside itself */
static u8 mem[0x10000];
u8* g_mem = mem;
_Atomic int g_cs_any, g_gfx_any, g_gfx_img_any;
u64 gmmu_translate(u64 gva) { return gva < sizeof mem ? gva : 0; } /* GPU addresses are guest ones, 0 unmapped */
void mem_query(u64 addr, MemBlock* out) {
    memset(out, 0, sizeof *out);
    out->addr = 0;
    out->size = sizeof mem;
    (void)addr;
}
void gpu_mem_written(u64 gva, u64 size) { (void)gva, (void)size; }
u64 gpu_ticks(void) { return 0; }
void r3d_const_flush(GpuChannel* ch) { (void)ch; }
void r3d_pending_flush(void) {}
int r3d_pending_on(void) { return 0; }

static pthread_mutex_t seen_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seen_cv = PTHREAD_COND_INITIALIZER;
static u32 seen[16]; /* the 3D methods' values, in the order they ran */
static u32 nseen;
static void see(u32 v) {
    pthread_mutex_lock(&seen_lk);
    if (nseen < 16) seen[nseen++] = v;
    pthread_cond_broadcast(&seen_cv);
    pthread_mutex_unlock(&seen_lk);
}
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)ch, (void)last;
    if (m == 0x100) see(v);
}
void compute_method(GpuChannel* ch, u32 m, u32 v, int last) { (void)ch, (void)m, (void)v, (void)last; }
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) { (void)ch, (void)m, (void)v; }
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) { (void)ch, (void)m, (void)v; }
void dma_method(GpuChannel* ch, u32 m, u32 v) { (void)ch, (void)m, (void)v; }

static u32 syncpt[256];
u32 syncpt_read(u32 id) { return __atomic_load_n(&syncpt[id & 255], __ATOMIC_SEQ_CST); }
void syncpt_incr(u32 id) {
    __atomic_add_fetch(&syncpt[id & 255], 1, __ATOMIC_SEQ_CST);
    gpu_kick();
}
static int no_async(u64* e, u32 n, u32 syncpt_id, u32 incs) {
    (void)e, (void)n, (void)syncpt_id, (void)incs;
    return 0;
}
static void nothing(void) {}
static const Renderer stand_in = {
    .name = "stand-in", .async_submit = no_async, .before_syncwait = nothing, .stream_incr = syncpt_incr};
const Renderer* g_renderer = &stand_in;

static int warned;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!lvl) {
        __atomic_add_fetch(&warned, 1, __ATOMIC_SEQ_CST);
        vfprintf(stderr, fmt, ap);
    }
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

u64 plat_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
}
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    if (ns < 0) return pthread_cond_wait(&c->c, &m->l), 0;
    u64 t = plat_ns() + (u64)ns;
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    return pthread_cond_timedwait(&c->c, &m->l, &ts) != 0;
}
static PThreadFn thread_fn;
static void* thread_main(void* arg) {
    thread_fn(arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    pthread_t t;
    (void)stack;
    thread_fn = fn;
    return pthread_create(&t, NULL, thread_main, arg) != 0;
}

/* ------------------------------------------------ pushbuffers */
#define SEM  0x8000 /* the semaphores */
#define PUSH 0x1000 /* the pushbuffers, 64 words apart */
static u32 npush;
/* a GPFIFO entry for these words */
static u64 push(const u32* w, u32 n) {
    u64 va = PUSH + (u64)npush++ * 256;
    memcpy(mem + va, w, (u64)n * 4);
    return va | ((u64)n << 42);
}
static u32 inc_header(u32 m, u32 n) { return (1u << 29) | (n << 16) | m; } /* subchannel 0, incrementing */
static u32 imm(u32 m, u32 v) { return (4u << 29) | (v << 16) | m; }        /* subchannel 0, immediate */
/* SemaphoreOperation op on the u32 at addr with payload v, then 3D method 0x100 = mark */
static u64 sem_then(u64 addr, u32 v, u32 op, u32 mark) {
    u32 w[6] = {inc_header(4, 4), (u32)(addr >> 32), (u32)addr, v, op, imm(0x100, mark)};
    return push(w, 6);
}
static u64 mark(u32 m) {
    u32 w[1] = {imm(0x100, m)};
    return push(w, 1);
}
static void submit(u64 e, u32 channel) { gpu_submit(&e, 1, channel, 0, ~0u, 0, 0); }
/* waits up to 1 s for n marks, and says how long it took */
static u64 wait_marks(u32 n) {
    u64 t0 = plat_ns();
    pthread_mutex_lock(&seen_lk);
    while (nseen < n && plat_ns() - t0 < 1000000000ull) {
        u64 t = plat_ns() + 10000000ull;
        struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
        pthread_cond_timedwait(&seen_cv, &seen_lk, &ts);
    }
    pthread_mutex_unlock(&seen_lk);
    return plat_ns() - t0;
}
static void called(void* arg) { see((u32)(uintptr_t)arg); }
static void forget_marks(void) {
    pthread_mutex_lock(&seen_lk);
    nseen = 0;
    pthread_mutex_unlock(&seen_lk);
}

static int failures;
static void expect(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    struct timespec ms20 = {0, 20000000};
    gpu_init();

    /* channel 21 waits for SEM to reach 1, then its later job; a call queued after them; channel 22's job queued last
       releases it (op 2, bit 24: one word) */
    submit(sem_then(SEM, 1, 4, 0xA1), 21);
    submit(mark(0xA2), 21);
    gpu_call(called, (void*)(uintptr_t)0xC0);
    submit(sem_then(SEM, 1, 2 | (1u << 24), 0xB1), 22);
    u64 took = wait_marks(4);
    expect(nseen == 4, "the waiting channel ran once the other channel released its semaphore");
    expect(took < 500000000ull, "the release ended the wait without waiting out its timeout");
    expect(nseen == 4 && seen[0] == 0xB1 && seen[1] == 0xA1 && seen[2] == 0xA2 && seen[3] == 0xC0,
           "the releasing channel first, then the waiting channel's jobs in order, then the call");

    /* the CPU writes the value an equal acquire waits for */
    forget_marks();
    submit(sem_then(SEM + 16, 7, 1, 0xD1), 21);
    nanosleep(&ms20, NULL);
    expect(__atomic_load_n(&nseen, __ATOMIC_SEQ_CST) == 0, "an equal acquire waits while the value differs");
    __atomic_store_n((u32*)(mem + SEM + 16), 7, __ATOMIC_SEQ_CST);
    took = wait_marks(1);
    expect(nseen == 1 && seen[0] == 0xD1, "a release the CPU wrote ended the wait");
    expect(took < 100000000ull, "the CPU's release was seen soon after");

    /* a submission waiting for a syncpoint's fence, incremented off the GPU thread */
    forget_marks();
    u64 e = mark(0xE1);
    gpu_submit(&e, 1, 23, 0, 30, 1, 0);
    nanosleep(&ms20, NULL);
    expect(__atomic_load_n(&nseen, __ATOMIC_SEQ_CST) == 0, "a submission waits for its fence");
    syncpt_incr(30);
    took = wait_marks(1);
    expect(nseen == 1 && seen[0] == 0xE1, "the syncpoint increment started the submission");
    expect(took < 100000000ull, "the increment woke the GPU thread");

    expect(!warned, "no wait timed out");
    if (!failures) printf("gpu_park: ok\n");
    return failures != 0;
}
