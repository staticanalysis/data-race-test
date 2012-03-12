//===-- tsan_report.cc ------------------------------------------*- C++ -*-===//
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
#include "tsan_report.h"
#include "tsan_rtl.h"

namespace __tsan {

static void PrintStack(const ReportStack *stack) {
  for (int i = 0; i < stack->cnt; i++) {
    const ReportStackEntry *ent = &stack->entry[i];
    Printf("    #%d %p: %s %s:%d\n", i,
           (void*)ent->pc, ent->func, ent->file, ent->line);
  }
}

void PrintReport(const ReportDesc *rep) {
  Printf("==================\n");
  Printf("WARNING: ThreadSanitizer: data race\n");
  for (int i = 0; i < rep->nmop; i++) {
    const ReportMop *mop = &rep->mop[i];
    Printf("  %s%s of size %d at %p by thread %d:\n",
           (i ? "Previous " : ""),
           (mop->write ? "Write" : "Read"),
           mop->size, (void*)mop->addr, mop->tid);
    PrintStack(&mop->stack);
  }
  if (rep->loc) {
    const ReportLocation *loc = rep->loc;
    if (loc->type == ReportLocationGlobal) {
      Printf("  Location is global %s of size %d at %p\n",
             loc->name, loc->size, loc->addr);
    } else if (loc->type == ReportLocationHeap) {
      Printf("  Location is heap of size %d at %p allocated by thread %d:\n",
             loc->size, loc->addr, loc->tid);
      PrintStack(&loc->stack);
    } else if (loc->type == ReportLocationStack) {
      Printf("  Location is stack of thread %d:\n", loc->tid);
    }
  }
  Printf("==================\n");
}

bool OnReport(const ReportDesc *rep, bool suppressed) {
  (void)rep;
  return suppressed;
}

void PrintStats(u64 *stat) {
  const char *name[StatCnt] = {};
  name[StatMop]                 = "Memory accesses";
  name[StatMopRead]             = "  Including reads";
  name[StatMopWrite]            = "            writes";
  name[StatMop1]                = "  Including size 1";
  name[StatMop2]                = "            size 2";
  name[StatMop4]                = "            size 4";
  name[StatMop8]                = "            size 8";
  name[StatShadowProcessed]     = "Shadow processed";
  name[StatShadowZero]          = "  Including empty";
  name[StatShadowSameSize]      = "  Including same size";
  name[StatShadowIntersect]     = "            intersect";
  name[StatShadowNotIntersect]  = "            not intersect";
  name[StatShadowSameThread]    = "  Including same thread";
  name[StatShadowAnotherThread] = "            another thread";
  name[StatShadowReplace]       = "  Including evicted";
  name[StatFuncEnter]           = "Function entries";
  name[StatFuncExit]            = "Function exits";
  name[StatEvents]              = "Events collected";

  Printf("Statistics:\n");
  for (int i = 0; i < StatCnt; i++)
    Printf("%-30s: %llu\n", name[i], stat[i]);
}

}  // namespace __tsan