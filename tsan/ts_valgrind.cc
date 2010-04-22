/*
  This file is part of ThreadSanitizer, a dynamic data race detector
  based on Valgrind.

  Copyright (C) 2008-2009 Google Inc
     opensource@google.com
  Copyright (C) 2007-2008 OpenWorks LLP
      info@open-works.co.uk

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
  02111-1307, USA.

  The GNU General Public License is contained in the file COPYING.
*/

// Author: Konstantin Serebryany.
// Parts of the code in this file are taken from Helgrind,
// a data race detector written by Julian Seward.

#include "ts_valgrind.h"
#include "valgrind.h"
#include "ts_valgrind_client_requests.h"
#include "thread_sanitizer.h"


//---------------------- C++ malloc support -------------- {{{1
class MallocCostCenterStack {
 public:
  void Push(const char *cc) {
    DCHECK(size_ < kMaxMallocStackSize);
    DCHECK(cc);
    malloc_cost_centers_[size_++] = cc;
  }
  void Pop() {
    DCHECK(size_ > 0);
    size_--;
  }
  const char *Top() {
    return size_ ? malloc_cost_centers_[size_ - 1] : "default_cc";
  }
 private:
  static const int kMaxMallocStackSize = 100;
  int size_;
  const char *malloc_cost_centers_[kMaxMallocStackSize];
};

// Not thread-safe. Need to make it thread-local once we are multi-threaded.
static MallocCostCenterStack g_malloc_stack;

void PushMallocCostCenter(const char *cc) { g_malloc_stack.Push(cc); }
void PopMallocCostCenter() { g_malloc_stack.Pop(); }


void *operator new (size_t size) {
  return VG_(malloc)((HChar*)g_malloc_stack.Top(), size);
}
void *operator new [](size_t size) {
  return VG_(malloc)((HChar*)g_malloc_stack.Top(), size);
}
void operator delete (void *p) {
  VG_(free)(p);
}
void operator delete [](void *p) {
  VG_(free)(p);
}

extern "C" void *malloc(size_t size) {
  return VG_(malloc)((HChar*)g_malloc_stack.Top(), size);
}

extern "C" void free(void *ptr) {
  VG_(free)(ptr);
}


//---------------------- Utils ------------------- {{{1

extern "C" int puts(const char *s) {
  Printf("%s", s);
  return 1;
}

extern "C" void exit(int e) { VG_(exit)(e); }

#ifndef VGP_arm_linux
extern "C" void abort() { CHECK(0); }
#endif


// TODO: make this rtn public
extern "C" {
  Bool VG_(get_fnname_no_cxx_demangle) ( Addr a, Char* buf, Int nbuf );
}


const int kBuffSize = 1024 * 10 - 1;
// not thread-safe.
static char g_buff1[kBuffSize+1];
static char g_buff2[kBuffSize+1];

string PcToRtnName(uintptr_t pc, bool demangle) {
  if (demangle) {
    if(VG_(get_fnname)(pc, (Char*)g_buff1, kBuffSize)) {
      return g_buff1;
    }
  } else {
    if(VG_(get_fnname_no_cxx_demangle)(pc, (Char*)g_buff1, kBuffSize)) {
      return g_buff1;
    }
  }
  string res = "???";
  if (VG_(get_objname)(pc, (Char*)g_buff1, kBuffSize)) {
    res += string("/") + g_buff1;
  }
  return res;
}

void PcToStrings(uintptr_t pc, bool demangle,
                string *img_name, string *rtn_name,
                string *file_name, int *line_no) {
  const int kBuffSize = 1024 * 10 - 1;
  Bool has_dirname = False;

  if (VG_(get_filename_linenum)
      (pc, (Char*)g_buff1, kBuffSize, (Char*)g_buff2, kBuffSize,
       &has_dirname, (UInt*)line_no) &&
      has_dirname) {
    *file_name = string(g_buff2) + "/" + g_buff1;
  } else {
    VG_(get_linenum)(pc, (UInt *)line_no);
    if (VG_(get_filename)(pc, (Char*)g_buff1, kBuffSize)) {
      *file_name = g_buff1;
    }
  }
  *file_name = ConvertToPlatformIndependentPath(*file_name);

  *rtn_name = PcToRtnName(pc, demangle);

  if (VG_(get_objname)(pc, (Char*)g_buff1, kBuffSize)) {
    *img_name = g_buff1;
  }
}



string Demangle(const char *str) {
  return str;
}

extern "C"
size_t strlen(const char *s) {
  return VG_(strlen)((const Char*)s);
}

static inline ThreadId GetVgTid() {
  extern ThreadId VG_(running_tid); // HACK: avoid calling get_running_tid()
  ThreadId res = VG_(running_tid);
  //DCHECK(res == VG_(get_running_tid)());
  return res;
}

static inline uintptr_t GetVgPc(ThreadId vg_tid) {
  return (uintptr_t)VG_(get_IP)(vg_tid);
}

#ifdef VGP_arm_linux
static inline uintptr_t GetVgLr(ThreadId vg_tid) {
  return (uintptr_t) VG_(get_LR)(vg_tid);
}
#endif

uintptr_t GetPcOfCurrentThread() {
  return GetVgPc(GetVgTid());
}

void GetThreadStack(int tid, uintptr_t *min_addr, uintptr_t *max_addr) {
  // tid is not used because we call it from the current thread anyway.
  uintptr_t stack_max  = VG_(thread_get_stack_max)(GetVgTid());
  uintptr_t stack_size = VG_(thread_get_stack_size)(GetVgTid());
  uintptr_t stack_min  = stack_max - stack_size;
  *min_addr = stack_min;
  *max_addr = stack_max;
}



struct CallStackRecord {
  Addr pc;
  Addr sp;
#ifdef VGP_arm_linux
  // We need to store LR in order to keep the shadow stack consistent.
  Addr lr;
#endif
};

struct ValgrindThread {
  int32_t zero_based_uniq_tid;
  vector<CallStackRecord> call_stack;

  int ignore_accesses;
  bool ignore_accesses_in_current_trace;
  int ignore_sync;
  int in_signal_handler;

