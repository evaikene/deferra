#include "logging.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace jb::core;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

struct RecordedField {
    using Value = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

    std::string name;
    Value       value;
};

struct Record {
    LogLevel                              level;
    std::string                           message;
    std::string                           file;
    unsigned int                          line{};
    std::chrono::system_clock::time_point timestamp;
    std::thread::id                       thread_id;
    std::string                           event_name;
    std::vector<RecordedField>            fields;
};

class CaptureLogger : public Logger {
public:
    void log(LogMessage const& msg) override
    {
        // A retaining sink must own each view's contents, including strings inside the field variant.
        Record record{.level      = msg.level,
                      .message    = std::string{msg.message},
                      .file       = msg.location.file_name(),
                      .line       = msg.location.line(),
                      .timestamp  = msg.timestamp,
                      .thread_id  = msg.thread_id,
                      .event_name = std::string{msg.event_name},
                      .fields     = {}};
        for (auto const& field : msg.fields) {
            auto value = std::visit(
                [](auto const& scalar) -> RecordedField::Value {
                    if constexpr (std::is_same_v<std::decay_t<decltype(scalar)>, std::string_view>) {
                        return std::string{scalar};
                    }
                    else {
                        return scalar;
                    }
                },
                field.value);
            record.fields.push_back({.name = std::string{field.name}, .value = std::move(value)});
        }

        std::scoped_lock lock{_mx};
        _records.push_back(std::move(record));
    }

    auto records() const -> std::vector<Record>
    {
        std::scoped_lock lock{_mx};
        return _records;
    }

    void clear()
    {
        std::scoped_lock lock{_mx};
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
    CHECK(recs[0].event_name.empty());
    CHECK(recs[0].fields.empty());
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
// Tests: structured events and borrowed storage
// ---------------------------------------------------------------------------

TEST_CASE("Structured events preserve scalar types and numeric boundaries", "[core][logging]")
{
    LoggerGuard      g;
    std::array const fields{
        LogField{.name = "enabled",  .value = true                                     },
        LogField{.name = "minimum",  .value = std::numeric_limits<std::int64_t>::min() },
        LogField{.name = "maximum",  .value = std::numeric_limits<std::int64_t>::max() },
        LogField{.name = "unsigned", .value = std::numeric_limits<std::uint64_t>::max()},
        LogField{.name = "fraction", .value = 1.25                                     },
        LogField{.name = "text",     .value = std::string_view{"a\0b", 3}              },
    };

    log_event(LogLevel::Info, "operation.complete", fields);

    auto const records = g.cap->records();
    REQUIRE(records.size() == 1);
    auto const& record = records[0];
    CHECK(record.level == LogLevel::Info);
    CHECK(record.event_name == "operation.complete");
    CHECK(record.message == record.event_name);
    REQUIRE(record.fields.size() == fields.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
        CHECK(record.fields[index].name == fields[index].name);
        CHECK(record.fields[index].value.index() == fields[index].value.index());
    }
    CHECK(std::get<bool>(record.fields[0].value));
    CHECK(std::get<std::int64_t>(record.fields[1].value) == std::numeric_limits<std::int64_t>::min());
    CHECK(std::get<std::int64_t>(record.fields[2].value) == std::numeric_limits<std::int64_t>::max());
    CHECK(std::get<std::uint64_t>(record.fields[3].value) == std::numeric_limits<std::uint64_t>::max());
    CHECK(std::get<double>(record.fields[4].value) == 1.25);
    CHECK(std::get<std::string>(record.fields[5].value) == std::string{"a\0b", 3});
}

TEST_CASE("Structured events forward duplicate names and nonfinite values to the sink", "[core][logging]")
{
    LoggerGuard      g;
    // Rendering policy belongs to the sink; the core record retains all supplied values and their order.
    std::array const fields{
        LogField{.name = "same", .value = std::numeric_limits<double>::infinity() },
        LogField{.name = "same", .value = std::numeric_limits<double>::quiet_NaN()},
    };
    log_event(LogLevel::Info, "values.forwarded", fields);

    auto const records = g.cap->records();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].fields.size() == 2);
    CHECK(records[0].fields[0].name == "same");
    CHECK(records[0].fields[1].name == "same");
    CHECK(std::isinf(std::get<double>(records[0].fields[0].value)));
    CHECK(std::isnan(std::get<double>(records[0].fields[1].value)));
}

