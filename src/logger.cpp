#include "logger.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <thread>

// ── Singleton ─────────────────────────────────────────────────────────────────

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::set_level(int level) noexcept {
    m_level.store(std::max(0, std::min(5, level)));
}

// ── Helpers ───────────────────────────────────────────────────────────────────

const char* Logger::level_name(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::ERR:   return "ERROR";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::DBG:   return "DEBUG";
        case LogLevel::TRACE: return "TRACE";
        default:              return "?";
    }
}

std::string Logger::make_timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::ostringstream oss;
    oss << buf << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::make_thread_id() {
    // Assign each thread a short sequential id on first use
    static std::atomic<int> counter{0};
    static thread_local int id = counter++;
    return "t" + std::to_string(id);
}

// ── Core ──────────────────────────────────────────────────────────────────────

void Logger::log(LogLevel level, const std::string& msg) {
    if (!is_enabled(level)) return;

    // Build the line outside the lock to minimise contention
    std::string line;
    line.reserve(128 + msg.size());
    line += '[';
    line += make_timestamp();
    line += "] [";
    line += level_name(level);
    line += "] [";
    line += make_thread_id();
    line += "] ";
    line += msg;
    line += '\n';

    std::lock_guard<std::mutex> lk(m_write_mutex);
    std::cerr << line;
}
