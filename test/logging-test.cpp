#include "logging.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace jb::core;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct Record {
    LogLevel     level;
    std::string  message;
    std::string  file;
    unsigned int line{};
};

class CaptureLogger : public Logger {
public:
    void log(LogMessage const& msg) override
    {
        std::lock_guard lock{_mx};
        _records.push_back({
            .level   = msg.level,
            .message = std::string{msg.message},
            .file    = msg.location.file_name(),
            .line    = msg.location.line(),
        });
    }

    auto records() const -> std::vector<Record>
    {
        std::lock_guard lock{_mx};
        return _records;
    }

    void clear()
    {
        std::lock_guard lock{_mx};
        _records.clear();
    }

private:
    mutable std::mutex  _mx;
    std::vector<Record> _records;
};

// RAII helper: installs a capture logger and restores the default on destruction.
struct LoggerGuard {
    std::shared_ptr<CaptureLogger> cap;

    explicit LoggerGuard(LogLevel level = LogLevel::Debug3)
        : cap{std::make_shared<CaptureLogger>()}
    {
        cap->set_level(level);
        set_logger(cap);
    }

    ~LoggerGuard() { set_logger(nullptr); }
};

// ---------------------------------------------------------------------------
// Tests: log_level_name
// ---------------------------------------------------------------------------

TEST_CASE("log_level_name returns correct strings", "[core][logging]")
{
    // clang-format off
    CHECK(log_level_name(LogLevel::Fatal)   == "FATAL");
    CHECK(log_level_name(LogLevel::Error)   == "ERROR");
    CHECK(log_level_name(LogLevel::Warning) == "WARN");
    CHECK(log_level_name(LogLevel::Info)    == "INFO");
    CHECK(log_level_name(LogLevel::Debug1)  == "DBG1");
    CHECK(log_level_name(LogLevel::Debug2)  == "DBG2");
    CHECK(log_level_name(LogLevel::Debug3)  == "DBG3");
    // clang-format on
}

// ---------------------------------------------------------------------------
// Tests: Logger base (is_enabled / set_level)
// ---------------------------------------------------------------------------

TEST_CASE("Logger default level is Warning", "[core][logging]")
{
    CaptureLogger lg;
    CHECK(lg.level() == LogLevel::Warning);
}

TEST_CASE("Logger is_enabled respects threshold", "[core][logging]")
{
    CaptureLogger lg;
    lg.set_level(LogLevel::Info);

    CHECK(lg.is_enabled(LogLevel::Fatal));
    CHECK(lg.is_enabled(LogLevel::Error));
    CHECK(lg.is_enabled(LogLevel::Warning));
    CHECK(lg.is_enabled(LogLevel::Info));
    CHECK_FALSE(lg.is_enabled(LogLevel::Debug1));
    CHECK_FALSE(lg.is_enabled(LogLevel::Debug2));
    CHECK_FALSE(lg.is_enabled(LogLevel::Debug3));
}

TEST_CASE("Logger set_level updates threshold", "[core][logging]")
{
    CaptureLogger lg;
    lg.set_level(LogLevel::Debug3);
    CHECK(lg.level() == LogLevel::Debug3);
    CHECK(lg.is_enabled(LogLevel::Debug3));

    lg.set_level(LogLevel::Fatal);
    CHECK(lg.level() == LogLevel::Fatal);
    CHECK_FALSE(lg.is_enabled(LogLevel::Error));
}

// ---------------------------------------------------------------------------
// Tests: global logger slot
// ---------------------------------------------------------------------------

TEST_CASE("logger() returns non-null", "[core][logging]")
{
    CHECK(logger() != nullptr);
}

TEST_CASE("set_logger installs a custom logger", "[core][logging]")
{
    LoggerGuard g;
    CHECK(logger() == g.cap);
}

TEST_CASE("set_logger nullptr restores the default ConsoleLogger", "[core][logging]")
{
    {
        LoggerGuard g;
        CHECK(logger() == g.cap);
    }
    // destructor called set_logger(nullptr)
    auto lg = logger();
    CHECK(lg != nullptr);
    CHECK(dynamic_cast<ConsoleLogger*>(lg.get()) != nullptr);
}

// ---------------------------------------------------------------------------
// Tests: log functions — level routing
// ---------------------------------------------------------------------------

TEST_CASE("Log functions route to the correct level", "[core][logging]")
{
    LoggerGuard g;

    log_fatal("f");
    log_error("e");
    log_warning("w");
    log_info("i");
    log_dbg1("d1");
    log_dbg2("d2");
    log_dbg3("d3");

    auto recs = g.cap->records();
    CHECK(recs.size() == 7);
    CHECK(recs[0].level == LogLevel::Fatal);
    CHECK(recs[1].level == LogLevel::Error);
    CHECK(recs[2].level == LogLevel::Warning);
    CHECK(recs[3].level == LogLevel::Info);
    CHECK(recs[4].level == LogLevel::Debug1);
    CHECK(recs[5].level == LogLevel::Debug2);
    CHECK(recs[6].level == LogLevel::Debug3);
}

TEST_CASE("Log functions below threshold are suppressed", "[core][logging]")
{
    LoggerGuard g{LogLevel::Warning};

    log_info("suppressed");
    log_dbg1("suppressed");
    log_dbg2("suppressed");
    log_dbg3("suppressed");
    CHECK(g.cap->records().empty());

    log_warning("visible");
    log_error("visible");
    log_fatal("visible");
    CHECK(g.cap->records().size() == 3);
}

// ---------------------------------------------------------------------------
// Tests: message formatting
// ---------------------------------------------------------------------------

TEST_CASE("Log functions format the message correctly", "[core][logging]")
{
    LoggerGuard g;

    log_info("value={} str={}", 42, "hello");

    auto recs = g.cap->records();
    CHECK(recs.size() == 1);
    CHECK(recs[0].message == "value=42 str=hello");
}

TEST_CASE("Log functions capture source location", "[core][logging]")
{
    LoggerGuard g;

    log_info("location test");

    auto recs = g.cap->records();
    CHECK_FALSE(recs.empty());
    CHECK(recs[0].line != 0);
    CHECK(std::string_view{recs[0].file}.find("logging-test") != std::string_view::npos);
}

TEST_CASE("Log message timestamp is set", "[core][logging]")
{
    using namespace std::chrono;

    struct TsLogger : Logger {
        system_clock::time_point ts;

        void log(LogMessage const& msg) override { ts = msg.timestamp; }
    };

    auto tsl = std::make_shared<TsLogger>();
    tsl->set_level(LogLevel::Debug3);
    set_logger(tsl);

    auto before = system_clock::now();
    log_info("ts");
    auto after = system_clock::now();

    set_logger(nullptr);

    CHECK(tsl->ts >= before);
    CHECK(tsl->ts <= after);
}

// ---------------------------------------------------------------------------
// Tests: ConsoleLogger smoke test
// ---------------------------------------------------------------------------

TEST_CASE("ConsoleLogger logs without throwing", "[core][logging]")
{
    ConsoleLogger cl;
    cl.set_level(LogLevel::Debug3);

    LogMessage msg;
    msg.level     = LogLevel::Info;
    msg.message   = "smoke test";
    msg.timestamp = std::chrono::system_clock::now();

    CHECK_NOTHROW(cl.log(msg));
}
