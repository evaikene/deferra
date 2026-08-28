#pragma once

#include "error.hpp"
#include "event_loop_types.hpp"
#include "http_capture_priv.hpp"
#include "http_client.hpp"
#include "result.hpp"

#include <curl/curl.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace jb::net::http::detail {

struct CurlEasyDeleter {
    void operator()(CURL* easy) const noexcept;
};

using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;

struct CurlSlistDeleter {
    void operator()(curl_slist* list) const noexcept;
};

using CurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

/// Owns one non-redirect easy handle, all pointer targets, response state, and completion obligation.
class CurlRequest final {
public:
    enum class State : std::uint8_t {
        Running,
        PendingCompletion,
    };

    [[nodiscard]] static auto create(HttpRequestId         id,
                                     HttpRequest           request,
                                     HttpCompletionHandler completion,
                                     std::size_t           maximum_parsed_response_header_bytes)
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

    void mark_accepted() noexcept;

    void               prepare_completion(HttpCompletionResult result);
    [[nodiscard]] auto take_handler() -> HttpCompletionHandler;
    [[nodiscard]] auto take_result() -> HttpCompletionResult;
    [[nodiscard]] auto transfer_result(CURLcode result) -> HttpCompletionResult;

private:
    enum class HeaderState : std::uint8_t {
        AwaitingStatus,
        Fields,
        Complete,
    };

    struct ParsedHeaderBlock {
        std::uint16_t           status_code{0};
        std::vector<HttpHeader> headers;
        HttpCapturedData        raw_headers;
    };

    CurlRequest(HttpRequestId         id,
                HttpRequest           request,
                HttpCompletionHandler completion,
                std::size_t           maximum_parsed_response_header_bytes,
                CurlEasy              easy);

    [[nodiscard]] static auto body_callback(char* data, std::size_t size, std::size_t count, void* context) noexcept
        -> std::size_t;
    [[nodiscard]] static auto header_callback(char* data, std::size_t size, std::size_t count, void* context) noexcept
        -> std::size_t;

    [[nodiscard]] auto append_body(void const* data, std::size_t size, std::size_t count)
        -> jb::core::Result<std::size_t, HttpError>;
    [[nodiscard]] auto append_header(char const* data, std::size_t size, std::size_t count)
        -> jb::core::Result<std::size_t, HttpError>;
    [[nodiscard]] auto complete_header_block() -> jb::core::Result<void, HttpError>;
    void               record_callback_error(HttpError error) noexcept;
    [[nodiscard]] auto elapsed() const noexcept -> jb::core::Duration;
    void               populate_error_observation(HttpError& error);

    HttpRequestId                       _id;
    HttpRequest                         _request;
    HttpCompletionHandler               _completion;
    std::size_t                         _maximum_parsed_response_header_bytes{0};
    std::uint64_t                       _parsed_response_header_bytes{0};
    jb::net::detail::CaptureBuffer      _body_capture;
    jb::net::detail::CaptureBuffer      _current_raw_header_capture;
    std::optional<std::uint16_t>        _current_status_code;
    std::vector<HttpHeader>             _current_headers;
    std::optional<ParsedHeaderBlock>    _final_header_block;
    std::optional<HttpError>            _callback_error;
    std::byte                           _empty_body_storage{};
    CurlSlist                           _request_headers;
    CurlEasy                            _easy;
    jb::core::TimePoint                 _accepted_at;
    State                               _state{State::Running};
    HeaderState                         _header_state{HeaderState::AwaitingStatus};
    bool                                _accepted{false};
    bool                                _callback_failed_without_error{false};
    std::optional<HttpCompletionResult> _result;
};

} // namespace jb::net::http::detail
