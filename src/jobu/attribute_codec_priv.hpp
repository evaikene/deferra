#pragma once

#include "attribute.hpp"
#include "json.hpp"
#include "result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace jb::jobu::detail {

enum class AttributeDocumentMode : std::uint8_t {
    Partial,
    Materialized,
};

class SerializedAttributeDocument final {
public:
    SerializedAttributeDocument(SerializedAttributeDocument&&) noexcept                    = default;
    auto operator=(SerializedAttributeDocument&&) noexcept -> SerializedAttributeDocument& = default;

    SerializedAttributeDocument(SerializedAttributeDocument const&)                    = delete;
    auto operator=(SerializedAttributeDocument const&) -> SerializedAttributeDocument& = delete;

    [[nodiscard]] auto serialized() const noexcept -> std::string_view { return _serialized; }

private:
    explicit SerializedAttributeDocument(std::string serialized) noexcept;

    friend auto encode_and_serialize_attribute_document(AttributeRegistry const& registry,
                                                        AttributeSet const&      values,
                                                        AttributeScope           scope,
                                                        AttributeDocumentMode    mode)
        -> jb::core::Result<SerializedAttributeDocument, jb::core::Error>;

    std::string _serialized;
};

[[nodiscard]] auto encode_attribute_document(AttributeRegistry const& registry,
                                             AttributeSet const&      values,
                                             AttributeScope           scope,
                                             AttributeDocumentMode    mode)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>;

[[nodiscard]] auto encode_and_serialize_attribute_document(AttributeRegistry const& registry,
                                                           AttributeSet const&      values,
                                                           AttributeScope           scope,
                                                           AttributeDocumentMode    mode)
    -> jb::core::Result<SerializedAttributeDocument, jb::core::Error>;

[[nodiscard]] auto decode_attribute_document(AttributeRegistry const&  registry,
                                             jb::rpc::JsonValue const& document,
                                             AttributeScope            scope,
                                             AttributeDocumentMode     mode)
    -> jb::core::Result<AttributeSet, jb::core::Error>;

} // namespace jb::jobu::detail
