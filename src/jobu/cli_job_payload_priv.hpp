#pragma once

#include "job_validation_priv.hpp"
#include "json.hpp"
#include "result.hpp"

#include <bitset>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jb::jobu::detail {

inline constexpr std::size_t maximum_cli_path_bytes{4096};
inline constexpr std::size_t maximum_cli_arguments{1024};
inline constexpr std::size_t maximum_cli_environment_entries{256};
inline constexpr std::size_t maximum_cli_path_entries{256};
inline constexpr std::size_t maximum_cli_path_candidate_bytes{std::size_t{256} * 1024};
inline constexpr std::size_t maximum_cli_prepared_request_bytes{std::size_t{256} * 1024};

using CliEnvironmentPatch  = std::map<std::string, std::optional<std::string>, std::less<>>;
using CliExpectedExitCodes = std::bitset<256>;

struct CliJobPayload {
    std::string              command;
    std::vector<std::string> arguments;
    std::string              working_directory{"/"};
    CliEnvironmentPatch      environment;
    CliExpectedExitCodes     expected_exit_codes;
};

[[nodiscard]] auto decode_cli_job_payload(jb::core::JsonValue const& payload)
    -> jb::core::Result<CliJobPayload, JobPayloadIssue>;

} // namespace jb::jobu::detail