  ValgrindThread() {
    Clear();
  }

  void Clear() {
    zero_based_uniq_tid = -1;
    ignore_accesses = 0;
    ignore_accesses_in_current_trace = false;
    ignore_sync = 0;
    in_signal_handler = 0;
    call_stack.clear();
  }
};

// If true, ignore all accesses in all threads.
static bool global_ignore;

// Array of VG_N_THREADS
static ValgrindThread *g_valgrind_threads = 0;
static map<uintptr_t, int> *g_ptid_to_ts_tid;

// maintains a uniq thread id (first thread will have id=0)
static int32_t g_uniq_thread_id_counter = 0;

static int32_t VgTidToTsTid(ThreadId vg_tid) {
  DCHECK(vg_tid < VG_N_THREADS);
  DCHECK(vg_tid >= 1);
  DCHECK(g_valgrind_threads);
  DCHECK(g_valgrind_threads[vg_tid].zero_based_uniq_tid >= 0);
  return g_valgrind_threads[vg_tid].zero_based_uniq_tid;
}


static vector<string> *g_command_line_options = 0;
static void InitCommandLineOptions() {
  if(G_flags == NULL) {
    G_flags = new FLAGS;
  }
  if (g_command_line_options == NULL) {
    g_command_line_options = new vector<string>;
  }
}

Bool ts_process_cmd_line_option (Char* arg) {
  InitCommandLineOptions();
  g_command_line_options->push_back((char*)arg);
  return True;
}

void ts_print_usage (void) {
  InitCommandLineOptions();
  ThreadSanitizerParseFlags(g_command_line_options);

  ThreadSanitizerPrintUsage();
}

void ts_print_debug_usage(void) {
  Printf("TODO\n");
}


void evh__die_mem ( Addr a, SizeT len ) {
}


extern int VG_(clo_error_exitcode);

void ts_post_clo_init(void) {
  InitCommandLineOptions();
  ThreadSanitizerParseFlags(g_command_line_options);

  // we get num-callers from valgrind flags.
  G_flags->num_callers = VG_(clo_backtrace_size);
  G_flags->error_exitcode = VG_(clo_error_exitcode);

  extern Int   VG_(clo_n_suppressions);
  extern Int   VG_(clo_gen_suppressions);
  extern Char* VG_(clo_suppressions)[];
  // get the suppressions from Valgrind
  for (int i = 0; i < VG_(clo_n_suppressions); i++) {
    G_flags->suppressions.push_back((char*)VG_(clo_suppressions)[i]);
  }
  G_flags->generate_suppressions |= VG_(clo_gen_suppressions) >= 1;

  if (G_flags->html) {
    Report("<pre>\n"
           "<br id=race0>"
           "<a href=\"#race1\">Go to first race report</a>\n");
  }
  Report("ThreadSanitizerValgrind r%s: "
         "pure-happens-before=%s fast-mode=%s ignore-in-dtor=%s\n",
         TS_VERSION,
         G_flags->pure_happens_before ? "yes" : "no",
         G_flags->fast_mode ? "yes" : "no",
         G_flags->ignore_in_dtor ? "yes" : "no");
  if (DEBUG_MODE) {
    Report("INFO: Debug build\n");
  }
  if (G_flags->max_mem_in_mb) {
    Report("INFO: ThreadSanitizer memory limit: %dMB\n",
           (int)G_flags->max_mem_in_mb);
  }
  ThreadSanitizerInit();

  g_valgrind_threads = new ValgrindThread[VG_N_THREADS];
  g_ptid_to_ts_tid = new map<uintptr_t, int>;
}

static inline void Put(EventType type, int32_t tid, uintptr_t pc,
                       uintptr_t a, uintptr_t info) {
  if (DEBUG_MODE && G_flags->dry_run >= 1) return;
  Event event(type, tid, pc, a, info);
  ThreadSanitizerHandleOneEvent(&event);
}

static void rtn_call(Addr sp_post_call_insn, Addr pc_post_call_insn,
                     IGNORE_BELOW_RTN ignore_below) {
  ThreadId vg_tid = GetVgTid();
  CallStackRecord record;
  record.pc = pc_post_call_insn;
  record.sp = sp_post_call_insn;
#ifdef VGP_arm_linux
  record.lr = GetVgLr(vg_tid);
#endif
  g_valgrind_threads[vg_tid].call_stack.push_back(record);
  // If the shadow stack grows too high this usually means it is not cleaned
  // properly. Or this may be a very deep recursion.
  DCHECK(g_valgrind_threads[vg_tid].call_stack.size() < 10000);
  uintptr_t call_pc = GetVgPc(vg_tid);

  ThreadSanitizerHandleRtnCall(VgTidToTsTid(vg_tid), call_pc, record.pc,
                               ignore_below);


  if (G_flags->verbosity >= 2) {
    Printf("T%d: >>: %s\n", VgTidToTsTid(vg_tid),
           PcToRtnNameAndFilePos(record.pc).c_str());
  }
}

VG_REGPARM(2) void evh__rtn_call_ignore_unknown ( Addr sp, Addr pc) {
  rtn_call(sp, pc, IGNORE_BELOW_RTN_UNKNOWN);
}
VG_REGPARM(2) void evh__rtn_call_ignore_yes ( Addr sp, Addr pc) {
  rtn_call(sp, pc, IGNORE_BELOW_RTN_YES);
}
VG_REGPARM(2) void evh__rtn_call_ignore_no ( Addr sp, Addr pc) {
  rtn_call(sp, pc, IGNORE_BELOW_RTN_NO);
}

