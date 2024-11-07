/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "core.h"
#include "nccl_net.h"
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include "param.h"

int ncclDebugLevel = -1;
static std::string rank = "?";
static std::string nranks = "?";
static int pid = -1;
static char hostname[1024];
thread_local int ncclDebugNoWarn = 0;
char ncclLastError[1024] = ""; // Global string for the last error in human readable form
uint64_t ncclDebugMask = NCCL_INIT|NCCL_ENV; // Default debug sub-system mask is INIT and ENV
FILE *ncclDebugFile = stdout;
pthread_mutex_t ncclDebugLock = PTHREAD_MUTEX_INITIALIZER;
std::chrono::steady_clock::time_point ncclEpoch;

static __thread int tid = -1;

static int numDigits(int n) {
  int ndigits = 0;
  while (n > 0) {
    n /= 10;
    ndigits++;
  }
  return ndigits; // Note this returns 0 for negative numbers, and that's ok for our purposes
}

void ncclDebugSetDistributorParams(int _rank, int _nranks) {
  pthread_mutex_lock(&ncclDebugLock);

  // dtrain logging zero-pads the rank depending on nranks (e.g. if nranks = 128, then for rank 8,
  // we'll format it as 008/128, hence the need to calculate numDigits.
  std::size_t ndigits = numDigits(_nranks);

  std::ostringstream rank_oss;
  rank_oss << std::setfill('0') << std::setw(ndigits) << _rank;
  rank = rank_oss.str();

  std::ostringstream nranks_oss;
  nranks_oss << std::setfill('0') << std::setw(ndigits) << _nranks;
  nranks = nranks_oss.str();

  pthread_mutex_unlock(&ncclDebugLock);
}

