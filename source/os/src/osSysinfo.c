/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "cus_name.h"

#define PROCESS_ITEM 12
#define UUIDLEN37 37

typedef struct {
  uint64_t user;
  uint64_t nice;
  uint64_t system;
  uint64_t idle;
  uint64_t wa;
  uint64_t hi;
  uint64_t si;
  uint64_t st;
  uint64_t guest;
  uint64_t guest_nice;
} SysCpuInfo;

typedef struct {
  uint64_t utime;   // user time
  uint64_t stime;   // kernel time
  uint64_t cutime;  // all user time
  uint64_t cstime;  // all dead time
} ProcCpuInfo;

#ifdef WINDOWS

/*
 * windows implementation
 */

#if (_WIN64)
#include <iphlpapi.h>
#include <mswsock.h>
#include <psapi.h>
#include <stdio.h>
#include <windows.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Mswsock.lib ")
#endif

#include <objbase.h>

#pragma warning(push)
#pragma warning(disable : 4091)
#include <DbgHelp.h>
#pragma warning(pop)

LONG WINAPI FlCrashDump(PEXCEPTION_POINTERS ep) {
  typedef BOOL(WINAPI * FxMiniDumpWriteDump)(IN HANDLE hProcess, IN DWORD ProcessId, IN HANDLE hFile,
                                             IN MINIDUMP_TYPE                           DumpType,
                                             IN CONST PMINIDUMP_EXCEPTION_INFORMATION   ExceptionParam,
                                             IN CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                             IN CONST PMINIDUMP_CALLBACK_INFORMATION    CallbackParam);

  HMODULE dll = LoadLibrary("dbghelp.dll");
  if (dll == NULL) return EXCEPTION_CONTINUE_SEARCH;
  FxMiniDumpWriteDump mdwd = (FxMiniDumpWriteDump)(GetProcAddress(dll, "MiniDumpWriteDump"));
  if (mdwd == NULL) {
    FreeLibrary(dll);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  TCHAR path[MAX_PATH];
  DWORD len = GetModuleFileName(NULL, path, _countof(path));
  path[len - 3] = 'd';
  path[len - 2] = 'm';
  path[len - 1] = 'p';

  HANDLE file = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    FreeLibrary(dll);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = ep;
  mei.ClientPointers = FALSE;

  (*mdwd)(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithHandleData, &mei, NULL, NULL);

  CloseHandle(file);
  FreeLibrary(dll);

  return EXCEPTION_CONTINUE_SEARCH;
}
LONG WINAPI exceptionHandler(LPEXCEPTION_POINTERS exception);

#elif defined(_TD_DARWIN_64)

#include <errno.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>
#include <CoreFoundation/CFString.h>
#include <stdio.h>

#else

#include <argp.h>
#ifndef TD_ASTRA
#include <linux/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#endif
#include <sys/utsname.h>
#include <unistd.h>

static pid_t tsProcId;
static char  tsSysNetFile[] = "/proc/net/dev";
static char  tsSysCpuFile[] = "/proc/stat";
static char  tsCpuPeriodFile[] = "/sys/fs/cgroup/cpu/cpu.cfs_period_us";
static char  tsCpuQuotaFile[] = "/sys/fs/cgroup/cpu/cpu.cfs_quota_us";
static char  tsProcCpuFile[25] = {0};
static char  tsProcMemFile[25] = {0};
static char  tsProcIOFile[25] = {0};

static void taosGetProcIOnfos() {
  tsPageSizeKB = sysconf(_SC_PAGESIZE) / 1024;
  tsOpenMax = sysconf(_SC_OPEN_MAX);
  tsStreamMax = TMAX(sysconf(_SC_STREAM_MAX), 0);
#ifndef TD_ASTRA
  tsProcId = (pid_t)syscall(SYS_gettid);

  (void)snprintf(tsProcMemFile, sizeof(tsProcMemFile), "/proc/%d/status", tsProcId);
  (void)snprintf(tsProcCpuFile, sizeof(tsProcCpuFile), "/proc/%d/stat", tsProcId);
  (void)snprintf(tsProcIOFile, sizeof(tsProcIOFile), "/proc/%d/io", tsProcId);
#endif
}
#endif

static int32_t taosGetSysCpuInfo(SysCpuInfo *cpuInfo) {
  int32_t code = 0;
#ifdef WINDOWS
  FILETIME pre_idleTime = {0};
  FILETIME pre_kernelTime = {0};
  FILETIME pre_userTime = {0};
  FILETIME idleTime;
  FILETIME kernelTime;
  FILETIME userTime;
  bool     res = GetSystemTimes(&idleTime, &kernelTime, &userTime);
  if (res) {
    cpuInfo->idle = CompareFileTime(&pre_idleTime, &idleTime);
    cpuInfo->system = CompareFileTime(&pre_kernelTime, &kernelTime);
    cpuInfo->user = CompareFileTime(&pre_userTime, &userTime);
    cpuInfo->nice = 0;
  }
#elif defined(DARWIN) || defined(TD_ASTRA)
  cpuInfo->idle = 0;
  cpuInfo->system = 0;
  cpuInfo->user = 0;
  cpuInfo->nice = 0;
#else
  TdFilePtr pFile = taosOpenFile(tsSysCpuFile, TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    return terrno;
  }

  char    line[1024];
  ssize_t bytes = taosGetsFile(pFile, sizeof(line), line);
  if (bytes < 0) {
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    return terrno;
  }

  char cpu[10] = {0};
  code = sscanf(line,
         "%s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
         " %" PRIu64,
         cpu, &cpuInfo->user, &cpuInfo->nice, &cpuInfo->system, &cpuInfo->idle, &cpuInfo->wa, &cpuInfo->hi,
         &cpuInfo->si, &cpuInfo->st, &cpuInfo->guest, &cpuInfo->guest_nice);
  if (EOF == code) {
    terrno = TAOS_SYSTEM_ERROR(ERRNO);
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    return terrno;
  }
  
  TAOS_SKIP_ERROR(taosCloseFile(&pFile));
#endif

  return 0;
}

static int32_t taosGetProcCpuInfo(ProcCpuInfo *cpuInfo) {
  int32_t code = 0;

#ifdef WINDOWS
  FILETIME pre_krnlTm = {0};
  FILETIME pre_usrTm = {0};
  FILETIME creatTm, exitTm, krnlTm, usrTm;

  if (GetThreadTimes(GetCurrentThread(), &creatTm, &exitTm, &krnlTm, &usrTm)) {
    cpuInfo->stime = CompareFileTime(&pre_krnlTm, &krnlTm);
    cpuInfo->utime = CompareFileTime(&pre_usrTm, &usrTm);
    cpuInfo->cutime = 0;
    cpuInfo->cstime = 0;
  }
#elif defined(DARWIN) || defined(TD_ASTRA)
  cpuInfo->stime = 0;
  cpuInfo->utime = 0;
  cpuInfo->cutime = 0;
  cpuInfo->cstime = 0;
#else
  TdFilePtr pFile = taosOpenFile(tsProcCpuFile, TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    return terrno;
  }

  char    line[1024] = {0};
  ssize_t bytes = taosGetsFile(pFile, sizeof(line), line);
  if (bytes < 0) {
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    return code;
  }

  for (int i = 0, blank = 0; line[i] != 0; ++i) {
    if (line[i] == ' ') blank++;
    if (blank == PROCESS_ITEM) {
      code = sscanf(line + i + 1, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64, &cpuInfo->utime, &cpuInfo->stime,
             &cpuInfo->cutime, &cpuInfo->cstime);
      if (EOF == code) {
        terrno = TAOS_SYSTEM_ERROR(ERRNO);
        return terrno;
      }
             
      break;
    }
  }

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));
#endif

  return 0;
}

