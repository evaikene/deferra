#pragma once

#include "byte_buffer.hpp"
#include "http_client.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jb::jobu::detail {

struct HttpJobPayload;

struct HttpStatusRange {
    std::uint16_t first{0};
    std::uint16_t last{0};

    auto operator==(HttpStatusRange const&) const -> bool = default;
};

class HttpStatusSet final {
public:
    [[nodiscard]] auto contains(std::uint16_t status) const noexcept -> bool;

    [[nodiscard]] auto ranges() const noexcept -> std::vector<HttpStatusRange> const& { return _ranges; }

    auto operator==(HttpStatusSet const&) const -> bool = default;

private:
    explicit HttpStatusSet(std::vector<HttpStatusRange> ranges) noexcept;

    friend auto decode_http_job_payload(jb::core::JsonValue const& payload)
        -> jb::core::Result<HttpJobPayload, JobPayloadIssue>;

    std::vector<HttpStatusRange> _ranges;
};

struct HttpJobPayload {
    std::string                         url;
    std::string                         method;
    std::vector<jb::net::HttpHeader>    headers;
    std::optional<jb::core::ByteBuffer> body;
    HttpStatusSet                       expected_statuses;
};

[[nodiscard]] auto decode_http_job_payload(jb::core::JsonValue const& payload)
    -> jb::core::Result<HttpJobPayload, JobPayloadIssue>;

} // namespace jb::jobu::detail
