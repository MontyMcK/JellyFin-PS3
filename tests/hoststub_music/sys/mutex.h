#pragma once
// Host stand-in for PSL1GHT's <sys/mutex.h> (pthread mutexes), for tests/test_music_engine.cpp.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

typedef pthread_mutex_t *sys_mutex_t;
typedef int sys_mutex_attr_t;

#define sysMutexAttrInitialize(a) ((a) = 0)

static inline int sysMutexCreate(sys_mutex_t *m, sys_mutex_attr_t *a) {
    (void)a;
    *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    return pthread_mutex_init(*m, NULL);
}
static inline int sysMutexLock(sys_mutex_t m, uint64_t timeout) { (void)timeout; return pthread_mutex_lock(m); }
static inline int sysMutexUnlock(sys_mutex_t m) { return pthread_mutex_unlock(m); }
