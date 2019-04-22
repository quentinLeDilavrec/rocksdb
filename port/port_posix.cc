//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "port/port_posix.h"

#include <assert.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cstdlib>
#include "util/logging.h"

#include "rsg/engine.hpp"
#include "rsg/actor.hpp"

using namespace ::simgrid;

namespace rocksdb {
namespace port {

static int PthreadCall(const char* label, int result) {
  if (result != 0 && result != ETIMEDOUT) {
    fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
    abort();
  }
  return result;
}

Mutex::Mutex(bool adaptive) {
  if (rsg::isClient()) {
    rsgMutex = new rsg::Mutex();
  } else {
    (void) adaptive;
#ifdef ROCKSDB_PTHREAD_ADAPTIVE_MUTEX
    if (!adaptive) {
      PthreadCall("init mutex", pthread_mutex_init(&mu_, nullptr));
    } else {
      pthread_mutexattr_t mutex_attr;
      PthreadCall("init mutex attr", pthread_mutexattr_init(&mutex_attr));
      PthreadCall("set mutex attr",
                  pthread_mutexattr_settype(&mutex_attr,
                                            PTHREAD_MUTEX_ADAPTIVE_NP));
      PthreadCall("init mutex", pthread_mutex_init(&mu_, &mutex_attr));
      PthreadCall("destroy mutex attr",
                  pthread_mutexattr_destroy(&mutex_attr));
    }
#else
    PthreadCall("init mutex", pthread_mutex_init(&mu_, nullptr));
#endif // ROCKSDB_PTHREAD_ADAPTIVE_MUTEX
  }
}

Mutex::~Mutex() {
  if (rsg::isClient()) {
    rsgMutex->destroy();
    delete rsgMutex;
  } else {
    PthreadCall("destroy mutex", pthread_mutex_destroy(&mu_));
  }
}

void Mutex::Lock() {
  if (rsg::isClient()) {
    rsgMutex->lock();
  } else {
    PthreadCall("lock", pthread_mutex_lock(&mu_));
  }
#ifndef NDEBUG
    locked_ = true;
#endif
}

void Mutex::Unlock() {
#ifndef NDEBUG
  locked_ = false;
#endif
  if (rsg::isClient()) {
    rsgMutex->unlock();
  } else {
    PthreadCall("unlock", pthread_mutex_unlock(&mu_));
  }
}

void Mutex::AssertHeld() {
#ifndef NDEBUG
  assert(locked_);
#endif
}

CondVar::CondVar(Mutex* mu)
    : mu_(mu) {
  if (simgrid::rsg::isClient()) {
    rsgCond = new simgrid::rsg::ConditionVariable();
  } else {
    PthreadCall("init cv", pthread_cond_init(&cv_, nullptr));
  }
}

CondVar::~CondVar() {
  if (simgrid::rsg::isClient()) {
    rsgCond->destroy();
    delete rsgCond;
  } else {
    PthreadCall("destroy cv", pthread_cond_destroy(&cv_));
  }
}

void CondVar::Wait() {
#ifndef NDEBUG
  mu_->locked_ = false;
#endif
  if (simgrid::rsg::isClient()) {
    rsgCond->wait(mu_->rsgMutex);
  } else {
    PthreadCall("wait", pthread_cond_wait(&cv_, &mu_->mu_));
  }
#ifndef NDEBUG
  mu_->locked_ = true;
#endif
}

bool CondVar::TimedWait(uint64_t abs_time_us) {
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(abs_time_us / 1000000);
  ts.tv_nsec = static_cast<suseconds_t>((abs_time_us % 1000000) * 1000);

#ifndef NDEBUG
  mu_->locked_ = false;
#endif
  int err;
  if (simgrid::rsg::isClient()) {
    err = (int)rsgCond->wait_for(mu_->rsgMutex, (double(abs_time_us))/1000000.0);
  } else {
    err = pthread_cond_timedwait(&cv_, &mu_->mu_, &ts);
  }
#ifndef NDEBUG
  mu_->locked_ = true;
#endif
  if (err == ETIMEDOUT) {
    return true;
  }
  if (err != 0) {
    PthreadCall("timedwait", err);
  }
  return false;
}

void CondVar::Signal() {
  if (simgrid::rsg::isClient()) {
    rsgCond->notify_one();
  } else {
    PthreadCall("signal", pthread_cond_signal(&cv_));
  }
}

void CondVar::SignalAll() {
  if (simgrid::rsg::isClient()) {
    rsgCond->notify_all();
  } else {
    PthreadCall("broadcast", pthread_cond_broadcast(&cv_));
  }
}

RWMutex::RWMutex() {
  if (simgrid::rsg::isClient()) {
    rsgMutex = new simgrid::rsg::Mutex();
  } else {
    PthreadCall("init mutex", pthread_rwlock_init(&mu_, nullptr));
  }
}

RWMutex::~RWMutex() {
  if (simgrid::rsg::isClient()) {
    rsgMutex->destroy();
    delete rsgMutex;
  } else {
    PthreadCall("destroy mutex", pthread_rwlock_destroy(&mu_));
  }
}

void RWMutex::ReadLock() {
  if (simgrid::rsg::isClient()) {
    rsgMutex->lock();
  } else {
    PthreadCall("read lock", pthread_rwlock_rdlock(&mu_));
  }
}

void RWMutex::WriteLock() {
  if (simgrid::rsg::isClient()) {
    rsgMutex->lock();
  } else {
    PthreadCall("write lock", pthread_rwlock_wrlock(&mu_));
  }
}

void RWMutex::ReadUnlock() {
  if (simgrid::rsg::isClient()) {
    rsgMutex->unlock();
  } else {
    PthreadCall("read unlock", pthread_rwlock_unlock(&mu_));
  }
}

void RWMutex::WriteUnlock() {
  if (simgrid::rsg::isClient()) {
    rsgMutex->unlock();
  } else {
    PthreadCall("write unlock", pthread_rwlock_unlock(&mu_));
  }
}

int PhysicalCoreID() {
#if defined(ROCKSDB_SCHED_GETCPU_PRESENT) && defined(__x86_64__) && \
    (__GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 22))
  // sched_getcpu uses VDSO getcpu() syscall since 2.22. I believe Linux offers VDSO
  // support only on x86_64. This is the fastest/preferred method if available.
  int cpuno = sched_getcpu();
  if (cpuno < 0) {
    return -1;
  }
  return cpuno;
#elif defined(__x86_64__) || defined(__i386__)
  // clang/gcc both provide cpuid.h, which defines __get_cpuid(), for x86_64 and i386.
  unsigned eax, ebx = 0, ecx, edx;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
    return -1;
  }
  return ebx >> 24;
#else
  // give up, the caller can generate a random number or something.
  return -1;
#endif
}

