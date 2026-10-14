#pragma once

#include "GpuTiming.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <unistd.h>
#include <sys/resource.h>

// Per-flip frame-time breakdown (part of ANYPS5_METAL_GPU_TIMING, any nonzero value).
//
// Every command buffer from MetalDevice::CommandBuffer reports its GPU interval (GPUStartTime,
// GPUEndTime, host seconds) with the kind and tag of the work that created it; every CPU wait on
// the GPU (timeline drains, synchronous waits) reports its host interval and why it waited. At
// each flip the frame before the previous one (so every completion handler of it has run) is cut
// into: GPU busy (union of command-buffer intervals), GPU idle, CPU blocked on the GPU, and the
// part of that blocked time the GPU sat idle too (round trips, not GPU work). Every 32 frames one
// "frame_profile" JSON line with per-frame averages goes to stderr:
//   wall/busy/idle/wait/idle_in_wait ms, driver-thread and process CPU ms, command buffers,
//   GPU ms and count per kind, wait ms and count per cause, and the 8 costliest tags.
// Wait causes: below 0x100 the PM4 opcode that drained; 0x100 draw decode, 0x101 dispatch decode,
// 0x102 CPU read of guest memory the GPU may have written, 0x105 end of submission, 0x106 flip,
// 0x107 end of command list, 0x108 predication, 0x10d mapping change, 0x10f other drain,
// 0x200 + kind a synchronous wait inside that kind of work (MetalDevice::Wait).
namespace AgcDriver::Metal::FrameProfile {

inline bool Enabled() { return GpuTiming::Enabled(); }

inline double Now() {
    static const double scale = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return static_cast<double>(info.numer) / static_cast<double>(info.denom) / 1e9;
    }();
    return static_cast<double>(mach_absolute_time()) * scale;
}

struct Interval {
    double start;
    double end;
};

class Profile {
public:
    void Completed(GpuTiming::Kind kind, std::uint64_t tag, double start, double end) {
        if (!(end > start) || start <= 0) return;
        std::lock_guard lock(mutex);
        cbs.push_back({start, end, kind, tag});
    }

    void Waited(std::uint32_t cause, double start, double end) {
        if (!(end > start)) return;
        std::lock_guard lock(mutex);
        waits.push_back({start, end, cause});
    }

