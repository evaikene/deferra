#include "daemon_logger_priv.hpp"

#include "json.hpp"
#include "text_validation.hpp"

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace jb::jobud::detail {

namespace {

using jb::core::JsonValue;

auto utf8_width(unsigned char byte) noexcept -> std::size_t
{
    if (byte <= 0x7fU) {
        return 1;
    }
    if (byte >= 0xc2U && byte <= 0xdfU) {
        return 2;
    }
    if (byte >= 0xe0U && byte <= 0xefU) {
        return 3;
    }
    if (byte >= 0xf0U && byte <= 0xf4U) {
        return 4;
    }
    return 0;
}

auto repaired_text(std::string_view text) -> std::string
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        auto const byte  = static_cast<unsigned char>(text[index]);
        auto const width = utf8_width(byte);
        // Validate one candidate scalar using the strict reusable validator. An invalid byte is
        // consumed independently so malformed input cannot hide a following valid character.
        if (width != 0 && width <= text.size() - index && jb::core::is_valid_utf8(text.substr(index, width))) {
            result.append(text.substr(index, width));
            index += width;
        }
        else {
            result.append("\xef\xbf\xbd");
            ++index;
        }
    }
    return result;
}

auto field_value(jb::core::LogField::Value const& value) -> JsonValue
{
    return std::visit(
        [](auto const& scalar) -> JsonValue {
            using Scalar = std::decay_t<decltype(scalar)>;
            if constexpr (std::is_same_v<Scalar, std::string_view>) {
                return {.data = repaired_text(scalar)};
            }
            else if constexpr (std::is_same_v<Scalar, double>) {
                return std::isfinite(scalar) ? JsonValue{.data = scalar} : JsonValue{};
            }
            else {
                return {.data = scalar};
            }
        },
        value);
}

auto timestamp_text(std::chrono::system_clock::time_point timestamp) -> std::string
{
    auto const seconds    = std::chrono::floor<std::chrono::seconds>(timestamp);
    auto const millis     = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - seconds).count();
    auto const clock_time = std::chrono::system_clock::to_time_t(seconds);
    std::tm    utc{};
    if (::gmtime_r(&clock_time, &utc) == nullptr) {
        return "unavailable";
    }
    return fmt::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", utc, millis);
}

auto envelope(jb::core::LogMessage const& record) -> JsonValue
{
    JsonValue::Object fields;
    for (auto const& field : record.fields) {
        // emplace preserves the first field, including names colliding after UTF-8 repair.
        fields.emplace(repaired_text(field.name), field_value(field.value));
    }
    return {
        .data = JsonValue::Object{
                                  {"time", {.data = timestamp_text(record.timestamp)}},
                                  {"level", {.data = std::string{jb::core::log_level_name(record.level)}}},
                                  {"event", {.data = repaired_text(record.event_name)}},
                                  {"message", {.data = repaired_text(record.message)}},
                                  {"thread", {.data = fmt::format("{}", fmt::streamed(record.thread_id))}},
                                  {"fields", {.data = std::move(fields)}},
                                  }
    };
}

auto format_record(jb::core::LogMessage const& record, LoggingFormat format) -> std::string
{
    auto const document = envelope(record);
    if (format == LoggingFormat::Json) {
        auto encoded = jb::core::serialize_json(document);
        // Every string/number has already been normalized. Keep an independent, valid fallback
        // for an unexpected codec failure, without forwarding codec detail or recursing into logging.
        if (!encoded) {
            return "{\"event\":\"jobud.log.encoding_failed\",\"fields\":{},\"level\":\"ERROR\","
                   "\"message\":\"Logging record could not be "
                   "encoded\",\"thread\":\"unavailable\",\"time\":\"unavailable\"}\n";
        }
        return std::move(*encoded) + '\n';
    }

    auto const& members = document.as_object();
    auto        event   = jb::core::serialize_json(members.at("event"));
    auto        message = jb::core::serialize_json(members.at("message"));
    auto        fields  = jb::core::serialize_json(members.at("fields"));
    if (!event || !message || !fields) {
        return "unavailable [ERROR] thread=unavailable event=\"jobud.log.encoding_failed\" "
               "message=\"Logging record could not be encoded\" fields={}\n";
    }
    return fmt::format("{} [{}] thread={} event={} message={} fields={}\n",
                       members.at("time").as_string(),
                       members.at("level").as_string(),
                       members.at("thread").as_string(),
                       *event,
                       *message,
                       *fields);
}

auto stderr_mutex() -> std::mutex&
{
    static std::mutex mutex;
    return mutex;
}

class DaemonLogger final : public jb::core::Logger {
public:
    explicit DaemonLogger(LoggingFormat format)
        : _format{format}
    {}

    void log(jb::core::LogMessage const& record) override
    {
        auto const line = format_record(record, _format);
        // Format before locking. One serialized write/flush covers the whole record, even when
        // another admitted call still holds a previous daemon sink during logger replacement.
        {
            std::scoped_lock lock{stderr_mutex()};
            static_cast<void>(std::fwrite(line.data(), 1, line.size(), stderr));
            static_cast<void>(std::fflush(stderr));
        }
        if (record.level == jb::core::LogLevel::Fatal && _abort_on_fatal_error) {
            std::abort();
        }
    }

private:
    LoggingFormat _format;
};

} // namespace

auto make_daemon_logger(LoggingFormat format, jb::core::LogLevel level) -> std::shared_ptr<jb::core::Logger>
{
    auto sink = std::make_shared<DaemonLogger>(format);
    sink->set_level(level);
    return sink;
}

DaemonLogScope::DaemonLogScope(LoggingFormat format, jb::core::LogLevel level)
    : _previous{jb::core::logger()}
{
    jb::core::set_logger(make_daemon_logger(format, level));
}

DaemonLogScope::~DaemonLogScope()
{
    jb::core::set_logger(std::move(_previous));
}

} // namespace jb::jobud::detail