bool taosCheckSystemIsLittleEnd() {
  union check {
    int16_t i;
    char    ch[2];
  } c;
  c.i = 1;
  return c.ch[0] == 1;
}

void taosGetSystemInfo() {
#ifdef WINDOWS
  TAOS_SKIP_ERROR(taosGetCpuCores(&tsNumOfCores, false));
  TAOS_SKIP_ERROR(taosGetTotalMemory(&tsTotalMemoryKB));
  TAOS_SKIP_ERROR(taosGetCpuUsage(NULL, NULL));
#elif defined(_TD_DARWIN_64)
  long physical_pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGESIZE);
  tsTotalMemoryKB = physical_pages * page_size / 1024;
  tsPageSizeKB = page_size / 1024;
  tsNumOfCores = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(TD_ASTRA)
  taosGetProcIOnfos();
  TAOS_SKIP_ERROR(taosGetCpuCores(&tsNumOfCores, false));
  TAOS_SKIP_ERROR(taosGetTotalMemory(&tsTotalMemoryKB));
  TAOS_SKIP_ERROR(taosGetCpuUsage(NULL, NULL));
#else
  taosGetProcIOnfos();
  TAOS_SKIP_ERROR(taosGetCpuCores(&tsNumOfCores, false)); 
  TAOS_SKIP_ERROR(taosGetTotalMemory(&tsTotalMemoryKB));
  TAOS_SKIP_ERROR(taosGetCpuUsage(NULL, NULL));
  TAOS_SKIP_ERROR(taosGetCpuInstructions(&tsSSE42Supported, &tsAVXSupported, &tsAVX2Supported, &tsFMASupported, &tsAVX512Supported));
#endif
}

int32_t taosGetEmail(char *email, int32_t maxLen) {
  OS_PARAM_CHECK(email);
#ifdef WINDOWS
  return 0;
#elif defined(_TD_DARWIN_64)
#ifdef CUS_PROMPT
  const char *filepath = "/usr/local/"CUS_PROMPT"/email";
#else
  const char *filepath = "/usr/local/taos/email";
#endif  // CUS_PROMPT

  TdFilePtr pFile = taosOpenFile(filepath, TD_FILE_READ);
  if (pFile == NULL) return false;

  if (taosReadFile(pFile, (void *)email, maxLen) < 0) {
    taosCloseFile(&pFile);
    return terrno;
  }

  taosCloseFile(&pFile);
  return 0;
#else
#ifdef CUS_PROMPT
  const char *filepath = "/usr/local/"CUS_PROMPT"/email";
#else
  const char *filepath = "/usr/local/taos/email";
#endif  // CUS_PROMPT

  TdFilePtr pFile = taosOpenFile(filepath, TD_FILE_READ);
  if (pFile == NULL) return terrno;

  if (taosReadFile(pFile, (void *)email, maxLen) < 0) {
    int32_t code = terrno;
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    return code;
  }

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));
  
  return 0;
#endif
}

#ifdef WINDOWS
bool getWinVersionReleaseName(char *releaseName, int32_t maxLen) {
  if(releaseName == NULL) return false;
  TCHAR          szFileName[MAX_PATH];
  DWORD             dwHandle;
  DWORD             dwLen;
  LPVOID            lpData;
  UINT              uLen;
  VS_FIXEDFILEINFO *pFileInfo;

  int ret = GetWindowsDirectory(szFileName, MAX_PATH);
  if (ret == 0) {
    return false;
  }
  wsprintf(szFileName, L"%s%s", szFileName, L"\\explorer.exe");
  dwLen = GetFileVersionInfoSize(szFileName, &dwHandle);
  if (dwLen == 0) {
    return false;
  }

  lpData = malloc(dwLen);
  if (lpData == NULL) return false;
  if (!GetFileVersionInfo(szFileName, dwHandle, dwLen, lpData)) {
    free(lpData);
    return false;
  }

  if (!VerQueryValue(lpData, L"\\", (LPVOID *)&pFileInfo, &uLen)) {
    free(lpData);
    return false;
  }

  snprintf(releaseName, maxLen, "Windows %d.%d", HIWORD(pFileInfo->dwProductVersionMS),
           LOWORD(pFileInfo->dwProductVersionMS));
  free(lpData);
  return true;
}
#endif

