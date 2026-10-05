#include "logging.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

static_assert(std::is_same_v<jb::core::LogField::Value,
                             std::variant<bool, std::int64_t, std::uint64_t, double, std::string_view>>);
static_assert(std::is_same_v<decltype(jb::core::LogMessage::fields), std::span<jb::core::LogField const>>);

namespace {

class HeaderLogger final : public jb::core::Logger {
public:
    void log(jb::core::LogMessage const& record) override
    {
        received = record.event_name == "header.checked" && record.message == record.event_name &&
                   record.fields.size() == 1 && record.fields[0].name == "valid" &&
                   std::get<bool>(record.fields[0].value);
    }

    bool received{false};
};

} // namespace

auto main() -> int
{
    auto sink = std::make_shared<HeaderLogger>();
    sink->set_level(jb::core::LogLevel::Info);
    jb::core::set_logger(sink);

    std::array const fields{
        jb::core::LogField{.name = "valid", .value = true}
    };
    jb::core::log_event(jb::core::LogLevel::Info, "header.checked", fields);
    jb::core::set_logger(nullptr);
    return sink->received ? 0 : 1;
}