#ifdef VGP_arm_linux
// Handle shadow stack frame deletion on ARM.
// Instrumented code calls this function for each non-call jump out of
// a superblock. If the |sp_post_call_insn| (the jump target address) is equal
// to a link register value of one or more frames on top of the shadow stack,
// those frames are popped out.
// TODO(glider): there may be problems with optimized recursive functions that
// don't change PC, SP and LR.
VG_REGPARM(2)
void evh__delete_frame ( Addr sp_post_call_insn,
                         Addr pc_post_call_insn) {
  ThreadId vg_tid = GetVgTid();
  vector<CallStackRecord> &call_stack = g_valgrind_threads[vg_tid].call_stack;
  int32_t ts_tid = VgTidToTsTid(vg_tid);
  while (!call_stack.empty()) {
    CallStackRecord &record = call_stack.back();
    if (record.lr != pc_post_call_insn) break;
    call_stack.pop_back();
    ThreadSanitizerHandleRtnExit(ts_tid);
  }
}
#endif

// TODO(kcc): remove this crap completely as soon as we are sure
// it is indeed crap.
#if 0
static INLINE void evh__new_mem_stack_helper ( Addr a, SizeT len ) {
  CHECK(0);
  ThreadId vg_tid = GetVgTid();
  if (!g_valgrind_threads[vg_tid].ignore_accesses) {
    // avoid stack updates when ignore is on.
    // TODO: is that right?
    int32_t ts_tid = VgTidToTsTid(vg_tid);
    ThreadSanitizerHandleStackMemChange(ts_tid, a, len, true);
  }
}

static void evh__new_mem_stack ( Addr a, SizeT len ) {
  evh__new_mem_stack_helper(a, len);
}
VG_REGPARM(1)
static void evh__new_mem_stack_8 ( Addr a) {
  evh__new_mem_stack_helper(a, 8);
}
VG_REGPARM(1)
static void evh__new_mem_stack_16 ( Addr a) {
  evh__new_mem_stack_helper(a, 16);
}
VG_REGPARM(1)
static void evh__new_mem_stack_32 ( Addr a) {
  evh__new_mem_stack_helper(a, 32);
}
#endif


static INLINE void evh__die_mem_stack_helper ( Addr a, SizeT len ) {
  ThreadId vg_tid = GetVgTid();
  int32_t ts_tid = VgTidToTsTid(vg_tid);
  vector<CallStackRecord> &call_stack = g_valgrind_threads[vg_tid].call_stack;
  while (!call_stack.empty()) {
    CallStackRecord &record = call_stack.back();
    Addr cur_top = record.sp;
    if (a < cur_top) break;
    call_stack.pop_back();
    ThreadSanitizerHandleRtnExit(ts_tid);
    // Put(RTN_EXIT, ts_tid, 0, 0, 0);
    if (G_flags->verbosity >= 2) {
      Printf("T%d: <<\n", ts_tid);
    }
    break;
  }
  if (G_flags->verbosity >= 2) {
    // Printf("T%d: -sp: %p => %p (%ld)\n", ts_tid, a, a + len, len);
  }
  if (!g_valgrind_threads[vg_tid].ignore_accesses) {
    ThreadSanitizerHandleStackMemChange(ts_tid, a, len, false);
    // Put(STACK_MEM_DIE, ts_tid, 0, a, len);
  }
}
static void evh__die_mem_stack ( Addr a, SizeT len ) {
//  Printf("** %d\n", (int)len);
  evh__die_mem_stack_helper(a, len);
}
VG_REGPARM(1)
static void evh__die_mem_stack_8 ( Addr a) {
//  Printf("** 8\n");
  evh__die_mem_stack_helper(a, 8);
}
VG_REGPARM(1)
static void evh__die_mem_stack_16 ( Addr a ) {
//  Printf("** 16\n");
  evh__die_mem_stack_helper(a, 16);
}
VG_REGPARM(1)
static void evh__die_mem_stack_32 ( Addr a ) {
//  Printf("** 32\n");
  evh__die_mem_stack_helper(a, 32);
}


void ts_fini(Int exitcode) {
  ThreadSanitizerFini();
  if (G_flags->error_exitcode && GetNumberOfFoundErrors() > 0) {
    exit(G_flags->error_exitcode);
  }
}


void evh__pre_thread_ll_create ( ThreadId parent, ThreadId child ) {
  tl_assert(parent != child);
  //  Printf("thread_create: %d->%d\n", parent, child);
  if (g_valgrind_threads[child].zero_based_uniq_tid != -1) {
    Printf("ThreadSanitizer WARNING: reusing TID %d w/o exiting thread\n",
           child);
  }
  g_valgrind_threads[child].Clear();
  g_valgrind_threads[child].zero_based_uniq_tid = g_uniq_thread_id_counter++;
  // Printf("VG: T%d: VG_THR_START: parent=%d\n", VgTidToTsTid(child), VgTidToTsTid(parent));
  uintptr_t pc = GetVgPc(parent);
  Put(THR_START, VgTidToTsTid(child), pc, 0,
      parent > 0 ? VgTidToTsTid(parent) : 0);
}

void evh__pre_workq_task_start(ThreadId vg_tid, Addr workitem) {
  uintptr_t pc = GetVgPc(vg_tid);
  int32_t ts_tid = VgTidToTsTid(vg_tid);
  Put(WAIT_BEFORE, ts_tid, pc, workitem, 0);
  Put(WAIT_AFTER, ts_tid, pc, 0, 0);
}

void evh__pre_thread_first_insn(const ThreadId tid) {
  Put(THR_FIRST_INSN, VgTidToTsTid(tid), GetVgPc(tid), 0, 0);
}


void evh__pre_thread_ll_exit ( ThreadId quit_tid ) {
//  Printf("thread_exit: %d\n", quit_tid);
//  Printf("T%d quiting thread; stack size=%ld\n",
//         VgTidToTsTid(quit_tid),
//         (int)g_valgrind_threads[quit_tid].call_stack.size());
  Put(THR_END, VgTidToTsTid(quit_tid), 0, 0, 0);
  g_valgrind_threads[quit_tid].zero_based_uniq_tid = -1;
}


// Memory operation...
static INLINE void Mop(Addr a, bool is_w, SizeT size) {
  ThreadId vg_tid = GetVgTid();
  ValgrindThread *thr = &g_valgrind_threads[vg_tid];
  if (thr->ignore_accesses) {
    //static int counter;
    //counter++;
    //if ((counter % 1024) == 0)
    //  Printf("ignore: %s\n", PcToRtnNameAndFilePos(pc).c_str());
    return;
  }
  ThreadSanitizerHandleMemoryAccess(VgTidToTsTid(vg_tid), a, size, is_w);
}


