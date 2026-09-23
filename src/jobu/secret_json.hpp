/// @file secret_json.hpp
/// @brief Typed JSON conversion for metadata-only secret administration.
#pragma once

#include "json.hpp"
#include "secret.hpp"

#include <string>
#include <string_view>

namespace jb::jobu {

/// Encodes raw bytes as canonical padded base64. The service still validates the name and value limit.
[[nodiscard]] auto set_secret_request_to_json(SetSecretRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
/// Strictly decodes {name,value:{encoding,data}}. Malformed input is invalid_request; decoded bytes above
/// 65,536 return jobu.secret.too_large without retaining or echoing the input.
[[nodiscard]] auto set_secret_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<SetSecretRequest, jb::core::Error>;

/// Encodes a list request with its limit and optional exclusive name boundary.
[[nodiscard]] auto secret_list_request_to_json(SecretListRequest const& request)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
/// Strictly decodes list fields; the service checks the limit and name policy.
[[nodiscard]] auto secret_list_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<SecretListRequest, jb::core::Error>;

/// Encodes a delete request containing one name.
[[nodiscard]] auto secret_delete_request_to_json(std::string_view name)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
/// Strictly decodes a delete request; the service checks the name policy.
[[nodiscard]] auto secret_delete_request_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<std::string, jb::core::Error>;

/// Encodes only the name and creation/update timestamps; never selects or serializes secret bytes.
[[nodiscard]] auto secret_metadata_to_json(SecretMetadata const& metadata)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
/// Decodes required metadata fields while ignoring unknown response members.
[[nodiscard]] auto secret_metadata_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<SecretMetadata, jb::core::Error>;

/// Encodes an ordered metadata page with an explicit nullable continuation.
[[nodiscard]] auto secret_page_to_json(SecretPage const& page)
    -> jb::core::Result<jb::core::JsonValue, jb::core::Error>;
/// Decodes an ordered metadata page while ignoring unknown response members.
[[nodiscard]] auto secret_page_from_json(jb::core::JsonValue const& value)
    -> jb::core::Result<SecretPage, jb::core::Error>;

} // namespace jb::jobu
