#pragma once

#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace jb::rpc::detail {

inline constexpr std::size_t default_max_batch_entries{64U};

struct RequestEnvelope {
    std::optional<RequestId>           id;
    std::string                        method;
    std::optional<jb::core::JsonValue> params;

    auto operator==(RequestEnvelope const&) const -> bool = default;
};

struct InvalidRequest {
    auto operator==(InvalidRequest const&) const -> bool = default;
};

using RequestEntry = std::variant<RequestEnvelope, InvalidRequest>;

enum class RequestDocumentKind : std::uint8_t {
    Single,
    Batch,
    RejectedBatch,
};

struct RequestDocument {
    RequestDocumentKind       kind{RequestDocumentKind::Single};
    std::vector<RequestEntry> entries;

    auto operator==(RequestDocument const&) const -> bool = default;
};

using ResponsePayload = std::variant<jb::core::JsonValue, RpcError>;

struct ResponseEnvelope {
    RequestId       id;
    ResponsePayload payload;

    auto operator==(ResponseEnvelope const&) const -> bool = default;
};

enum class ResponseDocumentKind : unsigned char {
    Single,
    Batch,
};

struct ResponseDocument {
    ResponseDocumentKind          kind{ResponseDocumentKind::Single};
    std::vector<ResponseEnvelope> entries;

    auto operator==(ResponseDocument const&) const -> bool = default;
};

[[nodiscard]] auto decode_request_document(jb::core::JsonValue const& value,
                                           std::size_t                max_batch_entries = default_max_batch_entries)
    -> RequestDocument;

[[nodiscard]] auto decode_response_document(jb::core::JsonValue const& value,
                                            std::size_t                max_batch_entries = default_max_batch_entries)
    -> jb::core::Result<ResponseDocument, jb::core::Error>;

[[nodiscard]] auto encode_request(RequestId const&                   id,
                                  std::string_view                   method,
                                  std::optional<jb::core::JsonValue> params = {}) -> jb::core::JsonValue;
[[nodiscard]] auto encode_notification(std::string_view method, std::optional<jb::core::JsonValue> params = {})
    -> jb::core::JsonValue;
[[nodiscard]] auto encode_success_response(RequestId const& id, jb::core::JsonValue const& result)
    -> jb::core::JsonValue;
[[nodiscard]] auto encode_error_response(RequestId const& id, RpcError const& error) -> jb::core::JsonValue;
[[nodiscard]] auto encode_batch(std::vector<jb::core::JsonValue> entries) -> std::optional<jb::core::JsonValue>;

[[nodiscard]] auto make_standard_error(ErrorCode code) -> RpcError;
[[nodiscard]] auto is_reserved_method(std::string_view method) noexcept -> bool;

} // namespace jb::rpc::detail
