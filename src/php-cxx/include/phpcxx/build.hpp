#pragma once

/// @file
/// Build metadata that can be queried without initializing PHP. See zend.hpp for
/// library integration, ABI matching, and request/thread lifetime requirements.

namespace php {

/// PHP ABI against which this copy of the static library was compiled.
struct BuildInfo {
    /// PHP_VERSION_ID: major * 10000 + minor * 100 + patch of the selected headers.
    int          version_id;
    /// ZEND_MODULE_API_NO of the selected PHP installation; not a PHP version number.
    int          module_api;
    /// Whether the library was compiled against a Zend debug build (ZEND_DEBUG).
    bool         debug;
    /// Whether the selected PHP build uses Zend Thread Safety (ZTS).
    bool         zts;
    /// Size of zend_long in bytes; supported installations have an eight-byte zend_long.
    unsigned int zend_long_size;
};

/// Return immutable build metadata owned by this linked copy of the library.
///
/// This query is thread-safe and does not require an initialized PHP request.
/// The reference remains valid while the containing executable or module is loaded.
const BuildInfo& build_info() noexcept;

} // namespace php
