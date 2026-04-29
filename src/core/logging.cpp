#include "logging.hpp"

#include <fmt/chrono.h>

#include <cstdio>
#include <ctime>
#include <mutex>

namespace jb::core {

namespace {

auto basename(std::string_view path) noexcept -> std::string_view
{
    auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

inline auto localtime(time_t secs, std::tm& tm) -> bool
{
    return localtime_r(&secs, &tm) != nullptr;
}

// serializes writes to stderr so concurrent log lines do not interleave
auto console_mutex() -> std::mutex&
{
    static std::mutex mx;
    return mx;
}

// logger slot mutex
auto slot_mutex() -> std::mutex&
{
    static std::mutex mx;
    return mx;
}

// currently installed logger (may be null until the first call to logger())
auto slot() -> std::shared_ptr<Logger>&
{
    static std::shared_ptr<Logger> s;
    return s;
}

// lazily created default logger
auto default_logger() -> std::shared_ptr<Logger>
{
    static auto def = std::make_shared<ConsoleLogger>();
    return def;
}

} // anonymous namespace

void ConsoleLogger::log(LogMessage const& msg)
{
    using namespace std::chrono;

    auto const secs = system_clock::to_time_t(msg.timestamp);
    auto const frac = msg.timestamp - system_clock::from_time_t(secs);
    auto const ms   = duration_cast<milliseconds>(frac).count();
    std::tm    tm{};
    localtime(secs, tm);

    std::lock_guard lock{console_mutex()};
    fmt::print(stderr,
               "{:%Y-%m-%d %H:%M:%S}.{:03} [{:<5}] {}:{} - {}\n",
               tm,
               ms,
               log_level_name(msg.level),
               basename(msg.location.file_name()),
               msg.location.line(),
               msg.message);
    std::fflush(stderr);
}

auto logger() -> std::shared_ptr<Logger>
{
    std::lock_guard lock{slot_mutex()};
    if (!slot()) {
        slot() = default_logger();
    }

    return slot();
}

void set_logger(std::shared_ptr<Logger> logger)
{
    if (!logger) {
        logger = default_logger();
    }
    std::lock_guard lock{slot_mutex()};
    slot() = std::move(logger);
}

} // namespace jb::core