void ncclDebugInit() {
  pthread_mutex_lock(&ncclDebugLock);
  if (ncclDebugLevel != -1) { pthread_mutex_unlock(&ncclDebugLock); return; }
  const char* nccl_debug = ncclGetEnv("NCCL_DEBUG");
  int tempNcclDebugLevel = -1;
  if (nccl_debug == NULL) {
    tempNcclDebugLevel = NCCL_LOG_NONE;
  } else if (strcasecmp(nccl_debug, "VERSION") == 0) {
    tempNcclDebugLevel = NCCL_LOG_VERSION;
  } else if (strcasecmp(nccl_debug, "WARN") == 0) {
    tempNcclDebugLevel = NCCL_LOG_WARN;
  } else if (strcasecmp(nccl_debug, "INFO") == 0) {
    tempNcclDebugLevel = NCCL_LOG_INFO;
  } else if (strcasecmp(nccl_debug, "ABORT") == 0) {
    tempNcclDebugLevel = NCCL_LOG_ABORT;
  } else if (strcasecmp(nccl_debug, "TRACE") == 0) {
    tempNcclDebugLevel = NCCL_LOG_TRACE;
  }

  /* Parse the NCCL_DEBUG_SUBSYS env var
   * This can be a comma separated list such as INIT,COLL
   * or ^INIT,COLL etc
   */
  const char* ncclDebugSubsysEnv = ncclGetEnv("NCCL_DEBUG_SUBSYS");
  if (ncclDebugSubsysEnv != NULL) {
    int invert = 0;
    if (ncclDebugSubsysEnv[0] == '^') { invert = 1; ncclDebugSubsysEnv++; }
    ncclDebugMask = invert ? ~0ULL : 0ULL;
    char *ncclDebugSubsys = strdup(ncclDebugSubsysEnv);
    char *subsys = strtok(ncclDebugSubsys, ",");
    while (subsys != NULL) {
      uint64_t mask = 0;
      if (strcasecmp(subsys, "INIT") == 0) {
        mask = NCCL_INIT;
      } else if (strcasecmp(subsys, "COLL") == 0) {
        mask = NCCL_COLL;
      } else if (strcasecmp(subsys, "P2P") == 0) {
        mask = NCCL_P2P;
      } else if (strcasecmp(subsys, "SHM") == 0) {
        mask = NCCL_SHM;
      } else if (strcasecmp(subsys, "NET") == 0) {
        mask = NCCL_NET;
      } else if (strcasecmp(subsys, "GRAPH") == 0) {
        mask = NCCL_GRAPH;
      } else if (strcasecmp(subsys, "TUNING") == 0) {
        mask = NCCL_TUNING;
      } else if (strcasecmp(subsys, "ENV") == 0) {
        mask = NCCL_ENV;
      } else if (strcasecmp(subsys, "ALLOC") == 0) {
        mask = NCCL_ALLOC;
      } else if (strcasecmp(subsys, "CALL") == 0) {
        mask = NCCL_CALL;
      } else if (strcasecmp(subsys, "PROXY") == 0) {
        mask = NCCL_PROXY;
      } else if (strcasecmp(subsys, "NVLS") == 0) {
        mask = NCCL_NVLS;
      } else if (strcasecmp(subsys, "BOOTSTRAP") == 0) {
        mask = NCCL_BOOTSTRAP;
      } else if (strcasecmp(subsys, "REG") == 0) {
        mask = NCCL_REG;
      } else if (strcasecmp(subsys, "ALL") == 0) {
        mask = NCCL_ALL;
      }
      if (mask) {
        if (invert) ncclDebugMask &= ~mask; else ncclDebugMask |= mask;
      }
      subsys = strtok(NULL, ",");
    }
    free(ncclDebugSubsys);
  }

  // Cache pid and hostname
  getHostName(hostname, 1024, '.');
  pid = getpid();

  /* Parse and expand the NCCL_DEBUG_FILE path and
   * then create the debug file. But don't bother unless the
   * NCCL_DEBUG level is > VERSION
   */
  const char* ncclDebugFileEnv = ncclGetEnv("NCCL_DEBUG_FILE");
  if (tempNcclDebugLevel > NCCL_LOG_VERSION && ncclDebugFileEnv != NULL) {
    int c = 0;
    char debugFn[PATH_MAX+1] = "";
    char *dfn = debugFn;
    while (ncclDebugFileEnv[c] != '\0' && c < PATH_MAX) {
      if (ncclDebugFileEnv[c++] != '%') {
        *dfn++ = ncclDebugFileEnv[c-1];
        continue;
      }
      switch (ncclDebugFileEnv[c++]) {
        case '%': // Double %
          *dfn++ = '%';
          break;
        case 'h': // %h = hostname
          dfn += snprintf(dfn, PATH_MAX, "%s", hostname);
          break;
        case 'p': // %p = pid
          dfn += snprintf(dfn, PATH_MAX, "%d", pid);
          break;
        default: // Echo everything we don't understand
          *dfn++ = '%';
          *dfn++ = ncclDebugFileEnv[c-1];
          break;
      }
    }
    *dfn = '\0';
    if (debugFn[0] != '\0') {
      FILE *file = fopen(debugFn, "w");
      if (file != nullptr) {
        setbuf(file, nullptr); // disable buffering
        ncclDebugFile = file;
      }
    }
  }

  ncclEpoch = std::chrono::steady_clock::now();

  tzset();  // Need to call this once for get_local_now()
  __atomic_store_n(&ncclDebugLevel, tempNcclDebugLevel, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&ncclDebugLock);
}

NCCL_PARAM(WarnSetDebugInfo, "WARN_ENABLE_DEBUG_INFO", 0);

std::string get_local_now() {
  const auto now = std::chrono::system_clock::now();
  const time_t time_ = std::chrono::system_clock::to_time_t(now);
  const int64_t msec = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000LL;

  std::ostringstream _fmt;
  _fmt << "%F %T," << std::setfill('0') << std::setw(3) << msec;
  std::string fmt = _fmt.str();

  struct tm tmp;
  localtime_r(&time_, &tmp);

  std::ostringstream out;
  out << std::put_time(&tmp, fmt.c_str());
  return out.str();
}