int32_t taosGetOsReleaseName(char *releaseName, char* sName, char* ver, int32_t maxLen) {
  OS_PARAM_CHECK(releaseName);
#ifdef WINDOWS
  if (!getWinVersionReleaseName(releaseName, maxLen)) {
    snprintf(releaseName, maxLen, "Windows");
  }
  if(sName) snprintf(sName, maxLen, "Windows");
  return 0;
#elif defined(_TD_DARWIN_64)
  char osversion[32];
  size_t osversion_len = sizeof(osversion) - 1;
  int osversion_name[] = { CTL_KERN, KERN_OSRELEASE };

  if(sName) snprintf(sName, maxLen, "macOS");
  if (sysctl(osversion_name, 2, osversion, &osversion_len, NULL, 0) == -1) {
    return TAOS_SYSTEM_ERROR(ERRNO);
  }

  uint32_t major, minor;
  if (sscanf(osversion, "%u.%u", &major, &minor) == EOF) {
      return TAOS_SYSTEM_ERROR(ERRNO);
  }
  if (major >= 20) {
      major -= 9; // macOS 11 and newer
      snprintf(releaseName, maxLen, "macOS %u.%u", major, minor);
  } else {
      major -= 4; // macOS 10.1.1 and newer
      snprintf(releaseName, maxLen, "macOS 10.%d.%d", major, minor);
  }

  return 0;
#elif defined(TD_ASTRA) // TD_ASTRA_TODO
  if(sName) snprintf(sName, maxLen, "Astra");
  snprintf(releaseName, maxLen, "Astra");
  return 0;
#else
  char    line[1024];
  char   *dest = NULL;
  size_t  size = 0;
  int32_t code = 0;
  int32_t cnt = 0;

  TdFilePtr pFile = taosOpenFile("/etc/os-release", TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    return terrno;
  }

  while ((size = taosGetsFile(pFile, sizeof(line), line)) > 0) {
    line[size - 1] = '\0';
    if (strncmp(line, "NAME", 4) == 0) {
      dest = sName;
    } else if (strncmp(line, "PRETTY_NAME", 11) == 0) {
      dest = releaseName;
      code = 0;
    } else if (strncmp(line, "VERSION_ID", 10) == 0) {
      dest = ver;
    } else {
      continue;
    }
    if (!dest) continue;
    const char *p = strchr(line, '=') + 1;
    if (*p == '"') {
      p++;
      line[size - 2] = 0;
    }
    tstrncpy(dest, p, maxLen);

    if (++cnt >= 3) break;
  }

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));
  return code;
#endif
}

int32_t taosGetCpuInfo(char *cpuModel, int32_t maxLen, float *numOfCores) {
  OS_PARAM_CHECK(cpuModel);
  OS_PARAM_CHECK(numOfCores);
#ifdef WINDOWS
  char  value[100];
  DWORD bufferSize = sizeof(value);
  LSTATUS ret = RegGetValue(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
              RRF_RT_ANY, NULL, (PVOID)&value, &bufferSize);
  if (ret != ERROR_SUCCESS) {
    return TAOS_SYSTEM_ERROR(ret);
  }
  tstrncpy(cpuModel, value, maxLen);
  SYSTEM_INFO si;
  memset(&si, 0, sizeof(SYSTEM_INFO));
  GetSystemInfo(&si);
  *numOfCores = si.dwNumberOfProcessors;
  return 0;
#elif defined(_TD_DARWIN_64)
  char    buf[16];
  int32_t done = 0;
  int32_t code = -1;

  TdCmdPtr pCmd = taosOpenCmd("sysctl -n machdep.cpu.brand_string");
  if (pCmd == NULL) return code;
  if (taosGetsCmd(pCmd, maxLen, cpuModel) > 0) {
    code = 0;
    done |= 1;
  }
  int endPos = strlen(cpuModel)-1;
  if (cpuModel[endPos] == '\n') {
    cpuModel[endPos] = '\0';
  }
  taosCloseCmd(&pCmd);

  pCmd = taosOpenCmd("sysctl -n machdep.cpu.core_count");
  if (pCmd == NULL) return code;
  memset(buf, 0, sizeof(buf));
  if (taosGetsCmd(pCmd, sizeof(buf) - 1, buf) > 0) {
    code = 0;
    done |= 2;
    *numOfCores = taosStr2Float(buf, NULL);
  }
  taosCloseCmd(&pCmd);

  return code;
#elif defined(TD_ASTRA) // TD_ASTRA_TODO
  tstrncpy(cpuModel, "ft_2000_4", maxLen);
  TAOS_SKIP_ERROR(taosGetCpuCores(numOfCores, false));
  return 0;
#else
  char    line[1024] = {0};
  size_t  size = 0;
  int32_t done = 0;
  int32_t code = 0;
  float   coreCount = 0;

  TdFilePtr pFile = taosOpenFile("/proc/cpuinfo", TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) return terrno;

  while (done != 3 && (size = taosGetsFile(pFile, sizeof(line), line)) > 0) {
    line[size - 1] = '\0';
    if (((done & 1) == 0) && strncmp(line, "model name", 10) == 0) {
      const char *v = strchr(line, ':') + 2;
      tstrncpy(cpuModel, v, maxLen);
      code = 0;
      done |= 1;
    } else if (((done & 2) == 0) && strncmp(line, "cpu cores", 9) == 0) {
      const char *v = strchr(line, ':') + 2;
      *numOfCores = taosStr2Float(v, NULL);
      done |= 2;
    }
    if (strncmp(line, "processor", 9) == 0) coreCount += 1;
  }

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));

  if (code != 0 && (done & 1) == 0) {
    TdFilePtr pFile1 = taosOpenFile("/proc/device-tree/model", TD_FILE_READ | TD_FILE_STREAM);
    if (pFile1 != NULL) {
      ssize_t bytes = taosGetsFile(pFile1, maxLen, cpuModel);
      TAOS_SKIP_ERROR(taosCloseFile(&pFile));
      if (bytes > 0) {
        code = 0;
        done |= 1;
      }
    }
  }

  if (code != 0 && (done & 1) == 0) {
    TdCmdPtr pCmd = taosOpenCmd("uname -a");
    if (pCmd == NULL) {
      return terrno;
    }
    if (taosGetsCmd(pCmd, maxLen, cpuModel) > 0) {
      code = 0;
      done |= 1;
    }
    taosCloseCmd(&pCmd);
  }

  if ((done & 2) == 0) {
    *numOfCores = coreCount;
    done |= 2;
  }

  return code;
#endif
}

// Returns the container's CPU quota if successful, otherwise returns the physical CPU cores
static int32_t taosCntrGetCpuCores(float *numOfCores) {
#ifdef WINDOWS
  return TSDB_CODE_UNSUPPORT_OS;
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
  return TSDB_CODE_UNSUPPORT_OS;
#else
  TdFilePtr pFile = NULL;
  if (!(pFile = taosOpenFile(tsCpuQuotaFile, TD_FILE_READ | TD_FILE_STREAM))) {
    goto _sys;
  }
  char qline[32] = {0};
  if (taosGetsFile(pFile, sizeof(qline), qline) <= 0) {
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    goto _sys;
  }
  
  TAOS_SKIP_ERROR(taosCloseFile(&pFile));
  float quota = taosStr2Float(qline, NULL);
  if (quota < 0) {
    goto _sys;
  }

  if (!(pFile = taosOpenFile(tsCpuPeriodFile, TD_FILE_READ | TD_FILE_STREAM))) {
    goto _sys;
  }
  
  char pline[32] = {0};
  if (taosGetsFile(pFile, sizeof(pline), pline) <= 0) {
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    goto _sys;
  }
  
  TAOS_SKIP_ERROR(taosCloseFile(&pFile));

  float period = taosStr2Float(pline, NULL);
  float quotaCores = quota / period;
  float sysCores = sysconf(_SC_NPROCESSORS_ONLN);
  if (quotaCores < sysCores && quotaCores > 0) {
    *numOfCores = quotaCores;
  } else {
    *numOfCores = sysCores;
  }
  if(*numOfCores <= 0) {
    return TAOS_SYSTEM_ERROR(ERRNO);
  }
  goto _end;
  
_sys:
  *numOfCores = sysconf(_SC_NPROCESSORS_ONLN);
  if(*numOfCores <= 0) {
    return TAOS_SYSTEM_ERROR(ERRNO);
  }
  
_end:
  return 0;
  
#endif
}

