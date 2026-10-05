#include "daemon_logger_priv.hpp"

#include "json.hpp"
#include "logging.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/operational_log_capture.hpp"
#include "text_validation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace jb::core;
using namespace jb::jobud::detail;

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
};

/// Capture the actual stderr write path in an anonymous file, avoiding pipe-capacity deadlocks.
/// The descriptor is restored before Catch reports assertions or an unwound failure.
class StderrCapture final {
public:
    StderrCapture()
        : _file{std::tmpfile()}
    {
        REQUIRE(_file);
        std::fflush(stderr);
        _saved = ::dup(STDERR_FILENO);
        REQUIRE(_saved >= 0);
        REQUIRE(::dup2(::fileno(_file.get()), STDERR_FILENO) >= 0);
    }

    ~StderrCapture() { restore(); }

    void restore()
    {
        if (_saved >= 0) {
            std::fflush(stderr);
            if (::dup2(_saved, STDERR_FILENO) < 0) {
                std::terminate();
            }
            ::close(_saved);
            _saved = -1;
            std::clearerr(stderr);
        }
    }

    auto text() -> std::string
    {
        restore();
        REQUIRE(std::fseek(_file.get(), 0, SEEK_SET) == 0);
        std::string            result;
        std::array<char, 4096> buffer{};
        while (auto count = std::fread(buffer.data(), 1, buffer.size(), _file.get())) {
            result.append(buffer.data(), count);
        }
        return result;
    }

private:
    std::unique_ptr<std::FILE, FileCloser> _file;
    int                                    _saved{-1};
};

auto json_lines(std::string_view text, LoggingFormat format = LoggingFormat::Json) -> std::vector<JsonValue>
{
    std::vector<JsonValue> records;
    while (!text.empty()) {
        auto const end = text.find('\n');
        REQUIRE(end != std::string_view::npos);
        auto line = text.substr(0, end);
        if (format == LoggingFormat::Text) {
            auto const fields = line.find(" fields=");
            REQUIRE(fields != std::string_view::npos);
            line.remove_prefix(fields + 8);
        }
        auto record = parse_json(line);
        REQUIRE(record);
        REQUIRE(record->is_object());
        if (format == LoggingFormat::Text) {
            records.push_back({.data = JsonValue::Object{{"fields", std::move(*record)}}});
        }
        else {
            records.push_back(std::move(*record));
        }
        text.remove_prefix(end + 1);
    }
    return records;
}

auto record(std::string_view message, std::span<LogField const> fields = {}) -> LogMessage
{
    return {.level      = LogLevel::Info,
            .message    = message,
            .timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{1234}},
            .thread_id  = std::this_thread::get_id(),
            .event_name = "jobud.test",
            .fields     = fields};
}

} // namespace

TEST_CASE("Daemon JSON preserves scalar types and envelope boundaries", "[jobud][logging]")
{
    auto const fields = std::array{
        LogField{.name = "bool",      .value = true                                     },
        LogField{.name = "signed",    .value = std::numeric_limits<std::int64_t>::min() },
        LogField{.name = "unsigned",  .value = std::numeric_limits<std::uint64_t>::max()},
        LogField{.name = "double",    .value = 1.25                                     },
        LogField{.name = "string",    .value = std::string_view{"quote\" slash\\\n\t"}  },
        LogField{.name = "event",     .value = std::string_view{"nested"}               },
        LogField{.name = "duplicate", .value = std::int64_t{1}                          },
        LogField{.name = "duplicate", .value = std::int64_t{2}                          },
        LogField{.name = "nul",       .value = std::string_view{"a\0b", 3}              }
    };
    auto          sink = make_daemon_logger(LoggingFormat::Json, LogLevel::Info);
    StderrCapture capture;
    sink->log(record("legacy-compatible event", fields));
    auto records = json_lines(capture.text());
    REQUIRE(records.size() == 1);
    auto const& envelope = records.front().as_object();
    CHECK(envelope.at("time").as_string() == "1970-01-01T00:00:01.234Z");
    CHECK(envelope.at("level").as_string() == "INFO");
    CHECK(envelope.at("event").as_string() == "jobud.test");
    CHECK(envelope.at("message").as_string() == "legacy-compatible event");
    CHECK_FALSE(envelope.at("thread").as_string().empty());
    auto const& values = envelope.at("fields").as_object();
    CHECK(values.at("bool").as_bool());
    CHECK(values.at("signed").as_int() == std::numeric_limits<std::int64_t>::min());
    CHECK(values.at("unsigned").as_uint() == std::numeric_limits<std::uint64_t>::max());
    CHECK(values.at("double").as_double() == 1.25);
    CHECK(values.at("string").as_string() == "quote\" slash\\\n\t");
    CHECK(values.at("event").as_string() == "nested");
    CHECK(values.at("duplicate").as_uint() == 1);
    CHECK(values.at("nul").as_string() == std::string{"a\0b", 3});
}

