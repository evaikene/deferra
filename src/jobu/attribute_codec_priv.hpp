#pragma once

#include "attribute.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstdint>

namespace jb::jobu::detail {

enum class AttributeDocumentMode : std::uint8_t {
    Partial,
    Materialized,
};

[[nodiscard]] auto encode_attribute_document(AttributeRegistry const& registry,
                                             AttributeSet const&      values,
                                             AttributeScope           scope,
                                             AttributeDocumentMode    mode)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

[[nodiscard]] auto decode_attribute_document(AttributeRegistry const&  registry,
                                             jb::rpc::JsonValue const& document,
                                             AttributeScope            scope,
                                             AttributeDocumentMode     mode)
    -> jb::core::Result<AttributeSet, jb::core::Error>;

} // namespace jb::jobu::detail