int32_t taosGetCpuCores(float *numOfCores, bool physical) {
  OS_PARAM_CHECK(numOfCores);
#ifdef WINDOWS
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  *numOfCores = info.dwNumberOfProcessors;
  return  0;
#elif defined(_TD_DARWIN_64)
  *numOfCores = sysconf(_SC_NPROCESSORS_ONLN);
  if(*numOfCores <= 0) {
    return TAOS_SYSTEM_ERROR(ERRNO);
  }
  return 0;
#elif defined(TD_ASTRA) // TD_ASTRA_TODO
  *numOfCores = 4;
  return 0;
#else
  if (physical) {
    *numOfCores = sysconf(_SC_NPROCESSORS_ONLN);
    if(*numOfCores <= 0) {
      return TAOS_SYSTEM_ERROR(ERRNO);
    }
  } else {
    int code= taosCntrGetCpuCores(numOfCores);
    if(code != 0) {
      return code;
    }
  }
  return 0;
#endif
}

int32_t taosGetCpuUsage(double *cpu_system, double *cpu_engine) {
  static int64_t lastSysUsed = -1;
  static int64_t lastSysTotal = -1;
  static int64_t lastProcTotal = -1;
  static int64_t curSysUsed = 0;
  static int64_t curSysTotal = 0;
  static int64_t curProcTotal = 0;

  if (cpu_system != NULL) *cpu_system = 0;
  if (cpu_engine != NULL) *cpu_engine = 0;

  SysCpuInfo  sysCpu = {0};
  ProcCpuInfo procCpu = {0};
  if (taosGetSysCpuInfo(&sysCpu) == 0 && taosGetProcCpuInfo(&procCpu) == 0) {
    curSysUsed = sysCpu.user + sysCpu.nice + sysCpu.system + sysCpu.wa + sysCpu.hi + sysCpu.si + sysCpu.st +
                 sysCpu.guest + sysCpu.guest_nice;
    curSysTotal = curSysUsed + sysCpu.idle;
    curProcTotal = procCpu.utime + procCpu.stime + procCpu.cutime + procCpu.cstime;

    if(lastSysUsed >= 0 && lastSysTotal >=0 && lastProcTotal >=0){
      if (curSysTotal - lastSysTotal > 0 && curSysUsed >= lastSysUsed && curProcTotal >= lastProcTotal) {
        if (cpu_system != NULL) {
          *cpu_system = (curSysUsed - lastSysUsed) / (double)(curSysTotal - lastSysTotal) * 100;
        }
        if (cpu_engine != NULL) {
          *cpu_engine = (curProcTotal - lastProcTotal) / (double)(curSysTotal - lastSysTotal) * 100;
        }
      }
    }

    lastSysUsed = curSysUsed;
    lastSysTotal = curSysTotal;
    lastProcTotal = curProcTotal;
  }
  return 0;
}

#define __cpuid_fix(level, a, b, c, d) \
              __asm__("xor %%ecx, %%ecx\n" \
                      "cpuid\n" \
                      : "=a"(a), "=b"(b), "=c"(c), "=d"(d) \
                      : "0"(level))

// todo add for windows and mac
int32_t taosGetCpuInstructions(char* sse42, char* avx, char* avx2, char* fma, char* avx512) {
#ifdef WINDOWS
#elif defined(_TD_DARWIN_64)
#else

#ifdef _TD_X86_
  // Since the compiler is not support avx/avx2 instructions, the global variables always need to be
  // set to be false
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

  int32_t ret = __get_cpuid(1, &eax, &ebx, &ecx, &edx);
  if (ret == 0) {
    return -1;  // failed to get the cpuid info
  }

  *sse42 = (char) ((ecx & bit_SSE4_2) == bit_SSE4_2);
  *avx   = (char) ((ecx & bit_AVX) == bit_AVX);
  *fma   = (char) ((ecx & bit_FMA) == bit_FMA);

  // work around a bug in GCC.
  // Ref to https://gcc.gnu.org/bugzilla/show_bug.cgi?id=77756
  __cpuid_fix(7u, eax, ebx, ecx, edx);
  *avx2 = (char) ((ebx & bit_AVX2) == bit_AVX2);
  *avx512 = (char)((ebx & bit_AVX512F) == bit_AVX512F);
#endif   // _TD_X86_
#endif

  return 0;
}

int32_t taosGetTotalMemory(int64_t *totalKB) {
  OS_PARAM_CHECK(totalKB);
#ifdef WINDOWS
  MEMORYSTATUSEX memsStat;
  memsStat.dwLength = sizeof(memsStat);
  if (!GlobalMemoryStatusEx(&memsStat)) {
    return TAOS_SYSTEM_WINAPI_ERROR(GetLastError());
  }

  *totalKB = memsStat.ullTotalPhys / 1024;
  return 0;
#elif defined(_TD_DARWIN_64)
  return 0;
#elif defined(TD_ASTRA) // TD_ASTRA_TODO
  *totalKB = (int64_t)256 * 1024;
  return 0;
#else
  *totalKB = (int64_t)(sysconf(_SC_PHYS_PAGES) * tsPageSizeKB);
  if(*totalKB <= 0) {
    return TAOS_SYSTEM_ERROR(ERRNO);
  }
  return 0;
#endif
}

