//===-- tsan_sync.cc --------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//
#include "tsan_sync.h"
#include "tsan_placement_new.h"
#include "tsan_rtl.h"
#include "tsan_mman.h"

namespace __tsan {

SyncVar::SyncVar(uptr addr)
  : mtx(MutexTypeSyncVar, StatMtxSyncVar)
  , addr(addr)
  , owner_tid(kInvalidTid)
  , recursion()
  , is_rw()
  , is_recursive()
  , is_broken() {
}

SyncTab::Part::Part()
  : mtx(MutexTypeSyncTab, StatMtxSyncTab)
  , val() {
}

SyncTab::SyncTab() {
}

SyncVar* SyncTab::GetAndLock(ThreadState *thr, uptr pc,
                             SlabCache *slab, uptr addr, bool write_lock) {
  DCHECK_EQ(slab->Size(), sizeof(SyncVar));
  Part *p = &tab_[PartIdx(addr)];
  {
    ReadLock l(&p->mtx);
    for (SyncVar *res = p->val; res; res = res->next) {
      if (res->addr == addr) {
        if (write_lock)
          res->mtx.Lock();
        else
          res->mtx.ReadLock();
        return res;
      }
    }
  }
  {
    Lock l(&p->mtx);
    SyncVar *res = p->val;
    for (; res; res = res->next) {
      if (res->addr == addr)
        break;
    }
    if (res == 0) {
      res = new(slab->Alloc()) SyncVar(addr);
      res->creation_stack.ObtainCurrent(thr, pc);
      res->next = p->val;
      p->val = res;
    }
    if (write_lock)
      res->mtx.Lock();
    else
      res->mtx.ReadLock();
    return res;
  }
}

SyncVar* SyncTab::GetAndRemove(uptr addr) {
  Part *p = &tab_[PartIdx(addr)];
  SyncVar *res = 0;
  {
    Lock l(&p->mtx);
    SyncVar **prev = &p->val;
    res = *prev;
    while (res) {
      if (res->addr == addr) {
        *prev = res->next;
        break;
      }
      prev = &res->next;
      res = *prev;
    }
  }
  if (res) {
    res->mtx.Lock();
    res->mtx.Unlock();
  }
  return res;
}

int SyncTab::PartIdx(uptr addr) {
  return (addr >> 3) % kPartCount;
}

StackTrace::StackTrace()
    : n_()
    , s_() {
}

StackTrace::~StackTrace() {
  CHECK_EQ(n_, 0);
  CHECK_EQ(s_, 0);
}

void StackTrace::Init(ThreadState *thr, uptr *pcs, uptr cnt) {
  Free(thr);
  if (cnt == 0)
    return;
  n_ = cnt;
  s_ = (uptr*)internal_alloc((n_) * sizeof(s_[0]));
  internal_memcpy(s_, pcs, n_ * sizeof(s_[0]));
}

void StackTrace::ObtainCurrent(ThreadState *thr, uptr toppc) {
  Free(thr);
  n_ = thr->shadow_stack_pos - &thr->shadow_stack[0];
  s_ = (uptr*)internal_alloc((n_ + !!toppc) * sizeof(s_[0]));
  for (uptr i = 0; i < n_; i++)
    s_[i] = thr->shadow_stack[i];
  if (toppc) {
    s_[n_] = toppc;
    n_++;
  }
}

void StackTrace::Free(ThreadState *thr) {
  if (s_) {
    CHECK_NE(n_, 0);
    internal_free(s_);
    s_ = 0;
    n_ = 0;
  }
}

bool StackTrace::IsEmpty() const {
  return n_ == 0;
}

uptr StackTrace::Size() const {
  return n_;
}

uptr StackTrace::Get(uptr i) const {
  CHECK_LT(i, n_);
  return s_[i];
}

const uptr *StackTrace::Begin() const {
  return s_;
}

}  // namespace __tsan