VG_REGPARM(1) void evh__mem_help_write_1(Addr a) { Mop(a, true, 1); }
VG_REGPARM(1) void evh__mem_help_write_2(Addr a) { Mop(a, true, 2); }
VG_REGPARM(1) void evh__mem_help_write_4(Addr a) { Mop(a, true, 4); }
VG_REGPARM(1) void evh__mem_help_write_8(Addr a) { Mop(a, true, 8); }
VG_REGPARM(1) void evh__mem_help_read_1(Addr a) { Mop(a, false, 1); }
VG_REGPARM(1) void evh__mem_help_read_2(Addr a) { Mop(a, false, 2); }
VG_REGPARM(1) void evh__mem_help_read_4(Addr a) { Mop(a, false, 4); }
VG_REGPARM(1) void evh__mem_help_read_8(Addr a) { Mop(a, false, 8); }
VG_REGPARM(2)
  void evh__mem_help_write_N(Addr a, SizeT size) { Mop(a, true, size); }
VG_REGPARM(2)
  void evh__mem_help_read_N(Addr a, SizeT size) { Mop(a, false, size); }


  extern "C" void VG_(show_all_errors)();

Bool ts_handle_client_request(ThreadId vg_tid, UWord* args, UWord* ret) {
  if (!VG_IS_TOOL_USERREQ('T', 'S', args[0]))
    return False;
  *ret = 0;
  uintptr_t pc = GetVgPc(vg_tid);
  int32_t ts_tid = VgTidToTsTid(vg_tid);
  switch (args[0]) {
    case TSREQ_SET_MY_PTHREAD_T:
      (*g_ptid_to_ts_tid)[args[1]] = ts_tid;
      break;
    case TSREQ_THR_STACK_TOP:
      Put(THR_STACK_TOP, ts_tid, pc, args[1], 0);
      break;
    case TSREQ_PTHREAD_JOIN_POST:
      Put(THR_JOIN_AFTER, ts_tid, pc, (*g_ptid_to_ts_tid)[args[1]], 0);
      break;
    case TSREQ_CLEAN_MEMORY:
      Put(MALLOC, ts_tid, pc, /*ptr=*/args[1], /*size=*/args[2]);
      break;
    case TSREQ_MAIN_IN:
      g_has_entered_main = true;
      // Report("INFO: Entred main(); argc=%d\n", (int)args[1]);
      break;
    case TSREQ_MAIN_OUT:
      g_has_exited_main = true;
      if (G_flags->exit_after_main) {
        Report("INFO: Exited main(); ret=%d\n", (int)args[1]);
        VG_(show_all_errors)();
        ThreadSanitizerFini();
        exit((int)args[1]);
      }
      break;
    case TSREQ_MALLOC:
      // Printf("Malloc: %p %ld\n", args[1], args[2]);
      Put(MALLOC, ts_tid, pc, /*ptr=*/args[1], /*size=*/args[2]);
      break;
    case TSREQ_FREE:
      // Printf("Free: %p\n", args[1]);
      Put(FREE, ts_tid, pc, /*ptr=*/args[1], 0);
      break;
    case TSREQ_BENIGN_RACE:
      Put(BENIGN_RACE, ts_tid, /*descr=*/args[3],
          /*p=*/args[1], /*size=*/args[2]);
      break;
    case TSREQ_EXPECT_RACE:
      Put(EXPECT_RACE, ts_tid, /*descr=*/args[3],
          /*p=*/args[1], /*size*/args[2]);
      break;
    case TSREQ_PCQ_CREATE:
      Put(PCQ_CREATE, ts_tid, pc, /*pcq=*/args[1], 0);
      break;
    case TSREQ_PCQ_DESTROY:
      Put(PCQ_DESTROY, ts_tid, pc, /*pcq=*/args[1], 0);
      break;
    case TSREQ_PCQ_PUT:
      Put(PCQ_PUT, ts_tid, pc, /*pcq=*/args[1], 0);
      break;
    case TSREQ_PCQ_GET:
      Put(PCQ_GET, ts_tid, pc, /*pcq=*/args[1], 0);
      break;
    case TSREQ_TRACE_MEM:
      Put(TRACE_MEM, ts_tid, pc, /*mem=*/args[1], 0);
      break;
    case TSREQ_MUTEX_IS_USED_AS_CONDVAR:
      Put(HB_LOCK, ts_tid, pc, /*lock=*/args[1], 0);
      break;
    case TSREQ_GLOBAL_IGNORE_ON:
      Report("INFO: GLOBAL IGNORE ON\n");
      global_ignore = true;
      break;
    case TSREQ_GLOBAL_IGNORE_OFF:
      Report("INFO: GLOBAL IGNORE OFF\n");
      global_ignore = false;
      break;
    case TSREQ_IGNORE_READS_BEGIN:
      Put(IGNORE_READS_BEG, ts_tid, pc, 0, 0);
      break;
    case TSREQ_IGNORE_READS_END:
      Put(IGNORE_READS_END, ts_tid, pc, 0, 0);
      break;
    case TSREQ_IGNORE_WRITES_BEGIN:
      Put(IGNORE_WRITES_BEG, ts_tid, pc, 0, 0);
      break;
    case TSREQ_IGNORE_WRITES_END:
      Put(IGNORE_WRITES_END, ts_tid, pc, 0, 0);
      break;
    case TSREQ_SET_THREAD_NAME:
      Put(SET_THREAD_NAME, ts_tid, pc, /*name=*/args[1], 0);
      break;
    case TSREQ_SET_LOCK_NAME:
      Put(SET_LOCK_NAME, ts_tid, pc, /*lock=*/args[1], /*name=*/args[2]);
      break;
    case TSREQ_IGNORE_ALL_ACCESSES_BEGIN:
      g_valgrind_threads[vg_tid].ignore_accesses++;
      break;
    case TSREQ_IGNORE_ALL_ACCESSES_END:
      g_valgrind_threads[vg_tid].ignore_accesses--;
      CHECK(g_valgrind_threads[vg_tid].ignore_accesses >= 0);
      break;
    case TSREQ_IGNORE_ALL_SYNC_BEGIN:
      g_valgrind_threads[vg_tid].ignore_sync++;
      break;
    case TSREQ_IGNORE_ALL_SYNC_END:
      g_valgrind_threads[vg_tid].ignore_sync--;
      CHECK(g_valgrind_threads[vg_tid].ignore_sync >= 0);
      break;
    case TSREQ_PUBLISH_MEMORY_RANGE:
      Put(PUBLISH_RANGE, ts_tid, pc, /*mem=*/args[1], /*size=*/args[2]);
      break;
    case TSREQ_UNPUBLISH_MEMORY_RANGE:
      Put(UNPUBLISH_RANGE, ts_tid, pc, /*mem=*/args[1], /*size=*/args[2]);
      break;
    case TSREQ_PRINT_MEMORY_USAGE:
    case TSREQ_PRINT_STATS:
    case TSREQ_RESET_STATS:
    case TSREQ_PTH_API_ERROR:
      break;
    case TSREQ_PTHREAD_COND_SIGNAL_PRE:
    case TSREQ_PTHREAD_COND_BROADCAST_PRE:
      Put(SIGNAL, ts_tid, pc, /*cv=*/args[1], 0);
      break;
    case TSREQ_PTHREAD_COND_WAIT_PRE:
      Put(WAIT_BEFORE, ts_tid, pc, /*cv=*/args[1], /*lock=*/args[2]);
      break;
    case TSREQ_PTHREAD_COND_WAIT_POST:
      Put(WAIT_AFTER, ts_tid, pc, 0, 0);
      break;
    case TSREQ_PTHREAD_COND_TWAIT_POST:
      Put(TWAIT_AFTER, ts_tid, pc, 0, 0);
      break;
    case TSREQ_PTHREAD_RWLOCK_CREATE_POST:
      Put(LOCK_CREATE, ts_tid, pc, /*lock=*/args[1], 0);
      break;
    case TSREQ_PTHREAD_RWLOCK_DESTROY_PRE:
      Put(LOCK_DESTROY, ts_tid, pc, /*lock=*/args[1], 0);
      break;
    case TSREQ_PTHREAD_RWLOCK_LOCK_PRE:
      break;
    case TSREQ_PTHREAD_RWLOCK_LOCK_POST:
      // We ignore locking events if ignore_sync != 0 and if we are not
      // inside a signal handler.
      if (g_valgrind_threads[vg_tid].ignore_sync
          && !g_valgrind_threads[vg_tid].in_signal_handler) break;
      Put(args[2] ? WRITER_LOCK : READER_LOCK, ts_tid, pc, /*lock=*/args[1], 0);
      break;
    case TSREQ_PTHREAD_RWLOCK_UNLOCK_PRE:
      if (g_valgrind_threads[vg_tid].ignore_sync
          && !g_valgrind_threads[vg_tid].in_signal_handler) break;
      Put(UNLOCK, ts_tid, pc, /*lock=*/args[1], 0);
      break;
    case TSREQ_PTHREAD_SPIN_LOCK_INIT_OR_UNLOCK:
      Put(UNLOCK_OR_INIT, ts_tid, pc, /*lock=*/args[1], 0);
      break;
    case TSREQ_PTHREAD_RWLOCK_UNLOCK_POST:
      break;

    case TSREQ_POSIX_SEM_INIT_POST:
    case TSREQ_POSIX_SEM_DESTROY_PRE:
      break;
    case TSREQ_POSIX_SEM_POST_PRE:
      Put(SIGNAL, ts_tid, pc, /*sem=*/args[1], 0);
      break;
    case TSREQ_POSIX_SEM_WAIT_POST:
      Put(WAIT_BEFORE, ts_tid, pc, /*sem=*/args[1], 0);
      Put(WAIT_AFTER, ts_tid, pc, 0, 0);
      break;
    case TSREQ_CYCLIC_BARRIER_INIT:
      Put(CYCLIC_BARRIER_INIT, ts_tid, pc, args[1], args[2]);
      break;
    case TSREQ_CYCLIC_BARRIER_WAIT_BEFORE:
      Put(CYCLIC_BARRIER_WAIT_BEFORE, ts_tid, pc, args[1], 0);
      break;
    case TSREQ_CYCLIC_BARRIER_WAIT_AFTER:
      Put(CYCLIC_BARRIER_WAIT_AFTER, ts_tid, pc, args[1], 0);
      break;
    case TSREQ_GET_MY_SEGMENT:
      break;
    case TSREQ_GET_THREAD_ID:
      *ret = ts_tid;
      break;
    case TSREQ_GET_VG_THREAD_ID:
      *ret = vg_tid;
      break;
    case TSREQ_GET_SEGMENT_ID:
      break;
    case TSREQ_THREAD_SANITIZER_QUERY:
      *ret = (UWord)ThreadSanitizerQuery((const char *)args[1]);
      break;
    case TSREQ_FLUSH_STATE:
      Put(FLUSH_STATE, ts_tid, pc, 0, 0);
      break;
    default: CHECK(0);
  }
  return True;
}

