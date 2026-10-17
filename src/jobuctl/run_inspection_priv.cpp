#include "run_inspection_priv.hpp"

#include "attempt.hpp"
#include "commands/commands_priv.hpp"
#include "history_json.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <csignal> // IWYU pragma: keep for the platform NSIG macro
#include <cstdint>
#include <string_view>
#include <variant>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

constexpr auto maximum_diagnostic_string_bytes = std::size_t{128};

constexpr std::array cli_outcomes{
    std::string_view{"success"},
    std::string_view{"unexpected_exit"},
    std::string_view{"signal"},
    std::string_view{"timeout"},
    std::string_view{"cancelled"},
    std::string_view{"start_failure"},
    std::string_view{"internal_error"},
};
constexpr std::array http_outcomes{
    std::string_view{"expected_status"},
    std::string_view{"unexpected_status"},
    std::string_view{"transport_error"},
    std::string_view{"cancelled"},
    std::string_view{"internal_error"},
};
constexpr std::array http_error_categories{
    std::string_view{"invalid_request"},
    std::string_view{"resolve"},
    std::string_view{"connect"},
    std::string_view{"tls_verification"},
    std::string_view{"tls_handshake"},
    std::string_view{"timeout"},
    std::string_view{"send"},
    std::string_view{"receive"},
    std::string_view{"redirect"},
    std::string_view{"protocol"},
    std::string_view{"cancelled"},
    std::string_view{"internal"},
};

auto bounded_integer(JsonValue::Object const& object,
                     std::string_view         field,
                     std::uint64_t            minimum,
                     std::uint64_t            maximum) -> std::optional<std::uint64_t>
{
    auto const found = object.find(field);
    if (found == object.end()) {
        return std::nullopt;
    }

    // Check the sign before conversion; doubles, booleans, and numeric strings are not integer observations.
    auto number = std::uint64_t{};
    if (auto const* value = std::get_if<std::int64_t>(&found->second.data)) {
        if (*value < 0) {
            return std::nullopt;
        }
        number = static_cast<std::uint64_t>(*value);
    }
    else if (auto const* value = std::get_if<std::uint64_t>(&found->second.data)) {
        number = *value;
    }
    else {
        return std::nullopt;
    }

    if (number < minimum || number > maximum) {
        return std::nullopt;
    }
    return number;
}

auto bounded_string(JsonValue::Object const& object, std::string_view field) -> std::optional<std::string_view>
{
    auto const found = object.find(field);
    if (found == object.end()) {
        return std::nullopt;
    }
    auto const* text = std::get_if<std::string>(&found->second.data);
    if (!text || text->empty() || text->size() > maximum_diagnostic_string_bytes) {
        return std::nullopt;
    }
    return *text;
}

auto known_token(JsonValue::Object const& object, std::string_view field, std::span<std::string_view const> tokens)
    -> std::optional<std::string>
{
    auto text = bounded_string(object, field);
    if (!text || std::ranges::find(tokens, *text) == tokens.end()) {
        return std::nullopt;
    }
    return std::string{*text};
}

auto ascii_identifier_start(char value) -> bool
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

auto diagnostic_error_code(JsonValue::Object const& object) -> std::optional<std::string>
{
    auto text = bounded_string(object, "error_code");
    if (!text || !ascii_identifier_start(text->front())) {
        return std::nullopt;
    }

    // Reject the entire value rather than display a safe-looking prefix of arbitrary text or credentials.
    auto const safe = std::ranges::all_of(*text, [](char value) {
        return ascii_identifier_start(value) || (value >= '0' && value <= '9') || value == '.' || value == '-';
    });
    return safe ? std::optional{std::string{*text}} : std::nullopt;
}

auto optional_summary_text(JsonValue const& value) -> std::string
{
    return value.is_null() ? "none" : escape_human(value.as_string());
}

