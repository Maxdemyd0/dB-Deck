#include "util/Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

namespace dbdeck::log {

namespace {
std::mutex g_mutex;
std::ofstream g_file;
bool g_stderr = true;
}  // namespace

void init(const std::filesystem::path& file, bool alsoStderr) {
    std::lock_guard lock(g_mutex);
    g_stderr = alsoStderr;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Keep one previous log for post-crash diagnostics.
    if (std::filesystem::exists(file, ec)) {
        std::filesystem::path prev = file;
        prev.replace_extension(".prev.log");
        std::filesystem::rename(file, prev, ec);
    }
    g_file.open(file, std::ios::out | std::ios::trunc);
}

void write(const char* level, const std::string& msg) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file << stamp << ' ' << level << ' ' << msg << '\n';
        g_file.flush();
    }
    if (g_stderr) std::cerr << stamp << ' ' << level << ' ' << msg << '\n';
}

}  // namespace dbdeck::log
