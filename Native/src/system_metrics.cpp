/**
 * =============================================================================
 * Windows System Metrics Tool (REST API / JSON)
 * =============================================================================
 *
 * PURPOSE:
 * Reads system metrics and outputs a single JSON payload suitable for REST API
 * submission (e.g. POST body). Each section includes timestamps as primary or
 * composite keys.
 *
 * OUTPUT: One JSON object to stdout with:
 *   - cpu: { cpu_mhz, cpu_usage_percent, system_responsiveness_percent, timestamp }
 *   - cpu_cores: [ { core_index, core_mhz, core_usage_percent, timestamp }, ... ]
 *   - memory: { total_bytes, free_bytes, used_bytes, memory_usage_percent,
 *               page_fault_count, timestamp }
 *   - disk: [ { drive_letter, total_bytes, free_bytes, read_speed_bytes_per_sec,
 *               write_speed_bytes_per_sec, timestamp }, ... ]
 *   - processes: [ { pid, name, owner, priority, cpu_percent, cpu_time_100ns,
 *                    location, timestamp }, ... ]
 *
 * NOTE: CPU/per-core usage and process cpu_percent are primed with short delays
 * so the first (and only) output has meaningful percentages.
 *
 * WINDOWS SERVICE MODE:
 *   Install: system_metrics.exe install   (requires admin)
 *   Remove:  system_metrics.exe remove
 *   When started by SCM, runs a loop writing metrics to JSON every 2 seconds.
 *
 * COMPILATION:
 *   cl /EHsc /W4 /std:c++17 /Fe:build\system_metrics.exe src\system_metrics.cpp ^
 *      /link psapi.lib advapi32.lib ntdll.lib pdh.lib powrprof.lib
 *
 * =============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <powerbase.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <map>
#include <cstdio>
#include <cstring>
#include <fstream>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "powrprof.lib")

/* PROCESSOR_POWER_INFORMATION may be omitted from headers; define for per-core CurrentMhz */
#ifndef _PROCESSOR_POWER_INFORMATION_DEFINED
typedef struct _PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION, *PPROCESSOR_POWER_INFORMATION;
#define _PROCESSOR_POWER_INFORMATION_DEFINED
#endif

#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0
#endif

/* Service name and poll interval (ms). ~2s between outputs (1s disk + 1s process sampling). */
#define SVC_NAME          "SystemMetricsService"
#define SVC_DISPLAY_NAME  "System Metrics (JSON poller)"
#define METRICS_POLL_MS   0

static SERVICE_STATUS_HANDLE g_svcStatusHandle = nullptr;
static SERVICE_STATUS g_svcStatus = {};
static volatile LONG g_svcStopRequested = 0;

/* =============================================================================
 * JSON helpers and metrics data structures for REST API payload
 * ============================================================================= */

 // Converts an arbitrary string to a form that is safe to embed in JSON.
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"') out += "\\\""; // Escape double quotes.
        else if (c == '\\') out += "\\\\"; // Escape backslashes.
        else if (c == '\b') out += "\\b"; // Escape backspace.
        else if (c == '\f') out += "\\f"; // Escape form feeds. 
        else if (c == '\n') out += "\\n"; // Escape newlines.
        else if (c == '\r') out += "\\r"; // Escape carriage returns.
        else if (c == '\t') out += "\\t"; // Escape tabs.
        else if (c < 32) { char buf[8]; sprintf_s(buf, "\\u%04x", c); out += buf; } // Escape control characters.
        else out += c; 
    }
    return out;
}
// Assumes text is already encoded as UTF-8. Otherwise, use a proper JSON encoder.

struct CpuData { DWORD mhz = 0; double usage_percent = 0.0; double responsiveness_percent = 100.0; std::string timestamp; };
struct CpuCoreEntry { DWORD index = 0; DWORD mhz = 0; double usage_percent = 0.0; std::string timestamp; };
struct MemoryData { ULONGLONG total_bytes = 0, free_bytes = 0, used_bytes = 0; DWORD usage_percent = 0, page_fault_count = 0; std::string timestamp; };
struct DiskEntry { std::string drive_letter; ULONGLONG total_bytes = 0, free_bytes = 0, read_speed = 0, write_speed = 0; std::string timestamp; };

/* -----------------------------------------------------------------------------
 * Ntdll types for per-core CPU (undocumented but stable)
 * SystemProcessorPerformanceInformation = 8
 * ----------------------------------------------------------------------------- */
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// NTSTATUS is a long integer that represents the status of an operation.
typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *PNtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION is a structure that contains the performance information for a processor.
typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, *PSYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

#define SystemProcessorPerformanceInformation 8 

/* -----------------------------------------------------------------------------
 * Timestamp as string (ISO-like) for primary key use
 * ----------------------------------------------------------------------------- */
 // GetTimestamp() returns a string representation of the current time in the format of YYYY-MM-DD HH:MM:SS.mmm
static std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm_buf;
    localtime_s(&tm_buf, &time);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S"); // Format the time as YYYY-MM-DD HH:MM:SS
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count(); // Add the milliseconds.
    return oss.str();
}

/* =============================================================================
 * CPU: MHz, current usage, timestamp
 * ============================================================================= */
static ULONGLONG s_prevIdle = 0, s_prevKernel = 0, s_prevUser = 0, s_prevTotal = 0;

