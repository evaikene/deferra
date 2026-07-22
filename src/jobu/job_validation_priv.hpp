#pragma once

#include "job.hpp"
#include "result.hpp"

#include <cstddef>
#include <string>
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

class ValidatedJobPayload final {
public:
    ValidatedJobPayload(ValidatedJobPayload&&) noexcept                    = default;
    auto operator=(ValidatedJobPayload&&) noexcept -> ValidatedJobPayload& = default;

    ValidatedJobPayload(ValidatedJobPayload const&)                    = delete;
    auto operator=(ValidatedJobPayload const&) -> ValidatedJobPayload& = delete;

    [[nodiscard]] auto serialized() const noexcept -> std::string_view { return _serialized; }

private:
    explicit ValidatedJobPayload(std::string serialized) noexcept;

    friend auto validate_and_serialize_job_payload(JobType type, jb::rpc::JsonValue const& payload)
        -> jb::core::Result<ValidatedJobPayload, JobPayloadIssue>;

    std::string _serialized;
};

[[nodiscard]] auto is_valid_job_name(std::string_view name) noexcept -> bool;
[[nodiscard]] auto job_payload_structure_issue(JobType type, jb::rpc::JsonValue const& payload) -> JobPayloadIssue;
[[nodiscard]] auto validate_and_serialize_job_payload(JobType type, jb::rpc::JsonValue const& payload)
    -> jb::core::Result<ValidatedJobPayload, JobPayloadIssue>;
[[nodiscard]] auto job_payload_issue_text(JobPayloadIssue issue) noexcept -> std::string_view;

} // namespace jb::jobu::detail