static void SignalIn(ThreadId vg_tid, Int sigNo, Bool alt_stack) {
  g_valgrind_threads[vg_tid].in_signal_handler++;
  DCHECK(g_valgrind_threads[vg_tid].in_signal_handler == 1);
//  int32_t ts_tid = VgTidToTsTid(vg_tid);
//  Printf("T%d %s\n", ts_tid, __FUNCTION__);
}

static void SignalOut(ThreadId vg_tid, Int sigNo) {
  g_valgrind_threads[vg_tid].in_signal_handler--;
  CHECK(g_valgrind_threads[vg_tid].in_signal_handler >= 0);
  DCHECK(g_valgrind_threads[vg_tid].in_signal_handler == 0);
//  int32_t ts_tid = VgTidToTsTid(vg_tid);
//  Printf("T%d %s\n", ts_tid, __FUNCTION__);
}

// ---------------- On Trace entry ------------------ {{{2
VG_REGPARM(1) static void evh__on_trace_entry(uint32_t trace_no) {
  ThreadId vg_tid = GetVgTid();
  uintptr_t pc = GetVgPc(vg_tid);
  ValgrindThread *thr = &g_valgrind_threads[vg_tid];

  if (thr->ignore_accesses_in_current_trace) {
    CHECK(thr->ignore_accesses > 0);
    thr->ignore_accesses--;
    thr->ignore_accesses_in_current_trace = false;
  }

  if (thr->ignore_accesses) return;

  if (global_ignore ||
      LiteRaceSkipTrace(vg_tid, trace_no, G_flags->literace_sampling)) {
    thr->ignore_accesses_in_current_trace = true;
    thr->ignore_accesses++;
  }

  ThreadSanitizerEnterSblock(VgTidToTsTid(vg_tid), pc);
}

