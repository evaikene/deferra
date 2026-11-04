#pragma once

namespace php {

/** PHP ABI against which this copy of the static library was compiled. */
struct BuildInfo {
    int          version_id;
    int          module_api;
    bool         debug;
    bool         zts;
    unsigned int zend_long_size;
};

/**
 * Return immutable build metadata owned by this linked copy of the library.
 *
 * This query is thread-safe and does not require an initialized PHP request.
 * The reference remains valid while the containing executable or module is loaded.
 */
const BuildInfo& build_info() noexcept;

} // namespace php