static bool GetCpuMhz(DWORD& outMhz) {
    HKEY hKey;
    // Open the registry key for the first processor. If the key is not found, return false.
    // The key is located at "HARDWARE\DESCRIPTION\System\CentralProcessor\0".
    
    //! See this block if the Windows registry is not available on a given restricted environment, or
    // if a future Windows update changes where the registry key is located.
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type, size = sizeof(DWORD);
    LONG r = RegQueryValueExA(hKey, "~MHz", nullptr, &type, (LPBYTE)&outMhz, &size);
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS && type == REG_DWORD);
}

// GetCpuUsagePercent() returns the CPU usage percentage.
// Calculates the CPU usage percentage based on the system times
// uses the previous total and idle times to calculate the usage percentage.
static double GetCpuUsagePercent() {
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
        return 0.0;
    ULARGE_INTEGER uIdle, uKernel, uUser;
    uIdle.LowPart = idleTime.dwLowDateTime;
    uIdle.HighPart = idleTime.dwHighDateTime;
    uKernel.LowPart = kernelTime.dwLowDateTime;
    uKernel.HighPart = kernelTime.dwHighDateTime;
    uUser.LowPart = userTime.dwLowDateTime;
    uUser.HighPart = userTime.dwHighDateTime;
    ULONGLONG idle = uIdle.QuadPart, kernel = uKernel.QuadPart, user = uUser.QuadPart;
    ULONGLONG total = kernel + user;
    double pct = 0.0;

    // Using the formula: (total - idle) / total * 100 
    // and the previous total and idle times to calculate the usage percentage.
    // The usage percentage is then stored in the s_prevTotal and s_prevIdle variables.
    if (s_prevTotal != 0 && total > s_prevTotal) {
        ULONGLONG dTotal = total - s_prevTotal;
        ULONGLONG dIdle = idle - s_prevIdle;
        if (dTotal > 0)
            pct = 100.0 * (1.0 - (double)(dIdle) / (double)dTotal);
    }
    s_prevIdle = idle;
    s_prevKernel = kernel;
    s_prevUser = user;
    s_prevTotal = total;
    return pct;
}

/* =============================================================================
 * System responsiveness: 100% when no DPC/interrupt delay
 * =============================================================================
 * Uses SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION deltas across all processors.
 * We treat DpcTime + InterruptTime as "delay" time inside kernel; responsiveness
 * is 100% when that share is 0, and decreases as the share increases. This differs
 * traditional measures of responsiveness, which typically use the ratio of kernel time to total time.
 */

static double GetSystemResponsivenessPercent() {
    static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prevInfo;
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    // Estimate 100% responsiveness if the NtQuerySystemInformation function is not available.
    // or if the CPU just spent ~0 iota of time handling interrupts and DPCs.
    if (!hNtdll) return 100.0;
    auto pNtQuery = (PNtQuerySystemInformation)GetProcAddress(hNtdll, "NtQuerySystemInformation");
    if (!pNtQuery) return 100.0;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD nCpus = si.dwNumberOfProcessors;
    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> info(nCpus);
    ULONG len = 0;
    if (pNtQuery(SystemProcessorPerformanceInformation, info.data(),
            (ULONG)(info.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)), &len) != 0)
        return 100.0;

    if (prevInfo.empty() || prevInfo.size() != info.size()) {
        prevInfo = info;
        return 100.0;
    }

    long double total = 0.0L;
    long double delay = 0.0L;

    // Sum across all processors to get the total time and the delay time.
    for (DWORD i = 0; i < nCpus; i++) {
        const auto& now = info[i];
        const auto& prev = prevInfo[i];

        ULONGLONG kNow = (ULONGLONG)now.KernelTime.QuadPart;
        ULONGLONG uNow = (ULONGLONG)now.UserTime.QuadPart;
        ULONGLONG kPrev = (ULONGLONG)prev.KernelTime.QuadPart;
        ULONGLONG uPrev = (ULONGLONG)prev.UserTime.QuadPart;
        // Use the formula: (kNow + uNow) - (kPrev + uPrev) to get the total time.
        LONGLONG dTotal = (LONGLONG)((kNow + uNow) - (kPrev + uPrev));
        if (dTotal <= 0)
            continue;

        ULONGLONG dDpc = (ULONGLONG)(now.DpcTime.QuadPart - prev.DpcTime.QuadPart);
        ULONGLONG dInt = (ULONGLONG)(now.InterruptTime.QuadPart - prev.InterruptTime.QuadPart);

        total += (long double)dTotal;
        delay += (long double)(dDpc + dInt);
    }

    prevInfo = info;

    if (total <= 0.0L)
        return 100.0;

    // Clamp the fraction to 0-1 and convert to a percentage.
    long double frac = delay / total;
    if (frac < 0.0L) frac = 0.0L;
    if (frac > 1.0L) frac = 1.0L;
    long double resp = (1.0L - frac) * 100.0L;
    if (resp < 0.0L) resp = 0.0L;
    if (resp > 100.0L) resp = 100.0L;
    return static_cast<double>(resp);
}

static std::vector<double> GetPerCoreUsage();  /* forward declaration */