int32_t taosGetProcMemory(int64_t *usedKB) {
  OS_PARAM_CHECK(usedKB);
#ifdef WINDOWS
  unsigned bytes_used = 0;

#if defined(_WIN64) && defined(_MSC_VER)
  PROCESS_MEMORY_COUNTERS pmc;
  HANDLE                  cur_proc = GetCurrentProcess();

  if (GetProcessMemoryInfo(cur_proc, &pmc, sizeof(pmc))) {
    bytes_used = (unsigned)(pmc.WorkingSetSize + pmc.PagefileUsage);
  }
#endif

  *usedKB = bytes_used / 1024;
  return 0;
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
  *usedKB = 0;
  return 0;
#else
  TdFilePtr pFile = taosOpenFile(tsProcMemFile, TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    // printf("open file:%s failed", tsProcMemFile);
    return terrno;
  }

  ssize_t bytes = 0;
  char    line[1024] = {0};
  while (!taosEOFFile(pFile)) {
    bytes = taosGetsFile(pFile, sizeof(line), line);
    if (bytes <= 0) {
      break;
    }
    if (strstr(line, "VmRSS:") != NULL) {
      break;
    }
  }

  char tmp[10];
  (void)sscanf(line, "%s %" PRId64, tmp, usedKB);

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));
  
  return 0;
#endif
}

int32_t taosGetSysAvailMemory(int64_t *availSize) {
#ifdef WINDOWS
  MEMORYSTATUSEX memsStat;
  memsStat.dwLength = sizeof(memsStat);
  if (!GlobalMemoryStatusEx(&memsStat)) {
    return -1;
  }

  int64_t nMemFree = memsStat.ullAvailPhys;
  int64_t nMemTotal = memsStat.ullTotalPhys;

  *availSize = nMemTotal - nMemFree;
  return 0;
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
  *availSize = 0;
  return 0;
#else
  TdFilePtr pFile = taosOpenFile("/proc/meminfo", TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    return terrno;
  }

  ssize_t bytes = 0;
  char    line[128] = {0};
  int32_t expectedSize = 13; //"MemAvailable:"
  while (!taosEOFFile(pFile)) {
    bytes = taosGetsFile(pFile, sizeof(line), line);
    if (bytes < 0) {
      break;
    }
    if (line[0] != 'M' && line[3] != 'A') {
      line[0] = 0;
      continue;
    }
    if (0 == strncmp(line, "MemAvailable:", expectedSize)) {
      break;
    }
  }

  if (0 == line[0]) {
    return TSDB_CODE_UNSUPPORT_OS;
  }
  
  char tmp[32];
  (void)sscanf(line, "%s %" PRId64, tmp, availSize);

  *availSize *= 1024;
  
  (void)taosCloseFile(&pFile);
  return 0;
#endif
}

int32_t taosGetSysMemory(int64_t *usedKB) {
  OS_PARAM_CHECK(usedKB);
#ifdef WINDOWS
  MEMORYSTATUSEX memsStat;
  memsStat.dwLength = sizeof(memsStat);
  if (!GlobalMemoryStatusEx(&memsStat)) {
    return TAOS_SYSTEM_WINAPI_ERROR(GetLastError());
  }

  int64_t nMemFree = memsStat.ullAvailPhys / 1024;
  int64_t nMemTotal = memsStat.ullTotalPhys / 1024.0;

  *usedKB = nMemTotal - nMemFree;
  return 0;
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA) // TD_ASTRA_TODO
  *usedKB = 0;
  return 0;
#else
  *usedKB = sysconf(_SC_AVPHYS_PAGES) * tsPageSizeKB;
  if(*usedKB <= 0) {
    return TAOS_SYSTEM_ERROR(ERRNO);
  }
  return 0;
#endif
}

int32_t taosGetDiskSize(char *dataDir, SDiskSize *diskSize) {
  OS_PARAM_CHECK(dataDir);
  OS_PARAM_CHECK(diskSize);
#if defined(WINDOWS)
  unsigned _int64 i64FreeBytesToCaller;
  unsigned _int64 i64TotalBytes;
  unsigned _int64 i64FreeBytes;

  BOOL fResult = GetDiskFreeSpaceExA(dataDir, (PULARGE_INTEGER)&i64FreeBytesToCaller, (PULARGE_INTEGER)&i64TotalBytes,
                                     (PULARGE_INTEGER)&i64FreeBytes);
  if (fResult) {
    diskSize->total = (int64_t)(i64TotalBytes);
    diskSize->avail = (int64_t)(i64FreeBytesToCaller);
    diskSize->used = (int64_t)(i64TotalBytes - i64FreeBytes);
    return 0;
  } else {
    // printf("failed to get disk size, dataDir:%s errno:%s", tsDataDir, strerror(ERRNO));
    terrno = TAOS_SYSTEM_WINAPI_ERROR(GetLastError());
    return terrno;
  }
#elif defined(_TD_DARWIN_64)
  struct statvfs info;
  if (statvfs(dataDir, &info)) {
    // printf("failed to get disk size, dataDir:%s errno:%s", tsDataDir, strerror(ERRNO));
    terrno = TAOS_SYSTEM_ERROR(ERRNO);
    return terrno;
  } else {
    diskSize->total = info.f_blocks * info.f_frsize;
    diskSize->avail = info.f_bavail * info.f_frsize;
    diskSize->used = (info.f_blocks - info.f_bfree) * info.f_frsize;
    return 0;
  }
#elif defined(TD_ASTRA)  // TD_ASTRA_TODO
  //  if (-1 == ioctl(dataDir, FIOFSTATVFSGETBYNAME, &info)) { // TODO:try to check whether the API is available
  //     terrno = TAOS_SYSTEM_ERROR(ERRNO);
  //     return terrno;
  diskSize->total = 100LL * 1024 * 1024 * 1024;
  diskSize->avail = 50LL * 1024 * 1024 * 1024;
  diskSize->used = 50LL * 1024 * 1024 * 1024;
  //  } else {
  //    diskSize->total = info.f_blocks * info.f_frsize;
  //    diskSize->avail = info.f_bavail * info.f_frsize;
  //    diskSize->used = diskSize->total - diskSize->avail;
  //  }
  return 0;
#else
  struct statvfs info;
  if (-1 == statvfs(dataDir, &info)) {
    terrno = TAOS_SYSTEM_ERROR(ERRNO);
    return terrno;
  } else {
    diskSize->total = info.f_blocks * info.f_frsize;
    diskSize->avail = info.f_bavail * info.f_frsize;
    diskSize->used = diskSize->total - diskSize->avail;
    
    return 0;
  }
#endif
}