void InitOnce(OnceType* once, void (*initializer)()) {
  PthreadCall("once", pthread_once(once, initializer));
}

void Crash(const std::string& srcfile, int srcline) {
  fprintf(stdout, "Crashing at %s:%d\n", srcfile.c_str(), srcline);
  fflush(stdout);
  if (simgrid::rsg::isClient()) {
    simgrid::rsg::this_actor::quit();
  }
  kill(getpid(), SIGTERM);
}

int GetMaxOpenFiles() {
#if defined(RLIMIT_NOFILE)
  struct rlimit no_files_limit;
  if (getrlimit(RLIMIT_NOFILE, &no_files_limit) != 0) {
    return -1;
  }
  // protect against overflow
  if (no_files_limit.rlim_cur >= std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(no_files_limit.rlim_cur);
#endif
  return -1;
}

void *cacheline_aligned_alloc(size_t size) {
#if __GNUC__ < 5 && defined(__SANITIZE_ADDRESS__)
  return malloc(size);
#elif ( _POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600 || defined(__APPLE__))
  void *m;
  errno = posix_memalign(&m, CACHE_LINE_SIZE, size);
  return errno ? nullptr : m;
#else
  return malloc(size);
#endif
}

void cacheline_aligned_free(void *memblock) {
  free(memblock);
}


void
instrumentedThread::swap(instrumentedThread& t) noexcept
{
  if (simgrid::rsg::isClient()) {
    std::swap(actor,t.actor); // TODO correct (probably not complete)
  }
  _thread->swap(*(t._thread));
}
bool
instrumentedThread::joinable() const noexcept
{ return _thread->joinable(); }
void
instrumentedThread::join(){
  if (simgrid::rsg::isClient()) {
    actor->join();
    delete actor;
  }else{
    _thread->join();
  }
}
void
instrumentedThread::detach(){
  _thread->detach();
}
std::thread::id
instrumentedThread::get_id() const noexcept
{ return _thread->get_id(); }
/** @pre thread is joinable
  */
std::thread::native_handle_type
instrumentedThread::native_handle(){
  return _thread->native_handle();
}

}  // namespace port
}  // namespace rocksdb
