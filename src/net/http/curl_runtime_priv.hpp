#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstdint>

namespace jb::net::http::detail {

inline constexpr std::uint32_t kMinimumCurlRuntimeVersion{0x075500U};

/// Copies the libcurl capabilities used by factory option and runtime validation.
struct CurlRuntimeCapabilities {
    std::uint32_t version_number{0};
    bool          supports_http{false};
    bool          supports_https{false};
    bool          supports_ssl{false};
    bool          supports_zlib{false};
    bool          supports_asynchronous_dns{false};
    bool          supports_https_proxy{false};
};

/// Validates the mandatory runtime capabilities without consulting process-global state.
[[nodiscard]] auto validate_curl_runtime(CurlRuntimeCapabilities const& capabilities)
    -> jb::core::Result<void, jb::core::Error>;

/// Initializes libcurl once for the process and returns a checked capability snapshot.
[[nodiscard]] auto preflight_curl_runtime() -> jb::core::Result<CurlRuntimeCapabilities, jb::core::Error>;

} // namespace jb::net::http::detail
