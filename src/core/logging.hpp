/// jb::core logging system
///
/// Provides a small, expandable logging facade. The core uses public `log*` functions
/// defined here and trusts that *some* `Logger` is installed. A no-op-free default
/// `ConsoleLogger` writing to `stderr` is created lazily on first use, so logging
/// works even before `main()` / `Application` setup.
///
/// Applications are expected to install their own `Logger` via `set_logger()` to
/// add coloring, multiple sinks, log files, syslog, async dispatch, etc.

#pragma once

#include "thread_context.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string_view>
#include <type_traits>

namespace jb::core {

/// Severity levels, ordered from most to least severe.
enum class LogLevel : std::uint8_t {
    Fatal = 0,
    Error,
    Warning,
    Info,
    Debug1,
    Debug2,
    Debug3,
};

/// Short uppercase name of a log level (e.g. "INFO").
constexpr auto log_level_name(LogLevel level) noexcept -> std::string_view
{
    switch (level) {
        case LogLevel::Fatal:
            return "FATAL";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Warning:
            return "WARN";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Debug1:
            return "DBG1";
        case LogLevel::Debug2:
            return "DBG2";
        case LogLevel::Debug3:
            return "DBG3";
        default:
            return "?";
    }
}

/// Single log record passed to `Logger::log()`. The `message` view points at a
/// string owned by the caller for the duration of the `log()` call only -
/// implementations that need to keep it (async sinks) must make a copy.
struct LogMessage {
    LogLevel                              level{LogLevel::Fatal};
    std::string_view                      message;
    std::source_location                  location;
    std::chrono::system_clock::time_point timestamp;
    ThreadCtx::id_t                       thread_id;
};

/// Abstract logger instance
///
/// Implementations are responsible for output (console, file, syslog, ...) and
/// MUST be thread-safe - `log()` may be called concurrently from any thread
/// without external synchronization.
///
/// The `abort_on_fatal_error` flag controls whether the process is aborted after
/// logging a fatal message. This is false by default in release builds and true in
/// debug builds, so that fatal messages are logged without aborting in release builds,
/// but still abort in debug builds.
class Logger {
public:

    virtual ~Logger() = default;

    /// Emit a log record.
    /// @param[in] msg Log record to emit
    ///
    /// This method is only called when `is_enabled(msg.level)` is true.
    virtual void log(LogMessage const& msg) = 0;

    /// Minimum log level this logger will emit. Messages strictly less severe
    /// (numerically greater) are dropped before formatting.
    auto level() const noexcept -> LogLevel { return _level; }

    void set_level(LogLevel l) noexcept { _level.store(l, std::memory_order_relaxed); }

    auto is_enabled(LogLevel l) const noexcept
    {
        return static_cast<std::uint8_t>(l) <= static_cast<std::uint8_t>(_level.load(std::memory_order_relaxed));
    }

    /// Changes the behavior of `log()` on fatal messages. If true, the process is
    /// aborted after logging a fatal message. Default is false in release builds and
    /// true in debug builds.
    void set_abort_on_fatal_error(bool v) noexcept { _abort_on_fatal_error = v; }

protected:
    std::atomic<LogLevel> _level = LogLevel::Warning;
#if defined(NDEBUG)
    bool _abort_on_fatal_error{false};
#else
    bool _abort_on_fatal_error{true};
#endif
};

/// Plain line-based logger writing to stderr. Thread-safe via an internal mutex.
///
/// This is the default logger used if no other logger is installed, so it is always
/// available and can be used for early logging before `main()` / `Application` setup.
///
class ConsoleLogger final : public Logger {
public:

    ConsoleLogger()           = default;
    ~ConsoleLogger() override = default;

    void log(LogMessage const& msg) override;

};

/// Returns the currently installed global logger (never nullptr).
/// If nothing is installed, a default ConsoleLogger is created lazily.
auto logger() -> std::shared_ptr<Logger>;

/// Replaces the global logger. Pass nullptr to restore the default
/// ConsoleLogger. Safe to call concurrently with log() / logger().
void set_logger(std::shared_ptr<Logger> logger);

//--- Internals: format-string + source-location capture at the call site

namespace priv {

/// Pairs a {fmt} format string with the source location of the call site.
/// The constructor is consteval so the format string is validated against
/// Args... at compile time.
template <typename... Args>
struct FormatLoc {
    fmt::format_string<Args...> format;
    std::source_location        loc;

    template <typename S>
    consteval FormatLoc(S const& s, std::source_location l = std::source_location::current())
        : format(s)
        , loc(l)
    {}
};

template <typename... Args>
inline void emit(LogLevel level, FormatLoc<std::type_identity_t<Args>...> const& fl, Args&&... args)
{
    auto lg = logger();
    if (!lg || !lg->is_enabled(level)) {
        return;
    }

    // format only when the level is enabled.
    auto       text = fmt::format(fl.format, std::forward<Args>(args)...);
    LogMessage msg{.level     = level,
                   .message   = text,
                   .location  = std::move(fl.loc),
                   .timestamp = std::chrono::system_clock::now(),
                   .thread_id = ThreadCtx::current()->id()};
    lg->log(msg);
}

} // namespace priv

//--- Public log functions
//
// Usage:
//  log_info("connected to {}:{}", host, port);
//  log_error("failed to open {}: {}", path, err);

template <typename... Args>
inline void log_fatal(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Fatal, fl, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_error(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Error, fl, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_warning(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Warning, fl, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_info(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Info, fl, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_dbg1(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Debug1, fl, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_dbg2(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Debug2, fl, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_dbg3(priv::FormatLoc<std::type_identity_t<Args>...> fl, Args&&... args)
{
    priv::emit(LogLevel::Debug3, fl, std::forward<Args>(args)...);
}

} // namespace jb::core
