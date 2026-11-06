#include "pl/runtime/optifine/OptifineProcess.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sched.h>
#include <string>
#include <vector>

#include "pl/runtime/optifine/OptifineDeviceRules.h"

namespace pl::runtime {
namespace {

/** Case-insensitive ASCII equality, without allocating a lowered copy of either side. */
bool EqualsIgnoreCase(const std::string &a, const char *b) {
  std::size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return i == a.size() && b[i] == '\0';
}

/** Reads a whole small file into a string, or empty on failure. */
std::string ReadSmallFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

bool IsAllDigits(const char *value) {
  if (value == nullptr || *value == '\0') return false;
  for (const char *p = value; *p != '\0'; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  }
  return true;
}

} // namespace

std::string ThreadName(pid_t tid) {
  const std::string path = "/proc/self/task/" + std::to_string(tid) + "/comm";
  std::string name = ReadSmallFile(path);
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r' ||
                           name.back() == ' ')) {
    name.pop_back();
  }
  return name;
}

pid_t FindThreadByName(const std::string &needle) {
  if (needle.empty()) return -1;

  DIR *dir = opendir("/proc/self/task");
  if (dir == nullptr) return -1;

  pid_t found = -1;
  struct dirent *entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    if (!IsAllDigits(entry->d_name)) continue;
    const pid_t tid = static_cast<pid_t>(std::atoi(entry->d_name));
    const std::string name = ThreadName(tid);
    // Exact, case-insensitive match. A substring match would make "Render" hit Android's HWUI
    // "RenderThread" and boost the wrong thread (audit item 11).
    if (!name.empty() && EqualsIgnoreCase(name, needle.c_str())) {
      found = tid;
      break;
    }
  }
  closedir(dir);
  return found;
}

bool ThreadNameMatches(pid_t tid, const std::string &expected) {
  if (tid <= 0) return false;
  const std::string name = ThreadName(tid);
  return !name.empty() && EqualsIgnoreCase(name, expected.c_str());
}

bool SetThreadScheduling(pid_t tid, int policy, int priority) {
  sched_param param{};
  param.sched_priority = priority;
  return sched_setscheduler(tid, policy, &param) == 0;
}

bool GetThreadScheduling(pid_t tid, int &policy, int &priority) {
  sched_param param{};
  const int result = sched_getparam(tid, &param);
  if (result != 0) return false;
  policy = sched_getscheduler(tid);
  priority = param.sched_priority;
  return true;
}

int ReadCpuMaxFrequencies(std::uint64_t *out, int maxCores) {
  if (out == nullptr || maxCores <= 0) return 0;
  // Iterate the cores the kernel reports present, not 0..N, so an offline core without a cpufreq
  // directory does not truncate the list (audit item 17). Fall back to a dense 0..N scan when the
  // present list is unavailable.
  std::vector<int> cores;
  const std::string present = ReadSmallFile("/sys/devices/system/cpu/present");
  if (!present.empty()) {
    std::size_t i = 0;
    while (i < present.size()) {
      while (i < present.size() && !std::isdigit(static_cast<unsigned char>(present[i]))) ++i;
      if (i >= present.size()) break;
      int start = 0;
      while (i < present.size() && std::isdigit(static_cast<unsigned char>(present[i]))) {
        start = start * 10 + (present[i] - '0');
        ++i;
      }
      int end = start;
      if (i < present.size() && present[i] == '-') {
        ++i;
        end = 0;
        while (i < present.size() && std::isdigit(static_cast<unsigned char>(present[i]))) {
          end = end * 10 + (present[i] - '0');
          ++i;
        }
      }
      for (int c = start; c <= end; ++c) cores.push_back(c);
    }
  }
  if (cores.empty()) {
    for (int c = 0; c < maxCores; ++c) cores.push_back(c);
  }

  int count = 0;
  for (int core : cores) {
    if (count >= maxCores) break;
    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(core) +
                             "/cpufreq/cpuinfo_max_freq";
    const std::string text = ReadSmallFile(path);
    if (text.empty()) {
      // A core with no cpufreq entry (offline, or a cluster-less kernel) contributes 0, but does not
      // stop the scan of the cores after it.
      out[count++] = 0;
      continue;
    }
    try {
      out[count++] = std::stoull(text);
    } catch (...) {
      out[count++] = 0;
    }
  }
  return count;
}

int ReadPerformanceCoreIndices(int *out, int maxCores) {
  if (out == nullptr || maxCores <= 0) return 0;

  std::uint64_t freq[kOptifineMaxCores] = {};
  const int read = ReadCpuMaxFrequencies(freq, kOptifineMaxCores);
  if (read <= 0) return 0;

  // The big cluster is found by the largest frequency gap, so a tri-cluster's gold cores count
  // (audit item 16). The indices written are those at or above the cluster threshold.
  return SelectBigClusterCoreIndices(freq, read, out, maxCores);
}

bool SetThreadAffinity(pid_t tid, unsigned long long mask) {
  if (mask == 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int core = 0; core < kOptifineMaxCores; ++core) {
    if ((mask & (1ULL << core)) != 0) {
      CPU_SET(core, &set);
    }
  }
  return sched_setaffinity(tid, sizeof(set), &set) == 0;
}

bool CanUseRealtimeScheduling() {
  // The kernel exposes the process's permitted RT priority range in
  // /proc/sys/kernel/sched_rt_runtime_us only indirectly; the practical probe is whether a
  // SCHED_FIFO request is permitted at all, which SetThreadScheduling reports. This helper
  // answers the coarser question the UI needs: is real-time scheduling compiled in and not
  // disabled by the runtime limit.
  const std::string runtime = ReadSmallFile("/proc/sys/kernel/sched_rt_runtime_us");
  if (runtime.empty()) return true; // Not exposed; let the actual syscall decide.
  try {
    return std::stoll(runtime) != -1;
  } catch (...) {
    return true;
  }
}

} // namespace pl::runtime