// ---------------------------- Instrumentation ---------------------------{{{1
static IRTemp gen_Get_SP ( IRSB*           bbOut,
                           VexGuestLayout* layout,
                           Int             hWordTy_szB )
{
  IRExpr* sp_expr;
  IRTemp  sp_temp;
  IRType  sp_type;
  /* This in effect forces the host and guest word sizes to be the
     same. */
  tl_assert(hWordTy_szB == layout->sizeof_SP);
  sp_type = layout->sizeof_SP == 8 ? Ity_I64 : Ity_I32;
  sp_expr = IRExpr_Get( layout->offset_SP, sp_type );
  sp_temp = newIRTemp( bbOut->tyenv, sp_type );
  addStmtToIRSB( bbOut, IRStmt_WrTmp( sp_temp, sp_expr ) );
  return sp_temp;
}


static void ts_instrument_trace_entry(IRSB *bbOut) {
   HChar*   hName    = (HChar*)"evh__on_trace_entry";
   static uint32_t trace_no;
   trace_no++;
   IRExpr **args = mkIRExprVec_1(mkIRExpr_HWord(trace_no));
   IRDirty* di = unsafeIRDirty_0_N( 1,
                           hName,
                           VG_(fnptr_to_fnentry)((void*)evh__on_trace_entry),
                           args);
   addStmtToIRSB( bbOut, IRStmt_Dirty(di));
}



static void ts_instrument_final_jump (
                                /*MOD*/IRSB* sbOut,
                                IRExpr* next,
                                IRJumpKind jumpkind,
                                VexGuestLayout* layout,
                                IRType gWordTy, IRType hWordTy ) {

#ifndef VGP_arm_linux
  // On non-ARM systems we instrument only function calls.
  if (jumpkind != Ijk_Call) return;
#else
  if (jumpkind != Ijk_Call) {
    // On an ARM system a non-call jump may possibly exit a function.
    IRTemp sp_post_call_insn
        = gen_Get_SP( sbOut, layout, sizeofIRType(hWordTy) );
    IRExpr **args = mkIRExprVec_2(
        IRExpr_RdTmp(sp_post_call_insn),
        next
        );
    IRDirty* di = unsafeIRDirty_0_N(
        2/*regparms*/,
        (char*)"evh__delete_frame",
        VG_(fnptr_to_fnentry)((void*) &evh__delete_frame ),
        args );
    addStmtToIRSB( sbOut, IRStmt_Dirty(di) );
    return;  // do not fall through
  }
#endif
  {
    const char *fn_name = "evh__rtn_call_ignore_unknown";
    void *fn = (void*)&evh__rtn_call_ignore_unknown;
    // Instrument the call instruction to keep the shadow stack consistent.
    IRTemp sp_post_call_insn
        = gen_Get_SP( sbOut, layout, sizeofIRType(hWordTy) );
    IRExpr **args = mkIRExprVec_2(
        IRExpr_RdTmp(sp_post_call_insn),
        next
        );
    if (next->tag == Iex_Const) {
      IRConst *con = next->Iex.Const.con;
      uintptr_t target = 0;
      if (con->tag == Ico_U32 || con->tag == Ico_U64) {
        target = con->tag == Ico_U32 ? con->Ico.U32 : con->Ico.U64;
        bool ignore = ThreadSanitizerIgnoreAccessesBelowFunction(target);
        if (ignore) {
          fn_name = "evh__rtn_call_ignore_yes";
          fn = (void*)&evh__rtn_call_ignore_yes;
        } else {
          fn_name = "evh__rtn_call_ignore_no";
          fn = (void*)&evh__rtn_call_ignore_no;
        }
      }
    }
    IRDirty* di = unsafeIRDirty_0_N(
        2/*regparms*/,
        (char*)fn_name,
        VG_(fnptr_to_fnentry)(fn),
        args );
    addStmtToIRSB( sbOut, IRStmt_Dirty(di) );
  }
}