int32_t taosGetProcIO(int64_t *rchars, int64_t *wchars, int64_t *read_bytes, int64_t *write_bytes) {
  OS_PARAM_CHECK(rchars);
  OS_PARAM_CHECK(wchars);
  OS_PARAM_CHECK(read_bytes);
  OS_PARAM_CHECK(write_bytes);
#ifdef WINDOWS
  IO_COUNTERS io_counter;
  if (GetProcessIoCounters(GetCurrentProcess(), &io_counter)) {
    *rchars = io_counter.ReadTransferCount;
    *wchars = io_counter.WriteTransferCount;
    *read_bytes = 0;
    *write_bytes = 0;
    return 0;
  }
  return TAOS_SYSTEM_WINAPI_ERROR(GetLastError());
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
  *rchars = 0;
  *wchars = 0;
  *read_bytes = 0;
  *write_bytes = 0;
  return 0;
#else
  TdFilePtr pFile = taosOpenFile(tsProcIOFile, TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    return terrno;
  }

  ssize_t bytes = 0;
  char    line[1024] = {0};
  char    tmp[24];
  int     readIndex = 0;

  while (!taosEOFFile(pFile)) {
    bytes = taosGetsFile(pFile, sizeof(line), line);
    if (bytes < 10) {
      break;
    }
    if (strstr(line, "rchar:") != NULL) {
      (void)sscanf(line, "%s %" PRId64, tmp, rchars);
      readIndex++;
    } else if (strstr(line, "wchar:") != NULL) {
      (void)sscanf(line, "%s %" PRId64, tmp, wchars);
      readIndex++;
    } else if (strstr(line, "read_bytes:") != NULL) {  // read_bytes
      (void)sscanf(line, "%s %" PRId64, tmp, read_bytes);
      readIndex++;
    } else if (strstr(line, "write_bytes:") != NULL) {  // write_bytes
      (void)sscanf(line, "%s %" PRId64, tmp, write_bytes);
      readIndex++;
    } else {
    }

    if (readIndex >= 4) break;
  }

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));

  if (readIndex < 4) {
    return -1;
  }

  return 0;
#endif
}

int32_t taosGetProcIODelta(int64_t *rchars, int64_t *wchars, int64_t *read_bytes, int64_t *write_bytes) {
  if (rchars == NULL || wchars == NULL || read_bytes == NULL || write_bytes == NULL) {
    return TSDB_CODE_INVALID_PARA;
  }
  static int64_t last_rchars = -1;
  static int64_t last_wchars = -1;
  static int64_t last_read_bytes = -1;
  static int64_t last_write_bytes = -1;
  static int64_t cur_rchars = 0;
  static int64_t cur_wchars = 0;
  static int64_t cur_read_bytes = 0;
  static int64_t cur_write_bytes = 0;
  int32_t code = taosGetProcIO(&cur_rchars, &cur_wchars, &cur_read_bytes, &cur_write_bytes);
  if (code == 0) {
    if(last_rchars >=0 && last_wchars >=0 && last_read_bytes >=0 && last_write_bytes >= 0){
      *rchars = cur_rchars - last_rchars;
      *wchars = cur_wchars - last_wchars;
      *read_bytes = cur_read_bytes - last_read_bytes;
      *write_bytes = cur_write_bytes - last_write_bytes;
    }
    else{
      *rchars = 0;
      *wchars = 0;
      *read_bytes = 0;
      *write_bytes = 0;
    }
    last_rchars = cur_rchars;
    last_wchars = cur_wchars;
    last_read_bytes = cur_read_bytes;
    last_write_bytes = cur_write_bytes;
  } else {
    return code;
  }
  return 0;
}
void taosSetDefaultProcIODelta(int64_t *rchars, int64_t *wchars, int64_t *read_bytes, int64_t *write_bytes) {
  if(rchars) *rchars = 0;
  if(wchars) *wchars = 0;
  if(read_bytes) *read_bytes = 0;
  if(write_bytes) *write_bytes = 0;
}

int32_t taosGetCardInfo(int64_t *receive_bytes, int64_t *transmit_bytes) {
  OS_PARAM_CHECK(receive_bytes);
  OS_PARAM_CHECK(transmit_bytes);
  *receive_bytes = 0;
  *transmit_bytes = 0;

#ifdef WINDOWS
  return 0;
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
  return 0;
#else
  TdFilePtr pFile = taosOpenFile(tsSysNetFile, TD_FILE_READ | TD_FILE_STREAM);
  if (pFile == NULL) {
    return terrno;
  }

  ssize_t _bytes = 0;
  char    line[1024];

  while (!taosEOFFile(pFile)) {
    int64_t o_rbytes = 0;
    int64_t rpackts = 0;
    int64_t o_tbytes = 0;
    int64_t tpackets = 0;
    int64_t nouse1 = 0;
    int64_t nouse2 = 0;
    int64_t nouse3 = 0;
    int64_t nouse4 = 0;
    int64_t nouse5 = 0;
    int64_t nouse6 = 0;
    char    nouse0[200] = {0};

    _bytes = taosGetsFile(pFile, sizeof(line), line);
    if (_bytes <= 0) {
      break;
    }

    line[_bytes - 1] = 0;

    if (strstr(line, "lo:") != NULL) {
      continue;
    }

    (void)sscanf(line,
           "%s %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64
           " %" PRId64,
           nouse0, &o_rbytes, &rpackts, &nouse1, &nouse2, &nouse3, &nouse4, &nouse5, &nouse6, &o_tbytes, &tpackets);
    *receive_bytes += o_rbytes;
    *transmit_bytes += o_tbytes;
  }

  TAOS_SKIP_ERROR(taosCloseFile(&pFile));

  return 0;
#endif
}

int32_t taosGetCardInfoDelta(int64_t *receive_bytes, int64_t *transmit_bytes) {
  OS_PARAM_CHECK(receive_bytes);
  OS_PARAM_CHECK(transmit_bytes);
  static int64_t last_receive_bytes = -1;
  static int64_t last_transmit_bytes = -1;
  int64_t cur_receive_bytes = 0;
  int64_t cur_transmit_bytes = 0;
  int32_t code = taosGetCardInfo(&cur_receive_bytes, &cur_transmit_bytes);
  if (code == 0) {
    if(last_receive_bytes >= 0 && last_transmit_bytes >= 0){
      *receive_bytes = cur_receive_bytes - last_receive_bytes;
      *transmit_bytes = cur_transmit_bytes - last_transmit_bytes;
    }
    else{
      *receive_bytes = 0;
      *transmit_bytes = 0;
    }

    last_receive_bytes = cur_receive_bytes;
    last_transmit_bytes = cur_transmit_bytes;
  } else {
    return code;
  }
  return 0;
}
void taosSetDefaultCardInfoDelta(int64_t *receive_bytes, int64_t *transmit_bytes) {
  if (receive_bytes) *receive_bytes = 0;
  if (transmit_bytes) *transmit_bytes = 0;
}