void append_diagnostics(std::string& text, AttemptDiagnostics const& diagnostics)
{
    if (diagnostics.exit_code) {
        text += fmt::format("  exit_code={}\n", *diagnostics.exit_code);
    }
    if (diagnostics.signal_number) {
        text += fmt::format("  signal={}\n", *diagnostics.signal_number);
    }
    if (diagnostics.http_status) {
        text += fmt::format("  status={}\n", *diagnostics.http_status);
    }

    // These strings have already passed bounded allowlists; escaping is still required at the display boundary.
    if (diagnostics.error_code) {
        text += fmt::format("  error_code={}\n", escape_human(*diagnostics.error_code));
    }
    if (diagnostics.error_category) {
        text += fmt::format("  error_category={}\n", escape_human(*diagnostics.error_category));
    }
    if (diagnostics.runner_outcome) {
        text += fmt::format("  runner_outcome={}\n", escape_human(*diagnostics.runner_outcome));
    }
}

} // namespace

auto project_attempt_diagnostics(JobType type, JsonValue const& result) -> AttemptDiagnostics
{
    auto const* object = std::get_if<JsonValue::Object>(&result.data);
    if (!object) {
        return {};
    }

    // The stored run type chooses the allowlist. A result's own type or outcome never overrides durable metadata.
    auto diagnostics = AttemptDiagnostics{};
    switch (type) {
        case JobType::Cli:
            if (auto value = bounded_integer(*object, "exit_code", 0, 255)) {
                diagnostics.exit_code = static_cast<int>(*value);
            }
            if (auto value = bounded_integer(*object, "signal", 1, static_cast<std::uint64_t>(NSIG - 1))) {
                diagnostics.signal_number = static_cast<int>(*value);
            }
            diagnostics.runner_outcome = known_token(*object, "outcome", cli_outcomes);
            break;
        case JobType::Http:
            if (auto value = bounded_integer(*object, "status", 100, 599)) {
                diagnostics.http_status = static_cast<unsigned>(*value);
            }
            diagnostics.error_category = known_token(*object, "error_category", http_error_categories);
            diagnostics.runner_outcome = known_token(*object, "outcome", http_outcomes);
            break;
        default:
            return {};
    }
    diagnostics.error_code = diagnostic_error_code(*object);
    return diagnostics;
}

auto format_last_attempt(AttemptSummary const& attempt) -> std::optional<std::string>
{
    // Encode only the public summary. Full detail serialization would traverse arbitrary optional result JSON.
    auto encoded = attempt_summary_to_json(attempt);
    if (!encoded) {
        return std::nullopt;
    }
    auto const& fields = encoded->as_object();
    return fmt::format("Last attempt: {}: state={}, outcome={}\n  started_at={}, completed_at={}\n",
                       attempt.attempt_number,
                       escape_human(fields.at("state").as_string()),
                       optional_summary_text(fields.at("outcome")),
                       optional_summary_text(fields.at("started_at")),
                       optional_summary_text(fields.at("completed_at")));
}

auto format_last_attempt(JobType type, AttemptDetails const& attempt) -> std::optional<std::string>
{
    auto text = format_last_attempt(attempt);
    if (text && attempt.state == AttemptState::Completed && attempt.result) {
        append_diagnostics(*text, project_attempt_diagnostics(type, *attempt.result));
    }
    return text;
}

auto run_output_hint(Uuid const& run_id, JobType type) -> std::string
{
    auto channel = std::string_view{};
    switch (type) {
        case JobType::Cli:
            channel = "stderr";
            break;
        case JobType::Http:
            channel = "body";
            break;
        default:
            return {};
    }
    return fmt::format("Output uses the latest completed attempt, which may be older than a running retry.\n"
                       "  jobuctl run output {} --channel {}\n",
                       run_id.to_string(),
                       channel);
}

auto latest_completed_attempt(std::span<AttemptSummary const> attempts) -> std::optional<AttemptSummary>
{
    auto const found = std::ranges::find_if(attempts, [](AttemptSummary const& attempt) {
        return attempt.state == AttemptState::Completed;
    });
    return found == attempts.end() ? std::nullopt : std::optional{*found};
}

} // namespace jb::jobuctl::detail