static void instrument_mem_access ( IRSB*   bbOut,
                                    IRExpr* addr,
                                    Int     szB,
                                    Bool    isStore,
                                    Int     hWordTy_szB )
{
   IRType   tyAddr   = Ity_INVALID;
   const HChar*   hName    = NULL;
   void*    hAddr    = NULL;
   Int      regparms = 0;
   IRExpr** argv     = NULL;
   IRDirty* di       = NULL;

   tl_assert(isIRAtom(addr));
   tl_assert(hWordTy_szB == 4 || hWordTy_szB == 8);

   tyAddr = typeOfIRExpr( bbOut->tyenv, addr );
   tl_assert(tyAddr == Ity_I32 || tyAddr == Ity_I64);

   /* So the effective address is in 'addr' now. */
   regparms = 1; // unless stated otherwise
   if (isStore) {
      switch (szB) {
         case 1:
            hName = "evh__mem_help_write_1";
            hAddr = (void*)&evh__mem_help_write_1;
            argv = mkIRExprVec_1( addr );
            break;
         case 2:
            hName = "evh__mem_help_write_2";
            hAddr = (void*)&evh__mem_help_write_2;
            argv = mkIRExprVec_1( addr );
            break;
         case 4:
            hName = "evh__mem_help_write_4";
            hAddr = (void*)&evh__mem_help_write_4;
            argv = mkIRExprVec_1( addr );
            break;
         case 8:
            hName = "evh__mem_help_write_8";
            hAddr = (void*)&evh__mem_help_write_8;
            argv = mkIRExprVec_1( addr );
            break;
         default:
            tl_assert(szB > 8 && szB <= 512); /* stay sane */
            regparms = 2;
            hName = "evh__mem_help_write_N";
            hAddr = (void*)&evh__mem_help_write_N;
            argv = mkIRExprVec_2( addr, mkIRExpr_HWord( szB ));
            break;
      }
   } else {
      switch (szB) {
         case 1:
            hName = "evh__mem_help_read_1";
            hAddr = (void*)&evh__mem_help_read_1;
            argv = mkIRExprVec_1( addr );
            break;
         case 2:
            hName = "evh__mem_help_read_2";
            hAddr = (void*)&evh__mem_help_read_2;
            argv = mkIRExprVec_1( addr );
            break;
         case 4:
            hName = "evh__mem_help_read_4";
            hAddr = (void*)&evh__mem_help_read_4;
            argv = mkIRExprVec_1( addr );
            break;
         case 8:
            hName = "evh__mem_help_read_8";
            hAddr = (void*)&evh__mem_help_read_8;
            argv = mkIRExprVec_1( addr );
            break;
         default:
            tl_assert(szB > 8 && szB <= 512); /* stay sane */
            regparms = 2;
            hName = "evh__mem_help_read_N";
            hAddr = (void*)&evh__mem_help_read_N;
            argv = mkIRExprVec_2( addr, mkIRExpr_HWord( szB ));
            break;
      }
   }

   /* Add the helper. */
   tl_assert(hName);
   tl_assert(hAddr);
   tl_assert(argv);
   di = unsafeIRDirty_0_N( regparms,
                           (HChar*)hName, VG_(fnptr_to_fnentry)( hAddr ),
                           argv );
   addStmtToIRSB( bbOut, IRStmt_Dirty(di) );
}

int instrument_statement ( IRStmt* st, IRSB* bbIn, IRSB* bbOut, IRType hWordTy,
                           bool do_instrument ) {
  int res = 0;
  switch (st->tag) {
    case Ist_NoOp:
    case Ist_AbiHint:
    case Ist_Put:
    case Ist_PutI:
    case Ist_IMark:
    case Ist_Exit:
      /* None of these can contain any memory references. */
      break;

    case Ist_MBE:
      //instrument_memory_bus_event( bbOut, st->Ist.MBE.event );
      switch (st->Ist.MBE.event) {
        case Imbe_Fence:
          break; /* not interesting */
        default:
          ppIRStmt(st);
          tl_assert(0);
      }
      break;

    case Ist_CAS:
      break;

    case Ist_Store:
      if (do_instrument) instrument_mem_access(
        bbOut,
        st->Ist.Store.addr,
        sizeofIRType(typeOfIRExpr(bbIn->tyenv, st->Ist.Store.data)),
        True/*isStore*/,
        sizeofIRType(hWordTy)
      );
      res++;
      break;

    case Ist_WrTmp: {
      IRExpr* data = st->Ist.WrTmp.data;
      if (data->tag == Iex_Load) {
        if (do_instrument) instrument_mem_access(
            bbOut,
            data->Iex.Load.addr,
            sizeofIRType(data->Iex.Load.ty),
            False/*!isStore*/,
            sizeofIRType(hWordTy)
            );
        res++;
      }
      break;
    }

    case Ist_LLSC: {
      /* Ignore store-conditionals, treat load-linked's as normal loads. */
      IRType dataTy;
      if (st->Ist.LLSC.storedata == NULL) {
        /* LL */
        dataTy = typeOfIRTemp(bbIn->tyenv, st->Ist.LLSC.result);
        if (do_instrument) instrument_mem_access(
          bbOut,
          st->Ist.LLSC.addr,
          sizeofIRType(dataTy),
          False/*isStore*/,
          sizeofIRType(hWordTy)
        );
        res++;
      } else {
        /* SC */
        /* ignore */
      }
      break;
    }

    case Ist_Dirty: {
      Int      dataSize;
      IRDirty* d = st->Ist.Dirty.details;
      if (d->mFx != Ifx_None) {
        /* This dirty helper accesses memory.  Collect the
           details. */
        tl_assert(d->mAddr != NULL);
        tl_assert(d->mSize != 0);
        dataSize = d->mSize;
        if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify) {
          if (do_instrument) instrument_mem_access(
            bbOut, d->mAddr, dataSize, False/*!isStore*/,
            sizeofIRType(hWordTy)
          );
          res++;
        }
        if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify) {
          if (do_instrument) instrument_mem_access(
            bbOut, d->mAddr, dataSize, True/*isStore*/,
            sizeofIRType(hWordTy)
          );
          res++;
        }
      } else {
        tl_assert(d->mAddr == NULL);
        tl_assert(d->mSize == 0);
      }
      break;
    }

    default:
      ppIRStmt(st);
      tl_assert(0);
  } /* switch (st->tag) */
  return res;
}


