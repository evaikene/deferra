#pragma once

#include "error.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <curl/curl.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace jb::net::http::detail {

struct CurlEasyDeleter {
    void operator()(CURL* easy) const noexcept;
};

using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;

/// Owns the minimal Stage 5.3 easy handle, request data, and completion obligation.
class CurlRequest final {
public:
    enum class State : std::uint8_t {
        Running,
        PendingCompletion,
    };

    [[nodiscard]] static auto create(HttpRequestId id, HttpRequest request, HttpCompletionHandler completion)
        -> jb::core::Result<std::unique_ptr<CurlRequest>, jb::core::Error>;

    ~CurlRequest() = default;

    CurlRequest(CurlRequest const&)                    = delete;
    CurlRequest(CurlRequest&&)                         = delete;
    auto operator=(CurlRequest const&) -> CurlRequest& = delete;
    auto operator=(CurlRequest&&) -> CurlRequest&      = delete;

    [[nodiscard]] auto id() const noexcept -> HttpRequestId { return _id; }

    [[nodiscard]] auto easy() const noexcept -> CURL* { return _easy.get(); }

    [[nodiscard]] auto state() const noexcept -> State { return _state; }

    [[nodiscard]] auto accepted() const noexcept -> bool { return _accepted; }

    void mark_accepted() noexcept { _accepted = true; }

    void               prepare_completion(HttpCompletionResult result);
    [[nodiscard]] auto take_handler() -> HttpCompletionHandler;
    [[nodiscard]] auto take_result() -> HttpCompletionResult;
    [[nodiscard]] auto minimal_transfer_result(CURLcode result) const -> HttpCompletionResult;

private:
    CurlRequest(HttpRequestId id, HttpRequest request, HttpCompletionHandler completion, CurlEasy easy);

    HttpRequestId                       _id;
    HttpRequest                         _request;
    HttpCompletionHandler               _completion;
    CurlEasy                            _easy;
    State                               _state{State::Running};
    bool                                _accepted{false};
    std::optional<HttpCompletionResult> _result;
};

} // namespace jb::net::http::detail
