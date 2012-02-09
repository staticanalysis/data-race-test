//===-- tsan_sync.h ---------------------------------------------*- C++ -*-===//
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
#ifndef TSAN_SYNC_H
#define TSAN_SYNC_H

#include "tsan_defs.h"
#include "tsan_clock.h"
#include <map>  // FIXME: remove me

namespace __tsan {

struct SyncVar {
  enum Type { Atomic, Mtx, Sem };

  SyncVar(Type type, uptr addr);

  const Type type;
  const uptr addr;
  // Mutex mtx;
  ChunkedClock clock;
};

struct MutexVar : SyncVar {
  MutexVar(uptr addr, bool is_rw);
  const bool is_rw;
};

class SyncTab {
 public:
  SyncTab();

  void insert(SyncVar *var);
  void remove(SyncVar *var);
  SyncVar* get_and_lock(uptr addr);

 private:
  // Mutex mtx_;
  typedef std::map<uptr, SyncVar*> tab_t;
  tab_t tab_;

  SyncTab(const SyncTab&);  // Not implemented.
  void operator = (const SyncTab&);  // Not implemented.
};

}  // namespace __tsan

#endif  // TSAN_SYNC_H
