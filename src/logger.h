#pragma once
#include <string>
#include <mutex>
#include <atomic>

enum class LogLevel : int {
    SILENT = 0,
    ERR    = 1,   // ERROR is a macro on some platforms
    WARN   = 2,
    INFO   = 3,
    DBG    = 4,
    TRACE  = 5,
};

// Thread-safe singleton logger.  All output goes to stderr.
// Must not be called while holding CacheLayer's mutex (separate lock order).
class Logger {
public:
    static Logger& instance();

    // thread-safe
    void set_level(int level) noexcept;
    [[nodiscard]] int  get_level() const noexcept { return m_level.load(); }
    [[nodiscard]] bool is_enabled(LogLevel l) const noexcept {
        return static_cast<int>(l) <= m_level.load();
    }

    // thread-safe; acquires m_mutex only for the final write
    void log(LogLevel level, const std::string& msg);

    void error(const std::string& m) { log(LogLevel::ERR,   m); }
    void warn (const std::string& m) { log(LogLevel::WARN,  m); }
    void info (const std::string& m) { log(LogLevel::INFO,  m); }
    void debug(const std::string& m) { log(LogLevel::DBG,   m); }
    void trace(const std::string& m) { log(LogLevel::TRACE, m); }

private:
    Logger() = default;

    std::atomic<int> m_level{1};
    std::mutex       m_write_mutex;

    static const char* level_name(LogLevel l) noexcept;
    static std::string make_timestamp();
    static std::string make_thread_id();
};
