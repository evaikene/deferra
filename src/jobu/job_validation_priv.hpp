#pragma once

#include "job.hpp"

#include <cstddef>
#include <string_view>

namespace jb::jobu::detail {

inline constexpr std::size_t maximum_job_document_bytes = 256U * 1024U;

enum class JobPayloadIssue {
    None,
    UnknownType,
    NotObject,
    MissingCommand,
    InvalidArguments,
    MissingUrl,
    InvalidMethod,
    InvalidJson,
    TooLarge,
};

[[nodiscard]] auto is_valid_job_name(std::string_view name) noexcept -> bool;
[[nodiscard]] auto job_payload_issue(JobType type, jb::rpc::JsonValue const& payload) -> JobPayloadIssue;
[[nodiscard]] auto job_payload_issue_text(JobPayloadIssue issue) noexcept -> std::string_view;

} // namespace jb::jobu::detail