// Cleanly package CPU data for the REST API. 
static CpuData GetCpuData() {
    CpuData d;
    d.timestamp = GetTimestamp();
    GetCpuMhz(d.mhz);
    d.usage_percent = GetCpuUsagePercent();
    d.responsiveness_percent = GetSystemResponsivenessPercent();
    return d;
}

/* Prime CPU and per-core samplers so the first output has a valid delta. */
static void PrimeCpuReadings() {
    GetCpuUsagePercent();
    GetPerCoreUsage();
}

/* =============================================================================
 * CPU Cores: index, MHz (current at sample time), usage, timestamp
 * ============================================================================= */
/* Get current MHz for each logical processor (may vary per core; falls back to registry if API fails). */
static std::vector<DWORD> GetPerCoreCurrentMhz() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<DWORD> mhz(si.dwNumberOfProcessors, 0);
    std::vector<PROCESSOR_POWER_INFORMATION> buf(si.dwNumberOfProcessors);
    ULONG bufLen = (ULONG)(buf.size() * sizeof(PROCESSOR_POWER_INFORMATION));
    /* ProcessorInformation = 11 (per-processor current MHz at sample time) */
    if (CallNtPowerInformation((POWER_INFORMATION_LEVEL)11, nullptr, 0, buf.data(), bufLen) == 0) {
        for (size_t i = 0; i < buf.size() && i < mhz.size(); i++)
            mhz[i] = buf[i].CurrentMhz;
        return mhz;
    }
    /* Fallback: registry ~MHz (nominal/max) per core */
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
        std::string path = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\" + std::to_string(i);
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD type, size = sizeof(DWORD);
            DWORD val = 0;
            if (RegQueryValueExA(hKey, "~MHz", nullptr, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS && type == REG_DWORD)
                mhz[i] = val;
            RegCloseKey(hKey);
        }
    }
    return mhz;
}

// Get from the same location as responsiveness with SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION.
// Calculate the usage percent as 100 * (1 - (idleNow - idlePrev) / (totalNow - totalPrev))
// for dtotal > 0. 
// Call GetPerCoreUsage() once before the real sampling to prime the previous values.
static std::vector<double> GetPerCoreUsage() {
    static std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prevInfo;
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");

    if (!hNtdll) return {};
    auto pNtQuery = (PNtQuerySystemInformation)GetProcAddress(hNtdll, "NtQuerySystemInformation");
    if (!pNtQuery) return {};
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    DWORD nCpus = si.dwNumberOfProcessors;
    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> info(nCpus);
    ULONG len = 0;

    if (pNtQuery(SystemProcessorPerformanceInformation, info.data(),
            (ULONG)(info.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)), &len) != 0)
        return {};

    std::vector<double> usage(nCpus, 0.0);

    if (!prevInfo.empty() && prevInfo.size() == info.size()) {
        for (size_t i = 0; i < info.size(); i++) {
            ULONGLONG idleNow = (ULONGLONG)info[i].IdleTime.QuadPart;
            ULONGLONG idlePrev = (ULONGLONG)prevInfo[i].IdleTime.QuadPart;
            ULONGLONG kernelNow = (ULONGLONG)info[i].KernelTime.QuadPart;
            ULONGLONG userNow = (ULONGLONG)info[i].UserTime.QuadPart;
            ULONGLONG kernelPrev = (ULONGLONG)prevInfo[i].KernelTime.QuadPart;
            ULONGLONG userPrev = (ULONGLONG)prevInfo[i].UserTime.QuadPart;
            ULONGLONG totalNow = kernelNow + userNow;
            ULONGLONG totalPrev = kernelPrev + userPrev;
            ULONGLONG dTotal = totalNow - totalPrev;
            ULONGLONG dIdle = idleNow - idlePrev;
            if (dTotal > 0)
                usage[i] = 100.0 * (1.0 - (double)dIdle / (double)dTotal);
        }
    }

    prevInfo = info;
    return usage;
}

// Defensively falls back to 0.0 usage if the number of processors is not available.
// Or resizes the vector to fit the number of processors on the system
static std::vector<CpuCoreEntry> GetCpuCoresData() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<double> coreUsage = GetPerCoreUsage();
    if (coreUsage.size() != si.dwNumberOfProcessors)
        coreUsage.resize(si.dwNumberOfProcessors, 0.0);
    std::vector<DWORD> coreMhz = GetPerCoreCurrentMhz();
    if (coreMhz.size() != si.dwNumberOfProcessors)
        coreMhz.resize(si.dwNumberOfProcessors, 0);
    std::string ts = GetTimestamp();
    std::vector<CpuCoreEntry> list;
    for (DWORD i = 0; i < si.dwNumberOfProcessors; i++) {
        CpuCoreEntry e;
        e.index = i;
        e.mhz = coreMhz[i];
        e.usage_percent = coreUsage[i];
        e.timestamp = ts;
        list.push_back(e);
    }
    return list;
}

/* =============================================================================
 * Memory: total, free, used, usage %, page faults, timestamp
 * ============================================================================= */

 // Initialize the memory data structure and get the memory usage percentage from the PDH counter.
 // Return default-initialized MemoryData d of zeroes if the GlobalMemoryStatusEx function fails.
