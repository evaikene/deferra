#include "execution_diagnostics_priv.hpp"

#include "job.hpp"
#include "json.hpp"

#include <csignal> // IWYU pragma: keep for the platform NSIG macro
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace jb::jobu::detail {

namespace {

auto bounded_integer(jb::core::JsonValue::Object const& object,
                     std::string_view                   field,
                     std::uint64_t                      minimum,
                     std::uint64_t                      maximum) -> std::optional<std::uint64_t>
{
    auto const found = object.find(field);
    if (found == object.end()) {
        return std::nullopt;
    }

    // JSON integers have two exact representations. Reject negative signed values before converting to unsigned.
    auto value = std::uint64_t{};
    if (auto const* signed_value = std::get_if<std::int64_t>(&found->second.data)) {
        if (*signed_value < 0) {
            return std::nullopt;
        }
        value = static_cast<std::uint64_t>(*signed_value);
    }
    else if (auto const* unsigned_value = std::get_if<std::uint64_t>(&found->second.data)) {
        value = *unsigned_value;
    }
    else {
        return std::nullopt;
    }

    if (value < minimum || value > maximum) {
        return std::nullopt;
    }
    return value;
}

} // namespace

auto project_execution_diagnostics(JobType type, jb::core::JsonValue const& effective_result) -> ExecutionDiagnostics
{
    auto const* object = std::get_if<jb::core::JsonValue::Object>(&effective_result.data);
    if (!object) {
        return {};
    }

    // Inspect a fixed allowlist selected by the trusted run type, never by the result's own type/outcome text.
    auto projected = ExecutionDiagnostics{};
    switch (type) {
        case JobType::Cli:
            if (auto value = bounded_integer(*object, "exit_code", 0, 255)) {
                projected.exit_code = static_cast<int>(*value);
            }
            // NSIG is the platform's exclusive signal bound, including real-time signals where supported.
            if (auto value = bounded_integer(*object, "signal", 1, static_cast<std::uint64_t>(NSIG - 1))) {
                projected.signal_number = static_cast<int>(*value);
            }
            break;
        case JobType::Http:
            if (auto value = bounded_integer(*object, "status", 100, 599)) {
                projected.http_status = static_cast<unsigned>(*value);
            }
            break;
        default:
            break;
    }
    return projected;
}

} // namespace jb::jobu::detail