#if 0
void taosKillSystem() {
#ifdef WINDOWS
  printf("function taosKillSystem, exit!");
  exit(0);
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
  printf("function taosKillSystem, exit!");
  exit(0);
#else
  // SIGINT
  (void)printf("%sd will shut down soon", CUS_PROMPT);
  (void)kill(tsProcId, 2);
#endif
}
#endif

#define UUIDLEN (36)
int32_t taosGetSystemUUIDLimit36(char *uid, int32_t uidlen) {
  OS_PARAM_CHECK(uid);
#ifdef WINDOWS
  GUID guid;
  HRESULT h = CoCreateGuid(&guid);
  if (h != S_OK) {
    return TAOS_SYSTEM_WINAPI_ERROR(GetLastError());
  }
  (void)snprintf(uid, uidlen, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", guid.Data1, guid.Data2, guid.Data3,
           guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
           guid.Data4[7]);

  return 0;
#elif defined(_TD_DARWIN_64)
  uuid_t uuid = {0};
  char   buf[UUIDLEN37];
  memset(buf, 0, UUIDLEN37);
  uuid_generate(uuid);
  // it's caller's responsibility to make enough space for `uid`, that's 36-char + 1-null
  uuid_unparse_lower(uuid, buf);
  (void)snprintf(uid, uidlen, "%.*s", (int)sizeof(buf), buf);
  return 0;
#elif defined(TD_ASTRA)
  const char *template = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
  const char *hex_chars = "0123456789abcdef";
  int32_t     len = uidlen > 36 ? 36 : uidlen;

  for (int32_t i = 0; i < len; i++) {
    if (template[i] == 'x') {
      uid[i] = hex_chars[taosRand() & 15];
    } else if (template[i] == 'y') {
      uid[i] = hex_chars[(taosRand() & 3) + 8];  // 8, 9, a, or b
    } else {
      uid[i] = template[i];
    }
  }
  if (len >= 0) {
    uid[len] = 0;
  }

  return 0;
#else
  int64_t len = 0;

  // fd = open("/proc/sys/kernel/random/uuid", 0);
  TdFilePtr pFile = taosOpenFile("/proc/sys/kernel/random/uuid", TD_FILE_READ);
  if (pFile == NULL) {
    return terrno;
  } else {
    len = taosReadFile(pFile, uid, uidlen);
    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
    if (len < 0) {
      return terrno;
    }
  }
  if (len >= UUIDLEN + 1) {
    uid[len - 1] = 0;
  } else {
    uid[uidlen - 1] = 0;
  }

  return 0;
#endif
}

int32_t taosGetSystemUUIDLen(char *uid, int32_t uidlen) {
  if (uid == NULL || uidlen <= 0) {
    return TSDB_CODE_APP_ERROR;
  }
  int num = (uidlen % UUIDLEN == 0) ? (uidlen / UUIDLEN) : (uidlen / UUIDLEN + 1);
  int left = uidlen;
  for (int i = 0; i < num; ++i) {
    int32_t code = taosGetSystemUUIDLimit36(uid + i * UUIDLEN, left);
    if (code != 0) {
      return code;
    }
    left -= UUIDLEN;
  }
  return TSDB_CODE_SUCCESS;
}

char *taosGetCmdlineByPID(int pid) {
#ifdef WINDOWS
  return "";
#elif defined(_TD_DARWIN_64)
  static char cmdline[1024];
  SET_ERRNO(0);

  if (proc_pidpath(pid, cmdline, sizeof(cmdline)) <= 0) {
    fprintf(stderr, "PID is %d, %s", pid, strerror(ERRNO));
    return strerror(ERRNO);
  }

  return cmdline;
#elif defined(TD_ASTRA)
  return "";
#else
  static char cmdline[1024];
  (void)snprintf(cmdline, sizeof(cmdline), "/proc/%d/cmdline", pid);

  // int fd = open(cmdline, O_RDONLY);
  TdFilePtr pFile = taosOpenFile(cmdline, TD_FILE_READ);
  if (pFile != NULL) {
    int n = taosReadFile(pFile, cmdline, sizeof(cmdline) - 1);
    if (n < 0) n = 0;

    if (n > 0 && cmdline[n - 1] == '\n') --n;

    cmdline[n] = 0;

    TAOS_SKIP_ERROR(taosCloseFile(&pFile));
  } else {
    cmdline[0] = 0;
  }

  return cmdline;
#endif
}

int64_t taosGetOsUptime() {
#ifdef WINDOWS
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
#else
  struct sysinfo info;
  if (-1 == sysinfo(&info)) {
    terrno = TAOS_SYSTEM_ERROR(ERRNO);
    return terrno;
  }
  
  return (int64_t)info.uptime * 1000;
  
#endif
  return 0;
}

void taosSetCoreDump(bool enable) {
  if (!enable) return;
#ifdef WINDOWS
  SetUnhandledExceptionFilter(exceptionHandler);
  SetUnhandledExceptionFilter(&FlCrashDump);
#elif defined(_TD_DARWIN_64) || defined(TD_ASTRA)
#else
  // 1. set ulimit -c unlimited
  struct rlimit rlim;
  struct rlimit rlim_new;
  if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
#ifndef _ALPINE
    // printf("the old unlimited para: rlim_cur=%" PRIu64 ", rlim_max=%" PRIu64, rlim.rlim_cur, rlim.rlim_max);
#else
    // printf("the old unlimited para: rlim_cur=%llu, rlim_max=%llu", rlim.rlim_cur, rlim.rlim_max);
#endif
    rlim_new.rlim_cur = RLIM_INFINITY;
    rlim_new.rlim_max = RLIM_INFINITY;
    if (setrlimit(RLIMIT_CORE, &rlim_new) != 0) {
      // printf("set unlimited fail, error: %s", strerror(ERRNO));
      rlim_new.rlim_cur = rlim.rlim_max;
      rlim_new.rlim_max = rlim.rlim_max;
      (void)setrlimit(RLIMIT_CORE, &rlim_new);
    }
  }

  if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
#ifndef _ALPINE
    // printf("the new unlimited para: rlim_cur=%" PRIu64 ", rlim_max=%" PRIu64, rlim.rlim_cur, rlim.rlim_max);
#else
    // printf("the new unlimited para: rlim_cur=%llu, rlim_max=%llu", rlim.rlim_cur, rlim.rlim_max);