    // Called by the driver thread at each flip, after the work before it completed.
    void Flip() {
        const double now = Now();
        timespec cpu{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        const double driverCpu = static_cast<double>(cpu.tv_sec) + static_cast<double>(cpu.tv_nsec) / 1e9;
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        const double processCpu = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
            static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
        auto threads = ThreadTimes();
        std::lock_guard lock(mutex);
        threadSnapshots.push_back(std::move(threads));
        if (threadSnapshots.size() > 3) threadSnapshots.erase(threadSnapshots.begin());
        // The frame [flips[0], flips[1]] is cut once flip 2 arrives: its handlers have all run.
        flips.push_back({now, driverCpu, processCpu});
        if (flips.size() > 3) flips.erase(flips.begin());
        if (flips.size() == 3) {
            for (const auto& [id, entry] : threadSnapshots[1]) {
                const auto before = threadSnapshots[0].find(id);
                const double ms = (entry.second - (before != threadSnapshots[0].end() ? before->second.second : 0)) * 1000.0;
                if (ms <= 0) continue;
                auto& total = threadMs[entry.first.empty() ? "tid" + std::to_string(id) : entry.first + "/" + std::to_string(id % 100000)];
                total += ms;
            }
            Cut(flips[0], flips[1]);
        }
    }

private:
    // CPU seconds of every thread of this process, by thread id, with its name.
    static std::map<std::uint64_t, std::pair<std::string, double>> ThreadTimes() {
        std::map<std::uint64_t, std::pair<std::string, double>> result;
        thread_act_array_t list = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return result;
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            thread_basic_info_data_t basic{};
            mach_msg_type_number_t size = THREAD_BASIC_INFO_COUNT;
            thread_identifier_info_data_t ident{};
            mach_msg_type_number_t identSize = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(list[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &size) == KERN_SUCCESS &&
                thread_info(list[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &identSize) == KERN_SUCCESS) {
                proc_threadinfo info{};
                std::string name;
                if (proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, ident.thread_id, &info, sizeof info) == sizeof info) name = info.pth_name;
                result[ident.thread_id] = {name, basic.user_time.seconds + basic.system_time.seconds +
                    (basic.user_time.microseconds + basic.system_time.microseconds) / 1e6};
            }
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), count * sizeof(thread_act_t));
        return result;
    }

    struct Cb {
        double start;
        double end;
        GpuTiming::Kind kind;
        std::uint64_t tag;
    };
    struct Wait {
        double start;
        double end;
        std::uint32_t cause;
    };
    struct Mark {
        double time;
        double driverCpu;
        double processCpu;
    };
    struct Sum {
        std::uint64_t count = 0;
        double ms = 0;
    };

    static std::vector<Interval> Union(std::vector<Interval> parts) {
        std::sort(parts.begin(), parts.end(), [](const Interval& a, const Interval& b) { return a.start < b.start; });
        std::vector<Interval> merged;
        for (const auto& part : parts) {
            if (!merged.empty() && part.start <= merged.back().end) merged.back().end = std::max(merged.back().end, part.end);
            else merged.push_back(part);
        }
        return merged;
    }
    static double Length(const std::vector<Interval>& parts) {
        double total = 0;
        for (const auto& part : parts) total += part.end - part.start;
        return total;
    }
    static double Overlap(const std::vector<Interval>& a, const std::vector<Interval>& b) {
        double total = 0;
        std::size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            const double from = std::max(a[i].start, b[j].start), to = std::min(a[i].end, b[j].end);
            if (to > from) total += to - from;
            if (a[i].end < b[j].end) ++i;
            else ++j;
        }
        return total;
    }

    // Called with the mutex held.
    void Cut(const Mark& begin, const Mark& end) {
        const double from = begin.time, to = end.time;
        std::vector<Interval> busyParts, waitParts;
        std::vector<Cb> keepCbs;
        for (const auto& cb : cbs) {
            const double s = std::max(cb.start, from), e = std::min(cb.end, to);
            if (e > s) {
                busyParts.push_back({s, e});
                auto& kind = kinds[static_cast<std::size_t>(cb.kind)];
                ++kind.count;
                kind.ms += (e - s) * 1000.0;
                auto& tag = tags[cb.tag != 0 ? cb.tag : GpuTiming::MakeTag(cb.kind, 0)];
                ++tag.count;
                tag.ms += (e - s) * 1000.0;
                ++cbCount;
            }
            if (cb.end > to) keepCbs.push_back(cb);
        }
        cbs = std::move(keepCbs);
        std::vector<Wait> keepWaits;
        for (const auto& wait : waits) {
            const double s = std::max(wait.start, from), e = std::min(wait.end, to);
            if (e > s) {
                waitParts.push_back({s, e});
                auto& cause = causes[wait.cause];
                ++cause.count;
                cause.ms += (e - s) * 1000.0;
            }
            if (wait.end > to) keepWaits.push_back(wait);
        }
        waits = std::move(keepWaits);
        const auto busy = Union(std::move(busyParts));
        const auto blocked = Union(std::move(waitParts));
        const double busyS = Length(busy), waitS = Length(blocked);
        wallMs += (to - from) * 1000.0;
        busyMs += busyS * 1000.0;
        waitMs += waitS * 1000.0;
        idleInWaitMs += (waitS - Overlap(busy, blocked)) * 1000.0;
        driverCpuMs += (end.driverCpu - begin.driverCpu) * 1000.0;
        processCpuMs += (end.processCpu - begin.processCpu) * 1000.0;
        for (std::size_t i = 1; i < busy.size(); ++i) {
            if (busy[i].start - busy[i - 1].end > 0.0005) ++gapsOverHalfMs;
        }
        if (++frames < 32) return;
        Print();
    }

    void Print() {
        const double n = static_cast<double>(frames);
        char head[512];
        std::snprintf(head, sizeof head,
            "{\"schema_version\":1,\"event\":\"frame_profile\",\"frames\":%llu,\"wall_ms\":%.1f,\"gpu_busy_ms\":%.1f,"
            "\"gpu_idle_ms\":%.1f,\"cpu_wait_ms\":%.1f,\"gpu_idle_in_wait_ms\":%.1f,\"driver_cpu_ms\":%.1f,\"process_cpu_ms\":%.1f,"
            "\"cbs\":%.1f,\"gaps_over_0_5ms\":%.1f",
            static_cast<unsigned long long>(frames), wallMs / n, busyMs / n, (wallMs - busyMs) / n, waitMs / n, idleInWaitMs / n,
            driverCpuMs / n, processCpuMs / n, static_cast<double>(cbCount) / n, static_cast<double>(gapsOverHalfMs) / n);
        std::string line = head;
        static constexpr const char* names[] = {"other", "dispatch", "draw", "readback", "upload"};
        line += ",\"kinds\":{";
        char item[128];
        bool first = true;
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            if (kinds[i].count == 0) continue;
            std::snprintf(item, sizeof item, "%s\"%s\":[%.1f,%.2f]", first ? "" : ",", names[i],
                static_cast<double>(kinds[i].count) / n, kinds[i].ms / n);
            line += item;
            first = false;
        }
        line += "},\"waits\":{";
        first = true;
        for (const auto& [cause, sum] : causes) {
            std::snprintf(item, sizeof item, "%s\"%x\":[%.1f,%.2f]", first ? "" : ",", cause, static_cast<double>(sum.count) / n, sum.ms / n);
            line += item;
            first = false;
        }
        line += "},\"top\":[";
        std::vector<std::pair<std::uint64_t, Sum>> sorted(tags.begin(), tags.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        for (std::size_t i = 0; i < sorted.size() && i < 8; ++i) {
            const auto kind = static_cast<std::size_t>(sorted[i].first >> 60u);
            std::snprintf(item, sizeof item, "%s[\"%s\",\"0x%llx\",%.1f,%.2f]", i ? "," : "", kind < 5 ? names[kind] : "?",
                static_cast<unsigned long long>(sorted[i].first & ((std::uint64_t{1} << 60u) - 1)),
                static_cast<double>(sorted[i].second.count) / n, sorted[i].second.ms / n);
            line += item;
        }
        line += "],\"threads\":[";
        std::vector<std::pair<std::string, double>> busiest(threadMs.begin(), threadMs.end());
        std::sort(busiest.begin(), busiest.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (std::size_t i = 0; i < busiest.size() && i < 6; ++i) {
            std::string name;
            for (char c : busiest[i].first) name += (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) ? '_' : c;
            std::snprintf(item, sizeof item, "%s[\"%.48s\",%.1f]", i ? "," : "", name.c_str(), busiest[i].second / n);
            line += item;
        }
        threadMs.clear();
        std::fprintf(stderr, "%s]}\n", line.c_str());
        frames = 0;
        wallMs = busyMs = waitMs = idleInWaitMs = driverCpuMs = processCpuMs = 0;
        cbCount = gapsOverHalfMs = 0;
        kinds = {};
        causes.clear();
        tags.clear();
    }

    std::mutex mutex;
    std::vector<Cb> cbs;
    std::vector<Wait> waits;
    std::vector<Mark> flips;
    std::vector<std::map<std::uint64_t, std::pair<std::string, double>>> threadSnapshots;
    std::map<std::string, double> threadMs;
    std::uint64_t frames = 0, cbCount = 0, gapsOverHalfMs = 0;
    double wallMs = 0, busyMs = 0, waitMs = 0, idleInWaitMs = 0, driverCpuMs = 0, processCpuMs = 0;
    std::array<Sum, static_cast<std::size_t>(GpuTiming::Kind::Count)> kinds{};
    std::map<std::uint32_t, Sum> causes;
    std::map<std::uint64_t, Sum> tags;
};

inline Profile& Shared() {
    static Profile* profile = new Profile;  // never destroyed: Metal threads may report at exit
    return *profile;
}

}
