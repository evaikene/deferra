#pragma once
#include <jb/jobu/history.hpp>
#include <jb/jobu/secret.hpp>
#include <jb/jobu/system_info.hpp>
#include <phpcxx/result.hpp>
#include <phpcxx/value.hpp>

namespace jb::php {
/// Bind generated metadata, result and output classes during MINIT.
auto bind_result_classes(zend_class_entry* system,
                         zend_class_entry* secret,
                         zend_class_entry* base,
                         zend_class_entry* cli,
                         zend_class_entry* http,
                         zend_class_entry* output) -> bool;
/// Copy validated native metadata; no client or secret bytes are retained.
auto system_info_to_php(jobu::SystemInfo const& value) -> ::php::Result<::php::Value>;
auto secret_metadata_to_php(jobu::SecretMetadata const& value) -> ::php::Result<::php::Value>;
/// Preserve a safe history result, selecting its class from trusted run metadata.
/// Null stays null; objects are bounded to 256 KiB / 64 levels. No application JSON interpretation.
auto execution_result_to_php(jobu::JobType type, std::optional<core::JsonValue> const& value)
    -> ::php::Result<::php::Value>;
/// Copy a decoded output slice, rejecting metadata that cannot fit PHP integers.
auto output_chunk_to_php(jobu::AttemptOutputChunk const& value) -> ::php::Result<::php::Value>;
} // namespace jb::php