TEST_CASE("Structured events capture call-site metadata and accept empty fields", "[core][logging]")
{
    LoggerGuard g;
    auto const  before = std::chrono::system_clock::now();
    auto const  line   = std::source_location::current().line() + 1;
    log_event(LogLevel::Info, "empty.event");
    auto const after = std::chrono::system_clock::now();

    auto const explicit_location = std::source_location::current();
    log_event(LogLevel::Warning, "located.event", {}, explicit_location);

    auto const records = g.cap->records();
    REQUIRE(records.size() == 2);
    CHECK(records[0].line == line);
    CHECK(records[0].file == std::source_location::current().file_name());
    CHECK(records[0].timestamp >= before);
    CHECK(records[0].timestamp <= after);
    CHECK(records[0].thread_id == std::this_thread::get_id());
    CHECK(records[0].fields.empty());
    CHECK(records[1].line == explicit_location.line());
    CHECK(records[1].level == LogLevel::Warning);
}

TEST_CASE("Structured event copies survive caller storage mutation and destruction", "[core][logging]")
{
    LoggerGuard g;
    {
        std::string      event_name{"owned.event"};
        std::string      field_name{"label"};
        std::string      value{"original"};
        std::array const fields{
            LogField{.name = field_name, .value = std::string_view{value}}
        };
        log_event(LogLevel::Info, event_name, fields);

        event_name.assign("changed");
        field_name.assign("changed");
        value.assign("changed");
    }

    auto const records = g.cap->records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].event_name == "owned.event");
    CHECK(records[0].message == "owned.event");
    REQUIRE(records[0].fields.size() == 1);
    CHECK(records[0].fields[0].name == "label");
    CHECK(std::get<std::string>(records[0].fields[0].value) == "original");
}

TEST_CASE("Structured events borrow caller storage during the synchronous sink call", "[core][logging]")
{
    struct BorrowLogger final : Logger {
        void log(LogMessage const& record) override
        {
            event_data = record.event_name.data();
            field_data = record.fields.data();
            if (record.fields.size() == 1) {
                name_data  = record.fields[0].name.data();
                value_data = std::get<std::string_view>(record.fields[0].value).data();
            }
        }

        char const*     event_data{};
        LogField const* field_data{};
        char const*     name_data{};
        char const*     value_data{};
    };

    LoggerGuard restore;
    auto        sink = std::make_shared<BorrowLogger>();
    sink->set_level(LogLevel::Info);
    set_logger(sink);

    std::string const event_name{"borrowed.event"};
    std::string const name{"name"};
    std::string const value{"value"};
    std::array const  fields{
        LogField{.name = name, .value = std::string_view{value}}
    };
    log_event(LogLevel::Info, event_name, fields);

    CHECK(sink->event_data == event_name.data());
    CHECK(sink->field_data == fields.data());
    CHECK(sink->name_data == name.data());
    CHECK(sink->value_data == value.data());
}

TEST_CASE("Structured events obey every severity threshold", "[core][logging]")
{
    LoggerGuard      g;
    std::array const levels{LogLevel::Fatal,
                            LogLevel::Error,
                            LogLevel::Warning,
                            LogLevel::Info,
                            LogLevel::Debug1,
                            LogLevel::Debug2,
                            LogLevel::Debug3};

    for (std::size_t threshold = 0; threshold < levels.size(); ++threshold) {
        g.cap->set_level(levels[threshold]);
        g.cap->clear();
        for (auto level : levels) {
            log_event(level, "threshold.checked");
        }

        auto const records = g.cap->records();
        REQUIRE(records.size() == threshold + 1);
        for (std::size_t index = 0; index < records.size(); ++index) {
            CHECK(records[index].level == levels[index]);
        }
    }
}

// ---------------------------------------------------------------------------
// Tests: concurrent logger ownership and admission
// ---------------------------------------------------------------------------

TEST_CASE("An in-flight event retains its sink across logger replacement", "[core][logging]")
{
    struct PausedCall {
        std::latch       entered{1};
        std::latch       release{1};
        std::atomic_bool destroyed{false};
        std::string      event_name;
    } state;

    struct PausedLogger final : Logger {
        explicit PausedLogger(PausedCall& call)
            : state{call}
        {}

        ~PausedLogger() override { state.destroyed.store(true); }

        void log(LogMessage const& record) override
        {
            state.entered.count_down();
            state.release.wait();
            state.event_name = record.event_name;
        }

        PausedCall& state;
    };

    LoggerGuard replacement;
    auto        original = std::make_shared<PausedLogger>(state);
    original->set_level(LogLevel::Info);
    std::weak_ptr<Logger> const weak = original;
    set_logger(original);
    original.reset();

    // The old sink is now owned only by the slot and the admitted call. Replace the slot while the call is paused.
    std::jthread caller{[] { log_event(LogLevel::Info, "original.event"); }};
    state.entered.wait();
    set_logger(replacement.cap);
    bool const retained = !weak.expired() && !state.destroyed.load();
    log_event(LogLevel::Info, "replacement.event");
    state.release.count_down();
    caller.join();

    CHECK(retained);
    CHECK(weak.expired());
    CHECK(state.destroyed.load());
    CHECK(state.event_name == "original.event");
    auto const records = replacement.cap->records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].event_name == "replacement.event");
}