/* Common logging function used by the INFO, WARN and TRACE macros
 * Also exported to the dynamically loadable Net transport modules so
 * they can share the debugging mechanisms and output files
 */
void ncclDebugLog(ncclDebugLogLevel level, unsigned long flags, const char *filefunc, int line, const char *fmt, ...) {
  if (__atomic_load_n(&ncclDebugLevel, __ATOMIC_ACQUIRE) == -1) ncclDebugInit();
  if (ncclDebugNoWarn != 0 && level == NCCL_LOG_WARN) { level = NCCL_LOG_INFO; flags = ncclDebugNoWarn; }

  // Save the last error (WARN) as a human readable string
  if (level == NCCL_LOG_WARN) {
    pthread_mutex_lock(&ncclDebugLock);
    va_list vargs;
    va_start(vargs, fmt);
    (void) vsnprintf(ncclLastError, sizeof(ncclLastError), fmt, vargs);
    va_end(vargs);
    pthread_mutex_unlock(&ncclDebugLock);
  }
  if (ncclDebugLevel < level || ((flags & ncclDebugMask) == 0)) return;

  if (tid == -1) {
    tid = syscall(SYS_gettid);
  }

  std::string local_now = get_local_now();
  char buffer[2048];
  size_t len = 0;
  if (level == NCCL_LOG_WARN) {
    len = snprintf(
        buffer, sizeof(buffer),
        "[%s/%s][%s] [%s:%d] [%s:pid=%d] NCCL WARN ",
        rank.c_str(), nranks.c_str(), local_now.c_str(), filefunc, line, hostname, pid
    );
    if (ncclParamWarnSetDebugInfo()) ncclDebugLevel = NCCL_LOG_INFO;
  } else if (level == NCCL_LOG_INFO) {
    len = snprintf(
        buffer, sizeof(buffer),
        "[%s/%s][%s] [%s:%d] [%s:pid=%d] NCCL INFO ",
        rank.c_str(), nranks.c_str(), local_now.c_str(), filefunc, line, hostname, pid
    );
  } else if (level == NCCL_LOG_TRACE && flags == NCCL_CALL) {
    len = snprintf(buffer, sizeof(buffer), "[%s:pid=%d] NCCL CALL ", hostname, pid);
  } else if (level == NCCL_LOG_TRACE) {
    auto delta = std::chrono::steady_clock::now() - ncclEpoch;
    double timestamp = std::chrono::duration_cast<std::chrono::duration<double>>(delta).count()*1000;
    len = snprintf(buffer, sizeof(buffer), "[%s:pid=%d] %f %s:%d NCCL TRACE ",
                   hostname, pid, timestamp, filefunc, line);
  }

  if (len) {
    va_list vargs;
    va_start(vargs, fmt);
    len += vsnprintf(buffer+len, sizeof(buffer)-len, fmt, vargs);
    va_end(vargs);
    // vsnprintf may return len > sizeof(buffer) in the case of a truncated output.
    // Rewind len so that we can replace the final \0 by \n
    if (len > sizeof(buffer)) len = sizeof(buffer)-1;
    buffer[len++] = '\n';
    fwrite(buffer, 1, len, ncclDebugFile);
  }
}

NCCL_PARAM(SetThreadName, "SET_THREAD_NAME", 0);

void ncclSetThreadName(pthread_t thread, const char *fmt, ...) {
  // pthread_setname_np is nonstandard GNU extension
  // needs the following feature test macro
#ifdef _GNU_SOURCE
  if (ncclParamSetThreadName() != 1) return;
  char threadName[NCCL_THREAD_NAMELEN];
  va_list vargs;
  va_start(vargs, fmt);
  vsnprintf(threadName, NCCL_THREAD_NAMELEN, fmt, vargs);
  va_end(vargs);
  pthread_setname_np(thread, threadName);
#endif
}
