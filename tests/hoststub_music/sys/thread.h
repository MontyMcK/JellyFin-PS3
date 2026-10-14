#pragma once
// Host stand-in for PSL1GHT's <sys/thread.h>: threads are pthreads.  Used by tests/test_music_engine.cpp,
// which compiles music/music_player.cpp itself.
#include <pthread.h>
#include <stdint.h>

typedef pthread_t sys_ppu_thread_t;
#define THREAD_JOINABLE 1

struct host_thread_start { void (*fn)(void *); void *arg; };

static inline void *host_thread_main(void *p) {
    host_thread_start s = *(host_thread_start *)p;
    delete (host_thread_start *)p;
    s.fn(s.arg);
    return NULL;
}

static inline int sysThreadCreate(sys_ppu_thread_t *t, void (*fn)(void *), void *arg, int prio, uint64_t stack,
                                  uint64_t flags, char *name) {
    (void)prio; (void)stack; (void)flags; (void)name;
    host_thread_start *s = new host_thread_start{ fn, arg };
    // The stack the console would give the thread, so a test fails if the code needs more than that.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack < 32768 ? 32768 : (size_t)stack);
    const int e = pthread_create(t, &attr, host_thread_main, s);
    pthread_attr_destroy(&attr);
    return e;
}

static inline int sysThreadJoin(sys_ppu_thread_t t, uint64_t *ret) {
    void *r;
    const int e = pthread_join(t, &r);
    if (ret) *ret = 0;
    return e;
}

static inline void sysThreadExit(int code) { (void)code; pthread_exit(NULL); }