TEST_CASE("Concurrent events remain complete across logger and threshold changes", "[core][logging]")
{
    LoggerGuard first;
    auto const  second = std::make_shared<CaptureLogger>();
    second->set_level(LogLevel::Info);
    constexpr std::size_t                     worker_count{4};
    constexpr std::uint64_t                   events_per_worker{100};
    std::barrier                              start{static_cast<std::ptrdiff_t>(worker_count + 1)};
    std::array<std::thread::id, worker_count> thread_ids;
    std::vector<std::jthread>                 workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            thread_ids[worker] = std::this_thread::get_id();
            start.arrive_and_wait();
            for (std::uint64_t sequence = 0; sequence < events_per_worker; ++sequence) {
                std::array const fields{
                    LogField{.name = "worker",   .value = static_cast<std::uint64_t>(worker)},
                    LogField{.name = "sequence", .value = sequence                          }
                };
                log_event(LogLevel::Error, "concurrent.event", fields);
                log_event(LogLevel::Info, "optional.event", fields);
            }
        });
    }

    // Error remains enabled at every threshold. Info delivery may vary, but every admitted record must be intact.
    start.arrive_and_wait();
    for (std::uint64_t iteration = 0; iteration < events_per_worker; ++iteration) {
        first.cap->set_level(LogLevel::Warning);
        second->set_level(LogLevel::Warning);
        set_logger(second);
        first.cap->set_level(LogLevel::Info);
        second->set_level(LogLevel::Info);
        set_logger(first.cap);
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::array<std::array<bool, events_per_worker>, worker_count> seen{};
    std::size_t                                                   required_count{};
    for (auto const& sink : {first.cap, second}) {
        for (auto const& record : sink->records()) {
            CHECK(record.message == record.event_name);
            REQUIRE(record.fields.size() == 2);
            CHECK(record.fields[0].name == "worker");
            CHECK(record.fields[1].name == "sequence");
            auto const worker   = std::get<std::uint64_t>(record.fields[0].value);
            auto const sequence = std::get<std::uint64_t>(record.fields[1].value);
            REQUIRE(worker < worker_count);
            REQUIRE(sequence < events_per_worker);
            CHECK(record.thread_id == thread_ids[worker]);

            if (record.level == LogLevel::Error) {
                CHECK(record.event_name == "concurrent.event");
                CHECK_FALSE(seen[worker][sequence]);
                seen[worker][sequence] = true;
                ++required_count;
            }
            else {
                CHECK(record.level == LogLevel::Info);
                CHECK(record.event_name == "optional.event");
            }
        }
    }
    CHECK(required_count == worker_count * events_per_worker);
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

TEST_CASE("ConsoleLogger tolerates closed stderr during cleanup", "[core][logging]")
{
    // Restore stderr before Catch reports any failure, including an exception from the logging call.
    struct StderrGuard {
        int saved{-1};

        StderrGuard()
        {
            std::fflush(stderr);
            saved = ::dup(STDERR_FILENO);
            REQUIRE(saved >= 0);
        }

        ~StderrGuard()
        {
            if (::dup2(saved, STDERR_FILENO) < 0) {
                std::terminate();
            }
            ::close(saved);
            std::clearerr(stderr);
        }
    };

    ConsoleLogger console;
    console.set_abort_on_fatal_error(false);
    for (auto level : {LogLevel::Error, LogLevel::Fatal}) {
        LogMessage message;
        message.level     = level;
        message.message   = "cleanup with unavailable stderr";
        message.timestamp = std::chrono::system_clock::now();
        bool write_failed{false};

        CHECK_NOTHROW([&] {
            StderrGuard restore;
            REQUIRE(::close(STDERR_FILENO) == 0);
            console.log(message);
            write_failed = std::ferror(stderr) != 0;
        }());
        CHECK(write_failed);
    }
}
