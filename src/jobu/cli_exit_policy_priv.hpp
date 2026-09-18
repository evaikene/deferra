#pragma once

#include "attribute.hpp"
#include "cli_capture_priv.hpp"
#include "cli_job_payload_priv.hpp"
#include "error.hpp"
#include "json.hpp"
#include "process.hpp"
#include "result.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace jb::jobu::detail {

struct CliExitCodeRange {
    std::uint16_t first{0};
    std::uint16_t last{0};

    auto operator==(CliExitCodeRange const&) const -> bool = default;
};

class CliExitCodeSet final {
public:
    [[nodiscard]] auto contains(int exit_code) const noexcept -> bool;

    [[nodiscard]] auto ranges() const noexcept -> std::vector<CliExitCodeRange> const& { return _ranges; }

    auto operator==(CliExitCodeSet const&) const -> bool = default;

private:
    explicit CliExitCodeSet(std::vector<CliExitCodeRange> ranges) noexcept;

    friend auto decode_cli_retry_exit_codes(AttributeValue::List const& selectors) -> std::optional<CliExitCodeSet>;

    std::vector<CliExitCodeRange> _ranges;
};

struct CliCompletionPolicy {
    AttemptOutcome                    outcome{AttemptOutcome::Failed};
    std::optional<FailureDisposition> failure_disposition;
    jb::core::JsonValue               result;
    std::optional<AttemptOutput>      output;
};

[[nodiscard]] auto decode_cli_retry_exit_codes(AttributeValue::List const& selectors) -> std::optional<CliExitCodeSet>;

[[nodiscard]] auto map_cli_completion(jb::core::ProcessExit const& process_exit,
                                      CliExpectedExitCodes const&  expected_exit_codes,
                                      CliExitCodeSet const&        retry_exit_codes,
                                      CliCaptureMode               capture_mode,
                                      CliCaptureSnapshot           capture)
    -> jb::core::Result<CliCompletionPolicy, jb::core::Error>;

} // namespace jb::jobu::detail
