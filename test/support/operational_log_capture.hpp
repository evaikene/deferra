#pragma once

#include "json.hpp"
#include "logging.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace jb::test {

struct OperationalRecord {
    jb::core::LogLevel          level;
    std::string                 event;
    std::string                 message;
    jb::core::JsonValue::Object fields;
};

/// Copies every borrowed part before returning, allowing assertions after service teardown.
class OperationalLogCapture final : public jb::core::Logger {
public:
    void log(jb::core::LogMessage const& message) override
    {
        OperationalRecord record{.level   = message.level,
                                 .event   = std::string{message.event_name},
                                 .message = std::string{message.message},
                                 .fields  = {}};
        for (auto const& field : message.fields) {
            auto value = std::visit(
                [](auto const& scalar) -> jb::core::JsonValue {
                    if constexpr (std::is_same_v<std::decay_t<decltype(scalar)>, std::string_view>) {
                        return {.data = std::string{scalar}};
                    }
                    else {
                        return {.data = scalar};
                    }
                },
                field.value);
            record.fields.emplace(std::string{field.name}, std::move(value));
        }
        std::scoped_lock lock{_mutex};
        _records.push_back(std::move(record));
    }

    auto records() const -> std::vector<OperationalRecord>
    {
        std::scoped_lock lock{_mutex};
        return _records;
    }

private:
    mutable std::mutex             _mutex;
    std::vector<OperationalRecord> _records;
};

struct OperationalLogGuard {
    OperationalLogGuard()
    {
        capture->set_level(jb::core::LogLevel::Debug3);
        jb::core::set_logger(capture);
    }

    ~OperationalLogGuard() { jb::core::set_logger(std::move(previous)); }

    std::shared_ptr<jb::core::Logger>      previous{jb::core::logger()};
    std::shared_ptr<OperationalLogCapture> capture{std::make_shared<OperationalLogCapture>()};
};

} // namespace jb::test
