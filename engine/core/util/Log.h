#pragma once
// Minimal thread-safe logger. Never call from real-time audio callbacks.

#include <filesystem>
#include <string>

namespace dbdeck::log {

void init(const std::filesystem::path& file, bool alsoStderr);
void write(const char* level, const std::string& msg);

inline void info(const std::string& m) { write("INFO", m); }
inline void warn(const std::string& m) { write("WARN", m); }
inline void error(const std::string& m) { write("ERROR", m); }

}  // namespace dbdeck::log
