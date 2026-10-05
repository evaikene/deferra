///
/// @file logging.hpp
/// @brief Provides the `jb::core` logging facade and logger interfaces.
///
/// The public `log_*` functions format messages with {fmt}, capture the source
/// location at the call site, and forward enabled records to the global
/// `Logger`. A `ConsoleLogger` writing line-oriented output to `stderr` is
/// created lazily on first use, so logging works before `main()` or
/// `Application` setup. Applications can install a custom logger with
/// `set_logger()` for additional sinks, formatting, or asynchronous delivery.
///
/// Set a logger's threshold to control which messages are emitted. Levels are
/// ordered from most to least severe, so a logger configured for `Info` also
/// accepts `Fatal`, `Error`, and `Warning` messages:
///
/// \code{.cpp}
/// jb::core::logger()->set_level(jb::core::LogLevel::Info);
/// jb::core::log_info("connected to {}:{}", host, port);
/// jb::core::log_error("failed to open {}: {}", path, error);
/// \endcode
///
/// Implement custom loggers by overriding `Logger::log()`. Implementations must
/// be thread-safe because logging may occur concurrently from any thread.
/// All text views and structured fields in `LogMessage` are valid only for
/// the duration of the call, so asynchronous loggers must copy them before
/// returning:
///
/// \code{.cpp}
/// class CaptureLogger final : public jb::core::Logger {
/// public:
///     void log(jb::core::LogMessage const& message) override
///     {
///         // Consume message immediately, or copy message.message for later use.
///     }
/// };
///
/// jb::core::set_logger(std::make_shared<CaptureLogger>());
/// \endcode
///
/// Fatal messages are logged before the process is aborted when
/// `abort_on_fatal_error` is enabled. It defaults to `false` in release builds
/// and `true` in debug builds; use `set_abort_on_fatal_error(false)` when a
/// test or recovery path must continue after a fatal log.
///

#pragma once

#include "thread_context.hpp"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

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

/// Named scalar value borrowed by a synchronous log record.
/// Names and string values must remain valid until `Logger::log()` returns.
/// Sinks retaining fields must copy both names and string values into owned storage.
/// Values are forwarded unchanged; escaping, duplicate names and nonfinite
/// number handling belong to the sink's output format.
struct LogField {
    using Value = std::variant<bool, std::int64_t, std::uint64_t, double, std::string_view>;

    std::string_view name;
    Value            value;
};

/// Single log record passed to `Logger::log()`.
/// Message/event text, the field span, field names and string field values
/// are borrowed only for the synchronous call. Sinks retaining a record must
/// copy all borrowed data, not merely the LogMessage or its span.
/// Formatted logs have an empty event name and no fields.
struct LogMessage {
    LogLevel                              level{LogLevel::Fatal};
    std::string_view                      message;
    std::source_location                  location;
    std::chrono::system_clock::time_point timestamp;
    ThreadCtx::id_t                       thread_id;
    std::string_view                      event_name;
    std::span<LogField const>             fields;
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
    /// The facade checks `is_enabled(msg.level)` before calling this method.
    /// Direct sink calls bypass that check. Borrowed record data must be
    /// consumed or copied before returning.
    virtual void log(LogMessage const& msg) = 0;

    /// Minimum log level this logger will emit. Messages strictly less severe
    /// (numerically greater) are dropped before formatting.
    auto level() const noexcept -> LogLevel { return _level; }

    /// Change the admission threshold. Safe concurrently with facade logging;
    /// a record already admitted may still be delivered after this returns.
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
#ifdef NDEBUG
    bool _abort_on_fatal_error{false};
#else
    bool _abort_on_fatal_error{true};
#endif
};

/// Plain line-based logger writing to stderr. Thread-safe via an internal mutex.
/// Write and flush errors discard output without throwing; fatal-abort behavior still applies.
/// Prints the message text only; structured fields are available to custom sinks.
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

/// Emit an enabled structured event through one snapshot of the global logger.
/// @param level Event severity, using the same admission rule as formatted logs.
/// @param event_name Stable event identifier; also supplied as `message` for legacy sinks.
/// @param fields Borrowed named scalar values; an empty span is permitted.
/// @param location Source location, captured at the call site by default.
///
/// The event name and all field storage must remain valid until this call
/// returns, and must not be modified while the sink consumes them. Retaining
/// sinks must copy all borrowed data. Disabled events skip metadata collection
/// and field processing; caller argument evaluation still occurs.
/// The sink is called without the global logger-slot lock. Logger replacement
/// or threshold changes do not cancel a record already admitted to that sink.
void log_event(LogLevel                  level,
               std::string_view          event_name,
               std::span<LogField const> fields   = {},
               std::source_location      location = std::source_location::current());

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
