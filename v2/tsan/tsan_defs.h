//===-- tsan_defs.h ---------------------------------------------*- C++ -*-===//
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

#ifndef TSAN_DEFS_H
#define TSAN_DEFS_H

#include "tsan_compiler.h"

#ifndef TSAN_DEBUG
#define TSAN_DEBUG 0
#endif  // TSAN_DEBUG

namespace __tsan {

typedef unsigned u32;  // NOLINT
typedef unsigned long long u64;  // NOLINT
typedef   signed long long s64;  // NOLINT
typedef unsigned long uptr;  // NOLINT

const uptr kPageSize = 4096;
const int kTidBits = 16;
const int kMaxTid = 1 << kTidBits;
const int kClkBits = 40;

#ifdef TSAN_SHADOW_COUNT
# if TSAN_SHADOW_COUNT == 2 \
  || TSAN_SHADOW_COUNT == 4 || TSAN_SHADOW_COUNT == 8
const unsigned kShadowCnt = TSAN_SHADOW_COUNT;
# else
#   error "TSAN_SHADOW_COUNT must be one of 2,4,8"
# endif
#else
const unsigned kShadowCnt = 8;
#endif

const unsigned kShadowCell = 8;

#if defined(TSAN_COLLECT_STATS) && TSAN_COLLECT_STATS
const bool kCollectStats = true;
#else
const bool kCollectStats = false;
#endif

#define CHECK_IMPL(c1, op, c2) \
  do { \
    __tsan::u64 v1 = (u64)(c1); \
    __tsan::u64 v2 = (u64)(c2); \
    if (!(v1 op v2)) \
      __tsan::CheckFailed(__FILE__, __LINE__, \
        "(" #c1 ") " #op " (" #c2 ")", v1, v2); \
  } while (false) \
/**/

#define CHECK(a)       CHECK_IMPL((a), !=, 0)
#define CHECK_EQ(a, b) CHECK_IMPL((a), ==, (b))
#define CHECK_NE(a, b) CHECK_IMPL((a), !=, (b))
#define CHECK_LT(a, b) CHECK_IMPL((a), <,  (b))
#define CHECK_LE(a, b) CHECK_IMPL((a), <=, (b))
#define CHECK_GT(a, b) CHECK_IMPL((a), >,  (b))
#define CHECK_GE(a, b) CHECK_IMPL((a), >=, (b))

#if TSAN_DEBUG
#define DCHECK(a)       CHECK(a)
#define DCHECK_EQ(a, b) CHECK_EQ(a, b)
#define DCHECK_NE(a, b) CHECK_NE(a, b)
#define DCHECK_LT(a, b) CHECK_LT(a, b)
#define DCHECK_LE(a, b) CHECK_LE(a, b)
#define DCHECK_GT(a, b) CHECK_GT(a, b)
#define DCHECK_GE(a, b) CHECK_GE(a, b)
#else
#define DCHECK(a)
#define DCHECK_EQ(a, b)
#define DCHECK_NE(a, b)
#define DCHECK_LT(a, b)
#define DCHECK_LE(a, b)
#define DCHECK_GT(a, b)
#define DCHECK_GE(a, b)
#endif

void CheckFailed(const char *file, int line, const char *cond, u64 v1, u64 v2);

template<typename T>
T min(T a, T b) {
  return a < b ? a : b;
}

template<typename T>
T max(T a, T b) {
  return a > b ? a : b;
}

void internal_memset(void *ptr, int c, uptr size);
void internal_memcpy(void *dst, const void *src, uptr size);
int internal_strcmp(const char *s1, const char *s2);
void internal_strcpy(char *s1, const char *s2);
uptr internal_strlen(const char *s);

struct MD5Hash {
  u64 hash[2];
  bool operator==(const MD5Hash &other) const {
    return hash[0] == other.hash[0] && hash[1] == other.hash[1];
  }
};

MD5Hash md5_hash(const void *data, uptr size);

enum StatType {
  StatMop,
  StatMopRead,
  StatMopWrite,
  StatMop1,  // These must be consequtive.
  StatMop2,
  StatMop4,
  StatMop8,
  StatMopSame,
  StatMopRange,
  StatShadowProcessed,
  StatShadowZero,
  StatShadowNonZero,  // Derived.
  StatShadowSameSize,
  StatShadowIntersect,
  StatShadowNotIntersect,
  StatShadowSameThread,
  StatShadowAnotherThread,
  StatShadowReplace,
  StatFuncEnter,
  StatFuncExit,
  StatEvents,
  StatMtxTotal,
  StatMtxTrace,
  StatMtxThreads,
  StatMtxReport,
  StatMtxSyncVar,
  StatMtxSyncTab,
  StatMtxSlab,
  StatMtxAnnotations,
  StatMtxAtExit,
  StatCnt,
};

struct ThreadState;
struct ThreadContext;
struct Context;
struct ReportDesc;
struct ReportStack;
class RegionAlloc;
class StackTrace;

}  // namespace __tsan

#endif  // TSAN_DEFS_H