TEST_CASE("Daemon replaces malformed UTF-8 and nonfinite numbers in both formats", "[jobud][logging]")
{
    auto const invalid = std::string{"good\xff\xc0\x80\xed\xa0\x80\xf4\x90\x80\x80\xe2"};
    auto const fields  = std::array{
        LogField{.name = std::string_view{"bad\xff"}, .value = std::string_view{invalid}                               },
        LogField{.name = "nan",                       .value = std::numeric_limits<double>::quiet_NaN()                },
        LogField{.name = "positive",                  .value = std::numeric_limits<double>::infinity()                 },
        LogField{.name = "negative",                  .value = -std::numeric_limits<double>::infinity()                },
        LogField{.name = "valid",                     .value = std::string_view{"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"}}
    };
    for (auto format : {LoggingFormat::Json, LoggingFormat::Text}) {
        StderrCapture capture;
        auto          message = record(invalid, fields);
        message.event_name    = invalid;
        make_daemon_logger(format, LogLevel::Info)->log(message);
        auto const output = capture.text();
        CHECK(is_valid_utf8(output));
        CHECK(output.find("\xef\xbf\xbd") != std::string::npos);
        CHECK(output.find("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") != std::string::npos);
        if (format == LoggingFormat::Json) {
            auto        records = json_lines(output);
            auto const& values  = records.front().as_object().at("fields").as_object();
            CHECK(values.at("nan").is_null());
            CHECK(values.at("positive").is_null());
            CHECK(values.at("negative").is_null());
        }
        else {
            CHECK(output.find("\"nan\":null") != std::string::npos);
        }
    }
}

TEST_CASE("Daemon configuration controls admission and preserves legacy records", "[jobud][logging]")
{
    auto previous = logger();
    for (auto format : {LoggingFormat::Json, LoggingFormat::Text}) {
        StderrCapture capture;
        {
            DaemonLogScope installation{format, LogLevel::Warning};
            log_event(LogLevel::Info, "disabled");
            log_warning("legacy {}", "quote\"\nnext");
        }
        CHECK(logger() == previous);
        auto const output = capture.text();
        CHECK(output.find("disabled") == std::string::npos);
        CHECK(output.find("quote\\\"\\nnext") != std::string::npos);
        CHECK(output.find('\n') == output.size() - 1);
        if (format == LoggingFormat::Json) {
            auto records = json_lines(output);
            REQUIRE(records.size() == 1);
            CHECK(records.front().as_object().at("event").as_string().empty());
            CHECK(records.front().as_object().at("fields").as_object().empty());
        }
    }
}

TEST_CASE("Concurrent daemon stderr records never interleave", "[jobud][logging][thread]")
{
    auto const               format = GENERATE(LoggingFormat::Json, LoggingFormat::Text);
    StderrCapture            capture;
    auto                     sink = make_daemon_logger(format, LogLevel::Info);
    std::barrier             start{4};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (std::uint64_t worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            start.arrive_and_wait();
            for (std::uint64_t sequence = 0; sequence < 100; ++sequence) {
                auto const fields = std::array{
                    LogField{.name = "worker",   .value = worker  },
                    LogField{.name = "sequence", .value = sequence}
                };
                sink->log(record("concurrent record", fields));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    auto records = json_lines(capture.text(), format);
    REQUIRE(records.size() == 400);
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen;
    for (auto const& message : records) {
        auto const& values = message.as_object().at("fields").as_object();
        CHECK(seen.emplace(values.at("worker").as_uint(), values.at("sequence").as_uint()).second);
    }
}

TEST_CASE("Daemon discards output failures without throwing or recursively logging", "[jobud][logging]")
{
    jb::test::OperationalLogGuard recursive_calls;
    for (auto format : {LoggingFormat::Json, LoggingFormat::Text}) {
        auto sink = make_daemon_logger(format, LogLevel::Info);
        sink->set_abort_on_fatal_error(false);
        bool failed{false};
        CHECK_NOTHROW([&] {
            StderrCapture capture;
            REQUIRE(::close(STDERR_FILENO) == 0);
            auto message  = record("unavailable stderr");
            message.level = LogLevel::Fatal;
            sink->log(message);
            failed = std::ferror(stderr) != 0;
        }());
        CHECK(failed);
        CHECK(recursive_calls.capture->records().empty());
    }
}