#endif
  }

#ifndef _TD_ARM_
  // 2. set the path for saving core file
  struct __sysctl_args args;

  int    old_usespid = 0;
  size_t old_len = 0;
  int    new_usespid = 1;
  size_t new_len = sizeof(new_usespid);

  int name[] = {CTL_KERN, KERN_CORE_USES_PID};

  (void)memset(&args, 0, sizeof(struct __sysctl_args));
  args.name = name;
  args.nlen = sizeof(name) / sizeof(name[0]);
  args.oldval = &old_usespid;
  args.oldlenp = &old_len;
  args.newval = &new_usespid;
  args.newlen = new_len;

  old_len = sizeof(old_usespid);

#ifndef __loongarch64
  if (syscall(SYS__sysctl, &args) == -1) {
    // printf("_sysctl(kern_core_uses_pid) set fail: %s", strerror(ERRNO));
  }
#endif

  // printf("The old core_uses_pid[%" PRIu64 "]: %d", old_len, old_usespid);

  old_usespid = 0;
  old_len = 0;
  (void)memset(&args, 0, sizeof(struct __sysctl_args));
  args.name = name;
  args.nlen = sizeof(name) / sizeof(name[0]);
  args.oldval = &old_usespid;
  args.oldlenp = &old_len;

  old_len = sizeof(old_usespid);

#ifndef __loongarch64
  if (syscall(SYS__sysctl, &args) == -1) {
    // printf("_sysctl(kern_core_uses_pid) get fail: %s", strerror(ERRNO));
  }
#endif

  // printf("The new core_uses_pid[%" PRIu64 "]: %d", old_len, old_usespid);
#endif
#endif
}

SysNameInfo taosGetSysNameInfo() {
#ifdef WINDOWS
  SysNameInfo info = {0};
  DWORD       dwVersion = GetVersion();

  char *tmp = NULL;
  tmp = getenv("OS");
  if (tmp != NULL) tstrncpy(info.sysname, tmp, sizeof(info.sysname));
  tmp = getenv("COMPUTERNAME");
  if (tmp != NULL) tstrncpy(info.nodename, tmp, sizeof(info.nodename));
  sprintf_s(info.release, sizeof(info.release), "%d", dwVersion & 0x0F);
  sprintf_s(info.version, sizeof(info.release), "%d", (dwVersion >> 8) & 0x0F);
  tmp = getenv("PROCESSOR_ARCHITECTURE");
  if (tmp != NULL) tstrncpy(info.machine, tmp, sizeof(info.machine));

  return info;
#elif defined(_TD_DARWIN_64)
  SysNameInfo info = {0};

  struct utsname uts;
  if (!uname(&uts)) {
    tstrncpy(info.sysname, uts.sysname, sizeof(info.sysname));
    tstrncpy(info.nodename, uts.nodename, sizeof(info.nodename));
    tstrncpy(info.release, uts.release, sizeof(info.release));
    tstrncpy(info.version, uts.version, sizeof(info.version));
    tstrncpy(info.machine, uts.machine, sizeof(info.machine));
  }

  char     localHostName[512];
  TAOS_SKIP_ERROR(taosGetlocalhostname(localHostName, 512));
  TdCmdPtr pCmd = taosOpenCmd("scutil --get LocalHostName");
  tstrncpy(info.nodename, localHostName, sizeof(info.nodename));

  return info;
#else
  SysNameInfo info = {0};
  struct utsname uts;
  if (!uname(&uts)) {
    tstrncpy(info.sysname, uts.sysname, sizeof(info.sysname));
    tstrncpy(info.nodename, uts.nodename, sizeof(info.nodename));
    tstrncpy(info.release, uts.release, sizeof(info.release));
    tstrncpy(info.version, uts.version, sizeof(info.version));
    tstrncpy(info.machine, uts.machine, sizeof(info.machine));
  } else {
    terrno = TAOS_SYSTEM_ERROR(ERRNO);
  }

  return info;
#endif
}

bool taosCheckCurrentInDll() {
#ifdef WINDOWS
  MEMORY_BASIC_INFORMATION mbi;
  char                     path[PATH_MAX] = {0};
  GetModuleFileName(
      ((VirtualQuery(taosCheckCurrentInDll, &mbi, sizeof(mbi)) != 0) ? (HMODULE)mbi.AllocationBase : NULL), path,
      PATH_MAX);
  int strLastIndex = strlen(path);
  if ((path[strLastIndex - 3] == 'd' || path[strLastIndex - 3] == 'D') &&
      (path[strLastIndex - 2] == 'l' || path[strLastIndex - 2] == 'L') &&
      (path[strLastIndex - 1] == 'l' || path[strLastIndex - 1] == 'L')) {
    return true;
  }
  return false;
#else
  return false;
#endif
}

#ifdef _TD_DARWIN_64
int32_t taosGetMaclocalhostnameByCommand(char *hostname, size_t maxLen) {
  TdCmdPtr pCmd = taosOpenCmd("scutil --get LocalHostName");
  if (pCmd != NULL) {
    if (taosGetsCmd(pCmd, maxLen - 1, hostname) > 0) {
      int len = strlen(hostname);
      if (hostname[len - 1] == '\n') {
        hostname[len - 1] = '\0';
      }
      return 0;
    }
    taosCloseCmd(&pCmd);
  }
  return TAOS_SYSTEM_ERROR(ERRNO);
}

int32_t getMacLocalHostNameBySCD(char *hostname, size_t maxLen) {
  SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR(""), NULL, NULL);
  CFStringRef       hostname_cfstr = SCDynamicStoreCopyLocalHostName(store);
  if (hostname_cfstr != NULL) {
    CFStringGetCString(hostname_cfstr, hostname, maxLen - 1, kCFStringEncodingMacRoman);
    CFRelease(hostname_cfstr);
  } else {
    return -1;
  }
  CFRelease(store);
  return 0;
}
#endif

int32_t taosGetlocalhostname(char *hostname, size_t maxLen) {
  OS_PARAM_CHECK(hostname);
#ifdef _TD_DARWIN_64
  int res = getMacLocalHostNameBySCD(hostname, maxLen);
  if (res != 0) {
    return taosGetMaclocalhostnameByCommand(hostname, maxLen);
  } else {
    return 0;
  }
#else
  int r = gethostname(hostname, maxLen);
  if (-1 == r) {
    terrno = TAOS_SYSTEM_ERROR(ERRNO);
    return terrno;
  }
  return r;
#endif
}