static MemoryData GetMemoryData() {
    MemoryData d;
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return d;
    static PDH_HQUERY hMemQuery = nullptr;
    static PDH_HCOUNTER hPageFaults = nullptr;
    static bool memInit = false;
    if (!memInit) {
        // Use English counter names for the Page Faults/sec counter.
        // This is more likely to be available on all systems.
        if (PdhOpenQueryA(nullptr, 0, &hMemQuery) == ERROR_SUCCESS &&
            PdhAddEnglishCounterA(hMemQuery, "\\Memory\\Page Faults/sec", 0, &hPageFaults) == ERROR_SUCCESS) {
            PdhCollectQueryData(hMemQuery);
            memInit = true;
        }
    }
    if (memInit && PdhCollectQueryData(hMemQuery) == ERROR_SUCCESS) {
        PDH_FMT_COUNTERVALUE val;
        if (PdhGetFormattedCounterValue(hPageFaults, PDH_FMT_LONG, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
            d.page_fault_count = (DWORD)val.longValue;
    }
    d.total_bytes = mem.ullTotalPhys;
    d.free_bytes = mem.ullAvailPhys;
    d.used_bytes = d.total_bytes - d.free_bytes;
    d.usage_percent = mem.dwMemoryLoad;
    d.timestamp = GetTimestamp();
    return d;
}

// Note for above: PDH rate counters (Disk Read/Write Bytes/sec) need two samples with a time
// interval between them; otherwise the rate is 0. We collect once, sleep 1s,
// collect again, then read the value.
// If _Total reports 0 for read (known on some systems), we sum per-disk instances.

/* =============================================================================
 * Disk: drive letter, total, free, read_speed, write_speed, timestamp
 * =============================================================================
 * PDH rate counters (Disk Read/Write Bytes/sec) need two samples with a time
 * interval between them; otherwise the rate is 0. We collect once, sleep 1s,
 * collect again, then read the value.
 * If _Total reports 0 for read (known on some systems), we sum per-disk instances.
 */
static void GetDiskSpeeds(ULONGLONG& readBytesPerSec, ULONGLONG& writeBytesPerSec) {
    static PDH_HQUERY hQuery = nullptr;
    static PDH_HCOUNTER hRead = nullptr, hWrite = nullptr;
    static bool init = false;
    readBytesPerSec = 0;
    writeBytesPerSec = 0;

    // Again, we need two samples with an interval between them; otherwise the rate is 0.
    // This function, as with memory, will introduce ~1s of latency per call. 

    // Potentially > 2s of latency per call if the defensive fallbacks are reached. 
    if (!init) {
        if (PdhOpenQueryA(nullptr, 0, &hQuery) != ERROR_SUCCESS) return;
        /* Try _Total first (all disks); fallback to first physical disk (0) */
        if (PdhAddEnglishCounterA(hQuery, "\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &hRead) != ERROR_SUCCESS &&
            PdhAddEnglishCounterA(hQuery, "\\PhysicalDisk(0)\\Disk Read Bytes/sec", 0, &hRead) != ERROR_SUCCESS) {
            PdhCloseQuery(hQuery);
            return;
        }
        if (PdhAddEnglishCounterA(hQuery, "\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &hWrite) != ERROR_SUCCESS &&
            PdhAddEnglishCounterA(hQuery, "\\PhysicalDisk(0)\\Disk Write Bytes/sec", 0, &hWrite) != ERROR_SUCCESS) {
            PdhCloseQuery(hQuery);
            return;
        }
        PdhCollectQueryData(hQuery);
        init = true;
    }

    /* Rate counters need two samples with an interval; sleep 1s then collect again */
    Sleep(1000); // Sleep for 1 second to collect the second sample.
    if (PdhCollectQueryData(hQuery) != ERROR_SUCCESS) return;
    PDH_FMT_COUNTERVALUE val;

    /* Read: some systems return rate as double; try both formats */
    if (PdhGetFormattedCounterValue(hRead, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
        readBytesPerSec = (ULONGLONG)val.doubleValue;

    if (readBytesPerSec == 0 && PdhGetFormattedCounterValue(hRead, PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
        readBytesPerSec = val.largeValue;
    /* Fallback: _Total often reports 0 for read on some Windows/drivers; sum instances 0..3 */

    if (readBytesPerSec == 0) {
        PDH_HQUERY hQ2 = nullptr;
        PDH_HCOUNTER hCounters[4] = { nullptr };
        int nAdded = 0;
        if (PdhOpenQueryA(nullptr, 0, &hQ2) == ERROR_SUCCESS) {
            for (int i = 0; i <= 3; i++) {
                char path[128];
                sprintf_s(path, "\\PhysicalDisk(%d)\\Disk Read Bytes/sec", i);
                if (PdhAddEnglishCounterA(hQ2, path, 0, &hCounters[nAdded]) == ERROR_SUCCESS)
                    nAdded++;
            }
            // Sum the read rates from all disks.
            if (nAdded > 0) {
                PdhCollectQueryData(hQ2);
                Sleep(1000); // Sleep for 1 second to collect the second sample.
                if (PdhCollectQueryData(hQ2) == ERROR_SUCCESS) {
                    ULONGLONG sum = 0;
                    for (int j = 0; j < nAdded; j++) {
                        if (PdhGetFormattedCounterValue(hCounters[j], PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
                            sum += val.largeValue;
                        else if (PdhGetFormattedCounterValue(hCounters[j], PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
                            sum += (ULONGLONG)val.doubleValue;
                    }
                    if (sum > 0) readBytesPerSec = sum;
                }
            }
            PdhCloseQuery(hQ2);
        }
    }

    // Write: same as read; try both formats.
    if (PdhGetFormattedCounterValue(hWrite, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
        writeBytesPerSec = (ULONGLONG)val.doubleValue;

    if (writeBytesPerSec == 0 && PdhGetFormattedCounterValue(hWrite, PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS && val.CStatus == PDH_CSTATUS_VALID_DATA)
        writeBytesPerSec = val.largeValue;
}

static std::vector<DiskEntry> GetDiskData() {
    std::vector<DiskEntry> list;
    std::string ts = GetTimestamp();
    char drives[256];
    if (GetLogicalDriveStringsA(sizeof(drives) - 1, drives) == 0) return list;
    ULONGLONG readSpeed = 0, writeSpeed = 0;
    GetDiskSpeeds(readSpeed, writeSpeed);

    // Iterate over all logical drives and get the disk usage data.
    // Why do we step by 4 characters? Because the logical drive strings are separated by null characters.
    //! This can break if Windows ever changes the format of the logical drive strings or returns a drive
    // letter longer than 3 characters.
    for (char* p = drives; *p; p += 4) {
        std::string root(p);
        if (root.size() >= 2) root.resize(2);
        UINT type = GetDriveTypeA(p);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER freeBytes, totalBytes, totalFree;
        if (!GetDiskFreeSpaceExA(p, (PULARGE_INTEGER)&freeBytes, (PULARGE_INTEGER)&totalBytes, (PULARGE_INTEGER)&totalFree))
            continue;
        DiskEntry e;
        e.drive_letter = (root.size() >= 2 && root[1] == '\\') ? root.substr(0, 1) : root;
        e.total_bytes = totalBytes.QuadPart;
        e.free_bytes = totalFree.QuadPart;
        e.read_speed = readSpeed;
        e.write_speed = writeSpeed;
        e.timestamp = ts;
        list.push_back(e);
    }
    return list;
}

/* =============================================================================
 * Process owner from token
 * ============================================================================= */
 // Get the owner of the process. This is a security-sensitive operation and should be used with caution.
static std::string GetProcessOwner(HANDLE hProcess) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) return "";
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    if (needed == 0) { CloseHandle(hToken); return ""; }
    std::vector<char> buf(needed);
    if (!GetTokenInformation(hToken, TokenUser, buf.data(), (DWORD)buf.size(), &needed)) {
        CloseHandle(hToken);
        return "";
    }
    TOKEN_USER* tu = (TOKEN_USER*)buf.data();
    char name[256], domain[256];
    DWORD nameLen = 256, domainLen = 256;
    SID_NAME_USE use;
    if (!LookupAccountSidA(nullptr, tu->User.Sid, name, &nameLen, domain, &domainLen, &use)) {
        CloseHandle(hToken);
        return "";
    }
    CloseHandle(hToken);
    if (domainLen > 0 && domain[0])
        return std::string(domain) + "\\" + name;
    return name;
}

// Convert the priority class to a string.
// Again, security-sensitive because it can reveal information about the process's priority
// to unauthorized users.

//! Be careful with real-time priority class; it can cause the process to starve other processes.
//! Avoid setting it unless required in testing or debugging. 
static std::string PriorityClassToString(DWORD pc) {
    switch (pc) {
        case REALTIME_PRIORITY_CLASS: return "REALTIME";
        case HIGH_PRIORITY_CLASS: return "HIGH";
        case ABOVE_NORMAL_PRIORITY_CLASS: return "ABOVE_NORMAL";
        case NORMAL_PRIORITY_CLASS: return "NORMAL";
        case BELOW_NORMAL_PRIORITY_CLASS: return "BELOW_NORMAL";
        case IDLE_PRIORITY_CLASS: return "IDLE";
        default: return "UNKNOWN";
    }
}

/* =============================================================================
 * Processes: PID, name, owner, priority, cpu_percent, cpu_time, location, timestamp
 * ============================================================================= */
 // Defines the core structure of per-process metrics in the JSON.
struct ProcessMetrics {
    DWORD pid;
    std::string name;
    std::string owner;
    std::string priority;
    double cpuPercent;
    ULONGLONG cpuTime100ns;
    ULONGLONG memoryWorkingSetBytes;
    double memoryPercent;
    std::string location;
};

// Read the following: 
// - total RAM (for memory percentage calculation)
// - total system time (for CPU percentage calculation)
// - total user time (for CPU percentage calculation)

// For each process, do the following:
// - Store defaults 
// - Attempt to open the process handle
// - If successful, populate the process owner, priority, and location
static std::vector<ProcessMetrics> GetProcessMetrics() {
    static std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> prevTimes;
    static ULONGLONG prevTotalKernel = 0, prevTotalUser = 0;
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    const long double totalPhys = (mem.ullTotalPhys > 0) ? (long double)mem.ullTotalPhys : 0.0L;
    FILETIME ftIdle, ftKernel, ftUser;
    GetSystemTimes(&ftIdle, &ftKernel, &ftUser);
    ULARGE_INTEGER uK, uU;
    uK.LowPart = ftKernel.dwLowDateTime; uK.HighPart = ftKernel.dwHighDateTime;
    uU.LowPart = ftUser.dwLowDateTime;   uU.HighPart = ftUser.dwHighDateTime;
    ULONGLONG totalSystem = uK.QuadPart + uU.QuadPart;
    std::vector<ProcessMetrics> list;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return list;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);

    // Same as obtaining overall CPU usage: we need two samples with an interval between them.
    if (!Process32First(hSnap, &pe)) { CloseHandle(hSnap); return list; }
    do {
        ProcessMetrics pm;
        // Set the process ID, name, owner, priority, and location to defaults.
        pm.pid = pe.th32ProcessID;
        pm.name = pe.szExeFile;
        pm.owner = "";
        pm.priority = "NORMAL";
        pm.cpuPercent = 0.0;
        pm.cpuTime100ns = 0;
        pm.memoryWorkingSetBytes = 0;
        pm.memoryPercent = 0.0;
        pm.location = "";

        // Open the process handle.
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, pe.th32ProcessID);
        if (hProc) {
            pm.owner = GetProcessOwner(hProc);
            DWORD pc = GetPriorityClass(hProc);
            if (pc) pm.priority = PriorityClassToString(pc);
            FILETIME create, exit, kernel, user;
            if (GetProcessTimes(hProc, &create, &exit, &kernel, &user)) {
                ULARGE_INTEGER uk, uu;
                uk.LowPart = kernel.dwLowDateTime; uk.HighPart = kernel.dwHighDateTime;
                uu.LowPart = user.dwLowDateTime;   uu.HighPart = user.dwHighDateTime;
                pm.cpuTime100ns = uk.QuadPart + uu.QuadPart;
                auto it = prevTimes.find(pm.pid);
                if (it != prevTimes.end() && totalSystem > prevTotalKernel + prevTotalUser) {
                    ULONGLONG dProc = pm.cpuTime100ns - it->second.first;
                    ULONGLONG dSys = totalSystem - (prevTotalKernel + prevTotalUser);
                    if (dSys > 0)
                        pm.cpuPercent = 100.0 * (double)dProc / (double)dSys;
                }
                prevTimes[pm.pid] = { pm.cpuTime100ns, 0 };
            }
            // Get the location of the process.
            char path[MAX_PATH] = {};
            if (GetModuleFileNameExA(hProc, nullptr, path, MAX_PATH))
                pm.location = path;

            // Get the memory usage of the process.
            PROCESS_MEMORY_COUNTERS_EX pmc = {};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(hProc, (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof(pmc))) {
                pm.memoryWorkingSetBytes = (ULONGLONG)pmc.WorkingSetSize;
                if (totalPhys > 0.0L) {
                    long double pct = ((long double)pm.memoryWorkingSetBytes / totalPhys) * 100.0L;
                    if (pct < 0.0L) pct = 0.0L;
                    if (pct > 100.0L) pct = 100.0L;
                    pm.memoryPercent = (double)pct;
                }
            }
            CloseHandle(hProc);
        }
        list.push_back(pm);
    } while (Process32Next(hSnap, &pe));
    prevTotalKernel = uK.QuadPart;
    prevTotalUser = uU.QuadPart;
    CloseHandle(hSnap);
    return list;
}

/* =============================================================================
 * Format collected metrics as JSON (compact for stdout / REST API)
 * ============================================================================= */

 // Format the collected metrics as JSON in a compact format for stdout / REST API.

 // Compacted form for concision.
static std::string FormatMetricsJsonCompact(const CpuData& cpu, const std::vector<CpuCoreEntry>& cores,
    const MemoryData& mem, const std::vector<DiskEntry>& disks,
    const std::vector<ProcessMetrics>& processes, const std::string& procTs) {
    std::ostringstream js;
    js << std::fixed << std::setprecision(2);
    js << "{\n";
    js << "  \"cpu\": {\"cpu_mhz\": " << cpu.mhz << ", \"cpu_usage_percent\": " << cpu.usage_percent
       << ", \"system_responsiveness_percent\": " << cpu.responsiveness_percent
       << ", \"timestamp\": \"" << JsonEscape(cpu.timestamp) << "\"},\n";
    js << "  \"cpu_cores\": [";
    for (size_t i = 0; i < cores.size(); i++) {
        const auto& c = cores[i];
        if (i) js << ",";
        js << "{\"core_index\": " << c.index << ", \"core_mhz\": " << c.mhz << ", \"core_usage_percent\": " << c.usage_percent << ", \"timestamp\": \"" << JsonEscape(c.timestamp) << "\"}";
    }
    js << "],\n";
    js << "  \"memory\": {\"total_bytes\": " << mem.total_bytes << ", \"free_bytes\": " << mem.free_bytes << ", \"used_bytes\": " << mem.used_bytes
       << ", \"memory_usage_percent\": " << mem.usage_percent << ", \"page_fault_count\": " << mem.page_fault_count << ", \"timestamp\": \"" << JsonEscape(mem.timestamp) << "\"},\n";
    js << "  \"disk\": [";
    for (size_t i = 0; i < disks.size(); i++) {
        const auto& d = disks[i];
        if (i) js << ",";
        js << "{\"drive_letter\": \"" << JsonEscape(d.drive_letter) << "\", \"total_bytes\": " << d.total_bytes << ", \"free_bytes\": " << d.free_bytes
           << ", \"read_speed_bytes_per_sec\": " << d.read_speed << ", \"write_speed_bytes_per_sec\": " << d.write_speed << ", \"timestamp\": \"" << JsonEscape(d.timestamp) << "\"}";
    }
    js << "],\n";
    js << "  \"processes\": [";
    for (size_t i = 0; i < processes.size(); i++) {
        const auto& p = processes[i];
        if (i) js << ",";
        js << "{\"pid\": " << p.pid << ", \"name\": \"" << JsonEscape(p.name) << "\", \"owner\": \"" << JsonEscape(p.owner) << "\", \"priority\": \"" << JsonEscape(p.priority)
           << "\", \"cpu_percent\": " << p.cpuPercent << ", \"cpu_time_100ns\": " << p.cpuTime100ns
           << ", \"memory_working_set_bytes\": " << p.memoryWorkingSetBytes << ", \"memory_usage_percent\": " << p.memoryPercent
           << ", \"location\": \"" << JsonEscape(p.location)
           << "\", \"timestamp\": \"" << JsonEscape(procTs) << "\"}";
    }
    js << "]\n}\n";
    return js.str();
}

/* =============================================================================
 * Pretty-printed JSON for file (cleaner display for testing)
 * ============================================================================= */
static std::string FormatMetricsJsonPretty(const CpuData& cpu, const std::vector<CpuCoreEntry>& cores,
    const MemoryData& mem, const std::vector<DiskEntry>& disks,
    const std::vector<ProcessMetrics>& processes, const std::string& procTs) {
    std::ostringstream js;
    js << std::fixed << std::setprecision(2);
    js << "{\n";
    js << "  \"cpu\": {\n";
    js << "    \"cpu_mhz\": " << cpu.mhz << ",\n"
       << "    \"cpu_usage_percent\": " << cpu.usage_percent << ",\n"
       << "    \"system_responsiveness_percent\": " << cpu.responsiveness_percent << ",\n"
       << "    \"timestamp\": \"" << JsonEscape(cpu.timestamp) << "\"\n  },\n";
    js << "  \"cpu_cores\": [\n";
    for (size_t i = 0; i < cores.size(); i++) {
        const auto& c = cores[i];
        js << "    {\n      \"core_index\": " << c.index << ",\n      \"core_mhz\": " << c.mhz
           << ",\n      \"core_usage_percent\": " << c.usage_percent << ",\n      \"timestamp\": \"" << JsonEscape(c.timestamp) << "\"\n    }";
        js << (i + 1 < cores.size() ? ",\n" : "\n");
    }
    js << "  ],\n";
    js << "  \"memory\": {\n";
    js << "    \"total_bytes\": " << mem.total_bytes << ",\n    \"free_bytes\": " << mem.free_bytes
       << ",\n    \"used_bytes\": " << mem.used_bytes << ",\n    \"memory_usage_percent\": " << mem.usage_percent
       << ",\n    \"page_fault_count\": " << mem.page_fault_count << ",\n    \"timestamp\": \"" << JsonEscape(mem.timestamp) << "\"\n  },\n";
    js << "  \"disk\": [\n";
    for (size_t i = 0; i < disks.size(); i++) {
        const auto& d = disks[i];
        js << "    {\n      \"drive_letter\": \"" << JsonEscape(d.drive_letter) << "\",\n      \"total_bytes\": " << d.total_bytes
           << ",\n      \"free_bytes\": " << d.free_bytes << ",\n      \"read_speed_bytes_per_sec\": " << d.read_speed
           << ",\n      \"write_speed_bytes_per_sec\": " << d.write_speed << ",\n      \"timestamp\": \"" << JsonEscape(d.timestamp) << "\"\n    }";
        js << (i + 1 < disks.size() ? ",\n" : "\n");
    }
    js << "  ],\n  \"processes\": [\n";
    for (size_t i = 0; i < processes.size(); i++) {
        const auto& p = processes[i];
        js << "    {\n      \"pid\": " << p.pid << ",\n      \"name\": \"" << JsonEscape(p.name)
           << "\",\n      \"owner\": \"" << JsonEscape(p.owner) << "\",\n      \"priority\": \"" << JsonEscape(p.priority)
           << "\",\n      \"cpu_percent\": " << p.cpuPercent << ",\n      \"cpu_time_100ns\": " << p.cpuTime100ns
           << ",\n      \"memory_working_set_bytes\": " << p.memoryWorkingSetBytes << ",\n      \"memory_usage_percent\": " << p.memoryPercent
           << ",\n      \"location\": \"" << JsonEscape(p.location) << "\",\n      \"timestamp\": \"" << JsonEscape(procTs) << "\"\n    }";
        js << (i + 1 < processes.size() ? ",\n" : "\n");
    }
    js << "  ]\n}\n";
    return js.str();
}

// Write everything to the file.
static bool WriteMetricsJsonToFile(const std::string& filepath, const std::string& jsonContent) {
    std::ofstream f(filepath);
    if (!f) return false;
    f << jsonContent;
    return f.good();
}

/* =============================================================================
 * Windows Service: get exe directory, handler, ServiceMain, install/remove
 * ============================================================================= */
 // Implement the Windows Service path to get the executable running as a service
 // This is a security-sensitive operation and should be used with caution.
static std::string GetExeDirectory() {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) return "";
    std::string s(path);
    size_t last = s.find_last_of("\\/");
    if (last != std::string::npos) s.resize(last + 1);
    return s;
}

// Handle the Windows Service control requests.
static VOID WINAPI SvcCtrlHandler(DWORD ctrl) {
    switch (ctrl) {
        // Stop the service.
        case SERVICE_CONTROL_STOP:
            InterlockedExchange(&g_svcStopRequested, 1);
            g_svcStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_svcStatus.dwWaitHint = 5000;
            SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
            return;
        // Do nothing.
        case SERVICE_CONTROL_INTERROGATE:
            break;
        default:
            break;
    }
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
}

// Report the status of the service to the Windows Service Control Manager.
static void ReportSvcStatus(DWORD state, DWORD win32Exit = NO_ERROR, DWORD waitHint = 0) {
    g_svcStatus.dwCurrentState = state;
    g_svcStatus.dwWin32ExitCode = win32Exit;
    g_svcStatus.dwWaitHint = waitHint;
    if (state == SERVICE_RUNNING)
        g_svcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    else if (state == SERVICE_STOPPED)
        g_svcStatus.dwControlsAccepted = 0;
    SetServiceStatus(g_svcStatusHandle, &g_svcStatus);
}

// The main function for the Windows Service.
static VOID WINAPI ServiceMain(DWORD argc, LPSTR* argv) {
    (void)argc;
    (void)argv;
    g_svcStatusHandle = RegisterServiceCtrlHandlerA(SVC_NAME, SvcCtrlHandler);
    if (!g_svcStatusHandle) return;
    g_svcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwServiceSpecificExitCode = 0;
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    PrimeCpuReadings();
    Sleep(500);
    ReportSvcStatus(SERVICE_RUNNING);

    std::string outPath = GetExeDirectory() + "system_metrics_output.json";
    while (InterlockedCompareExchange(&g_svcStopRequested, 0, 0) == 0) {
        CpuData cpu = GetCpuData();
        auto cores = GetCpuCoresData();
        MemoryData mem = GetMemoryData();
        auto disks = GetDiskData();
        GetProcessMetrics();
        Sleep(1000);
        auto processes = GetProcessMetrics();
        std::string procTs = GetTimestamp();
        std::string jsonPretty = FormatMetricsJsonPretty(cpu, cores, mem, disks, processes, procTs);
        WriteMetricsJsonToFile(outPath, jsonPretty);
        Sleep(METRICS_POLL_MS);
    }
    ReportSvcStatus(SERVICE_STOPPED);
}

// Install the Windows Service.
static bool InstallService() {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) return false;
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;
    SC_HANDLE svc = CreateServiceA(scm, SVC_NAME, SVC_DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, exePath, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return (svc != nullptr);
}

// Uninstall the Windows Service.
static bool RemoveService() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, SVC_NAME, DELETE);
    if (!svc) { CloseServiceHandle(scm); return false; }
    bool ok = (DeleteService(svc) != 0);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

/* =============================================================================
 * MAIN
 * ============================================================================= */
int main(int argc, char* argv[]) {
    if (argc >= 2) {
        const char* cmd = argv[1];
        if (_stricmp(cmd, "install") == 0) {
            if (InstallService())
                std::cout << "Service installed. Start with: net start " << SVC_NAME << "\n";
            else
                std::cerr << "Install failed (run as Administrator). Error: " << GetLastError() << "\n";
            return 0;
        }
        if (_stricmp(cmd, "remove") == 0 || _stricmp(cmd, "uninstall") == 0) {
            std::cout << "Stop the service first if running: net stop " << SVC_NAME << "\n";
            if (RemoveService())
                std::cout << "Service removed.\n";
            else
                std::cerr << "Remove failed. Error: " << GetLastError() << "\n";
            return 0;
        }
    }

    /* If started by SCM, dispatch to ServiceMain; otherwise run console one-shot */
    SERVICE_TABLE_ENTRYA dispatch[] = {
        { (LPSTR)SVC_NAME, (LPSERVICE_MAIN_FUNCTIONA)ServiceMain },
        { nullptr, nullptr }
    };
    if (StartServiceCtrlDispatcherA(dispatch)) return 0;
    if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) return 1;

    /* Console mode: one-shot metrics and file output */
    PrimeCpuReadings();
    Sleep(500);

    CpuData cpu = GetCpuData();
    auto cores = GetCpuCoresData();
    MemoryData mem = GetMemoryData();
    auto disks = GetDiskData();
    GetProcessMetrics();
    Sleep(1000);
    auto processes = GetProcessMetrics();
    std::string procTs = GetTimestamp();

    std::string jsonCompact = FormatMetricsJsonCompact(cpu, cores, mem, disks, processes, procTs);
    std::string jsonPretty = FormatMetricsJsonPretty(cpu, cores, mem, disks, processes, procTs);

    std::cout << jsonCompact;

    std::string outFile = "system_metrics_output.json";
    if (WriteMetricsJsonToFile(outFile, jsonPretty))
        std::cerr << "Wrote " << outFile << " (pretty format)\n";
    else
        std::cerr << "Warning: could not write " << outFile << "\n";

    return 0;
}