static IRSB* ts_instrument ( VgCallbackClosure* closure,
                             IRSB* bbIn,
                             VexGuestLayout* layout,
                             VexGuestExtents* vge,
                             IRType gWordTy, IRType hWordTy ) {
  if (G_flags->dry_run >= 2) return bbIn;
  Int   i;
  IRSB* bbOut;

  char objname[kBuffSize];
  if (VG_(get_objname)(closure->nraddr, (Char*)objname, kBuffSize)) {
    if (StringMatch("*/ld-2*", objname)) {
      // we want to completely ignore ld-so.
      return bbIn;
    }
  }

  bool instrument_memory =
      ThreadSanitizerWantToInstrumentSblock(closure->nraddr);
  bool create_segments =
      ThreadSanitizerWantToCreateSegmentsOnSblockEntry(closure->nraddr);

  if (gWordTy != hWordTy) {
    /* We don't currently support this case. */
    VG_(tool_panic)((Char*)"host/guest word size mismatch");
  }

  /* Set up BB */
  bbOut           = emptyIRSB();
  bbOut->tyenv    = deepCopyIRTypeEnv(bbIn->tyenv);
  bbOut->next     = deepCopyIRExpr(bbIn->next);
  bbOut->jumpkind = bbIn->jumpkind;

  // Copy verbatim any IR preamble preceding the first IMark
  i = 0;
  while (i < bbIn->stmts_used && bbIn->stmts[i]->tag != Ist_IMark) {
    addStmtToIRSB( bbOut, bbIn->stmts[i] );
    i++;
  }
  int first = i;
  int n_mops = 0;
  // count mops
  if (instrument_memory) {
    for (i = first; i < bbIn->stmts_used; i++) {
      IRStmt* st = bbIn->stmts[i];
      tl_assert(st);
      tl_assert(isFlatIRStmt(st));
      n_mops += instrument_statement(st, bbIn, bbOut, hWordTy, false);
    } /* iterate over bbIn->stmts */
  }
  int n_mops_done = 0;
  // instrument mops and copy the rest of BB to the new one.
  for (i = first; i < bbIn->stmts_used; i++) {
    IRStmt* st = bbIn->stmts[i];
    tl_assert(st);
    tl_assert(isFlatIRStmt(st));
    if (i == first && n_mops && G_flags->keep_history >= 1 && create_segments) {
      ts_instrument_trace_entry(bbOut);
    }
    if (instrument_memory) {
      n_mops_done += instrument_statement(st, bbIn, bbOut, hWordTy, true);
    }
    addStmtToIRSB( bbOut, st );
  } /* iterate over bbIn->stmts */
  CHECK(n_mops == n_mops_done);

  ts_instrument_final_jump(bbOut, bbIn->next, bbIn->jumpkind, layout, gWordTy, hWordTy);

  return bbOut;
}

extern "C"
void ts_pre_clo_init(void) {
  VG_(details_name)            ((Char*)"ThreadSanitizer");
  VG_(details_version)         ((Char*)NULL);
  VG_(details_description)     ((Char*)"a data race detector");
  VG_(details_copyright_author)(
      (Char*)"Copyright (C) 2008-2009, and GNU GPL'd, by Google Inc.");
  VG_(details_bug_reports_to)  ((Char*)"data-race-test@googlegroups.com");

  VG_(basic_tool_funcs)        (ts_post_clo_init,
                                ts_instrument,
                                ts_fini);


  VG_(needs_client_requests)     (ts_handle_client_request);

  VG_(needs_command_line_options)(ts_process_cmd_line_option,
                                  ts_print_usage,
                                  ts_print_debug_usage);

//   VG_(needs_var_info)(); // optional

/*
   VG_(needs_core_errors)         ();


   // FIXME?
   //VG_(needs_sanity_checks)       (hg_cheap_sanity_check,
   //                                hg_expensive_sanity_check);
   // FIXME: surely this isn't thread-aware
   VG_(track_copy_mem_remap)      ( shadow_mem_copy_range );

   VG_(track_change_mem_mprotect) ( evh__set_perms );

   */

//   VG_(track_new_mem_startup)     ( evh__new_mem_w_perms );
//   VG_(track_new_mem_stack_signal)( evh__new_mem_w_tid );
//   VG_(track_new_mem_brk)         ( evh__new_mem_w_tid );
//   VG_(track_new_mem_mmap)        ( evh__new_mem_w_perms );

//   VG_(track_new_mem_stack)       ( evh__new_mem_stack);
//   VG_(track_new_mem_stack_8)       ( evh__new_mem_stack_8);
//   VG_(track_new_mem_stack_16)       ( evh__new_mem_stack_16);
//   VG_(track_new_mem_stack_32)       ( evh__new_mem_stack_32);


   VG_(track_die_mem_stack)       ( evh__die_mem_stack );
   VG_(track_die_mem_stack_8)     ( evh__die_mem_stack_8 );
   VG_(track_die_mem_stack_16)     ( evh__die_mem_stack_16 );
   VG_(track_die_mem_stack_32)     ( evh__die_mem_stack_32 );

   VG_(track_die_mem_stack_signal)( evh__die_mem );
   VG_(track_die_mem_brk)         ( evh__die_mem );
   VG_(track_die_mem_munmap)      ( evh__die_mem );

   /*
   // FIXME: what is this for?
   VG_(track_ban_mem_stack)       (NULL);

   VG_(track_pre_mem_read)        ( evh__pre_mem_read );
   VG_(track_pre_mem_read_asciiz) ( evh__pre_mem_read_asciiz );
   VG_(track_pre_mem_write)       ( evh__pre_mem_write );
   VG_(track_post_mem_write)      (NULL);

   /////////////////

   */
   VG_(track_pre_thread_ll_create)( evh__pre_thread_ll_create );
   VG_(track_workq_task_start)( evh__pre_workq_task_start );
   VG_(track_pre_thread_first_insn)( evh__pre_thread_first_insn );
   VG_(track_pre_thread_ll_exit)  ( evh__pre_thread_ll_exit );

//
//   VG_(track_start_client_code)( evh__start_client_code );
//   VG_(track_stop_client_code)( evh__stop_client_code );

   VG_(clo_vex_control).iropt_unroll_thresh = 0;
   VG_(clo_vex_control).guest_chase_thresh = 0;

   VG_(track_pre_deliver_signal) (&SignalIn);
   VG_(track_post_deliver_signal)(&SignalOut);
}


VG_DETERMINE_INTERFACE_VERSION(ts_pre_clo_init)

// -------- thread_sanitizer.cc -------------------------- {{{1
// ... for performance reasons...
#ifdef INCLUDE_THREAD_SANITIZER_CC
# undef INCLUDE_THREAD_SANITIZER_CC
# include "thread_sanitizer.cc"
#endif

// {{{1 end
// vim:shiftwidth=2:softtabstop=2:expandtab
