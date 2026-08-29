#include "curl_request_priv.hpp"

#include "curl_error_priv.hpp"
#include "http_url_priv.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::net::http::detail {

namespace {

using RequestResult = jb::core::Result<std::unique_ptr<CurlRequest>, jb::core::Error>;

auto request_backend_unavailable(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.unavailable",
        .message  = "The system HTTP client could not prepare the request",
        .detail   = std::string{reason},
    };
}

auto request_internal_error(std::string_view reason = {}) -> HttpError
{
    return {
        .kind = HttpErrorKind::Internal,
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "net.http.internal",
                    .message  = "The HTTP request failed before detailed transport mapping was available",
                    .detail   = std::string{reason},
                    },
    };
}

auto request_protocol_error(std::string_view reason) -> HttpError
{
    return {
        .kind = HttpErrorKind::Protocol,
        .error =
            {
                    .category = jb::core::ErrorCategory::Io,
                    .code     = "net.http.protocol_error",
                    .message  = "The HTTP response was not usable",
                    .detail   = std::string{reason},
                    },
    };
}

auto request_cancelled_error() -> HttpError
{
    return {
        .kind = HttpErrorKind::Cancelled,
        .error =
            {
                    .category = jb::core::ErrorCategory::Cancelled,
                    .code     = "net.http.cancelled",
                    .message  = "The HTTP request was cancelled",
                    },
    };
}

auto request_redirect_error(std::string_view reason) -> HttpError
{
    return {
        .kind = HttpErrorKind::Redirect,
        .error =
            {
                    .category = jb::core::ErrorCategory::Io,
                    .code     = "net.http.redirect_failed",
                    .message  = "The HTTP redirect could not be followed safely",
                    .detail   = std::string{reason},
                    },
    };
}

auto request_redirect_error(jb::core::Error error) -> HttpError
{
    return {
        .kind  = HttpErrorKind::Redirect,
        .error = std::move(error),
    };
}

auto capture_error(jb::core::Error error) -> HttpError
{
    return {
        .kind  = HttpErrorKind::Internal,
        .error = std::move(error),
    };
}

template <typename Value>
auto set_option(CURL* easy, CURLoption option, Value value) noexcept -> bool
{
    return curl_easy_setopt(easy, option, value) == CURLE_OK;
}

auto append_slist(CurlSlist& list, std::string const& value) -> bool
{
    auto* previous = list.release();
    auto* appended = curl_slist_append(previous, value.c_str());
    if (!appended) {
        list.reset(previous);
        return false;
    }
    list.reset(appended);
    return true;
}

auto make_request_headers(HttpRequest const& request) -> jb::core::Result<CurlSlist, jb::core::Error>
{
    using Result = jb::core::Result<CurlSlist, jb::core::Error>;

    auto list = CurlSlist{};
    for (auto const& header : request.headers) {
        auto line = header.name + ": " + header.value;
        if (!append_slist(list, line)) {
            return Result::failure(request_backend_unavailable("request.header_allocation_failed"));
        }
    }

    // Suppress libcurl's automatic large-body Expect behavior while preserving the caller's validated fields.
    if (!append_slist(list, "Expect:")) {
        return Result::failure(request_backend_unavailable("request.header_allocation_failed"));
    }
    return Result::success(std::move(list));
}

constexpr auto ascii_lower(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](char left, char right) {
               return ascii_lower(static_cast<unsigned char>(left)) == ascii_lower(static_cast<unsigned char>(right));
           });
}

auto url_uses_tls(std::string_view url) noexcept -> bool
{
    auto const separator = url.find(':');
    return separator != std::string_view::npos && ascii_equal(url.substr(0U, separator), "https");
}

constexpr auto is_redirect_status(std::uint16_t status) noexcept -> bool
{
    return status == 301U || status == 302U || status == 303U || status == 307U || status == 308U;
}

auto is_credential_header(HttpHeader const& header) noexcept -> bool
{
    return ascii_equal(header.name, "Authorization") || ascii_equal(header.name, "Cookie") ||
           ascii_equal(header.name, "Proxy-Authorization");
}

auto redirect_location_count(std::vector<HttpHeader> const& headers) noexcept -> std::size_t
{
    return static_cast<std::size_t>(
        std::ranges::count_if(headers, [](HttpHeader const& header) { return ascii_equal(header.name, "Location"); }));
}

constexpr auto is_token_character(unsigned char value) noexcept -> bool
{
    if ((value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) ||
        (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9'))) {
        return true;
    }

    switch (value) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

auto is_token(std::string_view value) noexcept -> bool
{
    return !value.empty() && std::ranges::all_of(value, [](char character) {
        return is_token_character(static_cast<unsigned char>(character));
    });
}

auto parse_status_code(std::string_view line) -> jb::core::Result<std::uint16_t, HttpError>
{
    using Result = jb::core::Result<std::uint16_t, HttpError>;

    if (!line.starts_with("HTTP/")) {
        return Result::failure(request_protocol_error("response.invalid_status_line"));
    }
    auto const separator = line.find(' ');
    if (separator == std::string_view::npos || separator <= 5U || separator + 4U > line.size()) {
        return Result::failure(request_protocol_error("response.invalid_status_line"));
    }

    auto const code = line.substr(separator + 1U, 3U);
    if (!std::ranges::all_of(code, [](char character) { return character >= '0' && character <= '9'; }) ||
        (separator + 4U < line.size() && line[separator + 4U] != ' ' && line[separator + 4U] != '\t')) {
        return Result::failure(request_protocol_error("response.invalid_status_line"));
    }

    auto const value = static_cast<std::uint16_t>(((code[0] - '0') * 100) + ((code[1] - '0') * 10) + (code[2] - '0'));
    if (value < 100U || value > 599U) {
        return Result::failure(request_protocol_error("response.invalid_status_code"));
    }
    return Result::success(value);
}

auto trim_optional_whitespace(std::string_view value) noexcept -> std::string_view
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

auto valid_field_value(std::string_view value) noexcept -> bool
{
    return std::ranges::all_of(value, [](char character) {
        auto const byte = static_cast<unsigned char>(character);
        return (byte >= 0x20U || byte == static_cast<unsigned char>('\t')) && byte != 0x7fU;
    });
}

} // anonymous namespace

auto curl_timeout_milliseconds(jb::core::Duration remaining) -> jb::core::Result<long, jb::core::Error>
{
    using Result = jb::core::Result<long, jb::core::Error>;

    auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    if (milliseconds <= 0) {
        milliseconds = 1;
    }
    if (!std::in_range<long>(milliseconds)) {
        return Result::failure(request_backend_unavailable("request.timeout_conversion_failed"));
    }
    return Result::success(static_cast<long>(milliseconds));
}

void CurlEasyDeleter::operator()(CURL* easy) const noexcept
{
    curl_easy_cleanup(easy);
}

void CurlSlistDeleter::operator()(curl_slist* list) const noexcept
{
    curl_slist_free_all(list);
}

auto CurlRequest::create(HttpRequestId         id,
                         HttpRequest           request,
                         HttpCompletionHandler completion,
                         CurlTransferPolicy    policy,
                         std::size_t           maximum_parsed_response_header_bytes) -> RequestResult
{
    auto easy = CurlEasy{curl_easy_init()};
    if (!easy) {
        return RequestResult::failure(request_backend_unavailable("request.easy_initialization_failed"));
    }

    auto result = std::unique_ptr<CurlRequest>{
        new CurlRequest{id,
                        std::move(request),
                        std::move(completion),
                        std::move(policy),
                        maximum_parsed_response_header_bytes, std::move(easy)}
    };
    auto configured = result->configure_easy();
    if (!configured) {
        return RequestResult::failure(std::move(configured).error());
    }

    return RequestResult::success(std::move(result));
}

CurlRequest::CurlRequest(HttpRequestId         id,
                         HttpRequest           request,
                         HttpCompletionHandler completion,
                         CurlTransferPolicy    policy,
                         std::size_t           maximum_parsed_response_header_bytes,
                         CurlEasy              easy)
    : _id{id}
    , _request{std::move(request)}
    , _completion{std::move(completion)}
    , _policy{std::move(policy)}
    , _maximum_parsed_response_header_bytes{maximum_parsed_response_header_bytes}
    , _body_capture{_request.response_body_limit}
    , _current_raw_header_capture{_request.response_header_limit}
    , _easy{std::move(easy)}
    , _current_leg_uses_tls{url_uses_tls(_request.url)}
{}

auto CurlRequest::prepare_admission(jb::core::TimePoint accepted_at) -> jb::core::Result<void, jb::core::Error>
{
    using Result = jb::core::Result<void, jb::core::Error>;

    if (_request.timeout > jb::core::TimePoint::max() - accepted_at) {
        return Result::failure(request_backend_unavailable("request.deadline_overflow"));
    }

    _accepted_at = accepted_at;
    _deadline    = accepted_at + _request.timeout;
    return configure_remaining_timeout();
}

void CurlRequest::mark_accepted() noexcept
{
    _accepted = true;
}

void CurlRequest::prepare_completion(HttpCompletionResult result)
{
    _state  = State::PendingCompletion;
    _result = std::move(result);
}

auto CurlRequest::take_handler() -> HttpCompletionHandler
{
    return std::move(_completion);
}

auto CurlRequest::take_result() -> HttpCompletionResult
{
    return std::move(_result).value();
}

auto CurlRequest::cancellation_result() -> HttpCompletionResult
{
    auto error = request_cancelled_error();
    populate_error_observation(error);
    return HttpCompletionResult::failure(std::move(error));
}

auto CurlRequest::timeout_result() -> HttpCompletionResult
{
    auto error = map_curl_error(CURLE_OPERATION_TIMEDOUT);
    populate_error_observation(error);
    return HttpCompletionResult::failure(std::move(error));
}

auto CurlRequest::body_callback(char* data, std::size_t size, std::size_t count, void* context) noexcept -> std::size_t
{
    auto* request = static_cast<CurlRequest*>(context);
    if (!request || request->_callback_error || request->_callback_failed_without_error) {
        return 0U;
    }

    try {
        auto appended = request->append_body(data, size, count);
        if (!appended) {
            request->record_callback_error(std::move(appended).error());
            return 0U;
        }
        return *appended;
    }
    catch (...) {
        request->_callback_failed_without_error = true;
        return 0U;
    }
}

auto CurlRequest::header_callback(char* data, std::size_t size, std::size_t count, void* context) noexcept
    -> std::size_t
{
    auto* request = static_cast<CurlRequest*>(context);
    if (!request || request->_callback_error || request->_callback_failed_without_error) {
        return 0U;
    }

    try {
        auto appended = request->append_header(data, size, count);
        if (!appended) {
            request->record_callback_error(std::move(appended).error());
            return 0U;
        }
        return *appended;
    }
    catch (...) {
        request->_callback_failed_without_error = true;
        return 0U;
    }
}

auto CurlRequest::append_body(void const* data, std::size_t size, std::size_t count)
    -> jb::core::Result<std::size_t, HttpError>
{
    using Result = jb::core::Result<std::size_t, HttpError>;

    auto appended = _body_capture.append(data, size, count);
    if (!appended) {
        return Result::failure(capture_error(std::move(appended).error()));
    }
    return Result::success(*appended);
}

auto CurlRequest::append_header(char const* data, std::size_t size, std::size_t count)
    -> jb::core::Result<std::size_t, HttpError>
{
    using Result = jb::core::Result<std::size_t, HttpError>;

    // Count every informational, final, and trailer callback against the independent parser hard limit before any
    // bounded raw capture or parsed-field allocation is changed.
    auto byte_count = jb::net::detail::checked_capture_append_size(_parsed_response_header_bytes, size, count);
    if (!byte_count) {
        return Result::failure(capture_error(std::move(byte_count).error()));
    }
    auto const maximum = static_cast<std::uint64_t>(_maximum_parsed_response_header_bytes);
    if (_parsed_response_header_bytes > maximum || *byte_count > maximum - _parsed_response_header_bytes) {
        return Result::failure(request_protocol_error("response.headers_too_large"));
    }
    _parsed_response_header_bytes += static_cast<std::uint64_t>(*byte_count);

    auto line = std::string_view{data, *byte_count};
    if (line.size() < 2U || !line.ends_with("\r\n")) {
        return Result::failure(request_protocol_error("response.invalid_header_line"));
    }
    auto const content = line.substr(0, line.size() - 2U);

    // Each status line starts a new response block. Retaining only the last completed non-informational block also
    // keeps a future proxy CONNECT block from being confused with the origin response.
    if (content.starts_with("HTTP/")) {
        if (_header_state == HeaderState::Fields) {
            return Result::failure(request_protocol_error("response.incomplete_header_block"));
        }
        auto status = parse_status_code(content);
        if (!status) {
            return Result::failure(std::move(status).error());
        }

        static_cast<void>(_current_raw_header_capture.take());
        _current_headers.clear();
        _current_status_code = *status;
        _header_state        = HeaderState::Fields;
        auto captured        = _current_raw_header_capture.append(data, size, count);
        if (!captured) {
            return Result::failure(capture_error(std::move(captured).error()));
        }
        return Result::success(*byte_count);
    }

    // libcurl reports chunked trailers through the same callback after the final block. The public contract returns
    // only the final response status/header block, so trailers are counted against the parser cap and ignored here.
    if (_header_state == HeaderState::Complete) {
        return Result::success(*byte_count);
    }
    if (_header_state != HeaderState::Fields || !_current_status_code) {
        return Result::failure(request_protocol_error("response.header_without_status"));
    }

    auto captured = _current_raw_header_capture.append(data, size, count);
    if (!captured) {
        return Result::failure(capture_error(std::move(captured).error()));
    }
    if (content.empty()) {
        auto completed = complete_header_block();
        if (!completed) {
            return Result::failure(std::move(completed).error());
        }
        return Result::success(*byte_count);
    }
    if (content.front() == ' ' || content.front() == '\t') {
        return Result::failure(request_protocol_error("response.obsolete_header_folding"));
    }

    auto const separator = content.find(':');
    if (separator == std::string_view::npos || !is_token(content.substr(0, separator))) {
        return Result::failure(request_protocol_error("response.invalid_header_name"));
    }
    auto const value = trim_optional_whitespace(content.substr(separator + 1U));
    if (!valid_field_value(value)) {
        return Result::failure(request_protocol_error("response.invalid_header_value"));
    }
    _current_headers.push_back({
        .name      = std::string{content.substr(0, separator)},
        .value     = std::string{value},
        .sensitive = false,
    });
    return Result::success(*byte_count);
}

auto CurlRequest::complete_header_block() -> jb::core::Result<void, HttpError>
{
    using Result = jb::core::Result<void, HttpError>;

    if (!_current_status_code) {
        return Result::failure(request_protocol_error("response.header_without_status"));
    }

    auto const status = *_current_status_code;
    auto       raw    = _current_raw_header_capture.take();
    if (status < 100U || status >= 200U) {
        _final_header_block = ParsedHeaderBlock{
            .status_code = status,
            .headers     = std::move(_current_headers),
            .raw_headers = std::move(raw),
        };
    }
    else {
        _current_headers.clear();
    }
    _current_status_code.reset();
    _header_state = HeaderState::Complete;
    return Result::success();
}

auto CurlRequest::configure_easy() -> jb::core::Result<void, jb::core::Error>
{
    using Result = jb::core::Result<void, jb::core::Error>;

    auto headers = make_request_headers(_request);
    if (!headers) {
        return Result::failure(std::move(headers).error());
    }

    // Reapply the complete leg configuration so redirects cannot retain method state or lose TLS/proxy policy.
    // CurlRequest owns every string passed below until the detached easy handle is destroyed.
    curl_easy_reset(_easy.get());
    _request_headers = std::move(headers).value();
    auto configured  = set_option(_easy.get(), CURLOPT_URL, _request.url.c_str());
    if (_request.method == "HEAD") {
        configured = configured && set_option(_easy.get(), CURLOPT_NOBODY, 1L);
    }
    else if (_request.method == "GET" && !_request.body) {
        configured = configured && set_option(_easy.get(), CURLOPT_HTTPGET, 1L);
    }
    else {
        if (_request.body) {
            auto* body = _request.body->empty() ? reinterpret_cast<char*>(&_empty_body_storage)
                                                : reinterpret_cast<char*>(_request.body->data());
            configured =
                configured &&
                set_option(_easy.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(_request.body->size())) &&
                set_option(_easy.get(), CURLOPT_POSTFIELDS, body);
        }
        configured = configured && set_option(_easy.get(), CURLOPT_CUSTOMREQUEST, _request.method.c_str());
    }

    auto const* proxy              = _policy.proxy ? _policy.proxy->c_str() : "";
    auto const  verify_origin_peer = _request.verify_tls ? 1L : 0L;
    auto const  verify_origin_host = _request.verify_tls ? 2L : 0L;
    configured = configured && set_option(_easy.get(), CURLOPT_NOSIGNAL, 1L) &&
                 set_option(_easy.get(), CURLOPT_PROTOCOLS_STR, "http,https") &&
                 set_option(_easy.get(), CURLOPT_FOLLOWLOCATION, 0L) &&
                 set_option(_easy.get(), CURLOPT_ACCEPT_ENCODING, "") &&
                 set_option(_easy.get(), CURLOPT_PROXY, proxy) && set_option(_easy.get(), CURLOPT_NOPROXY, "") &&
                 set_option(_easy.get(), CURLOPT_SSL_VERIFYPEER, verify_origin_peer) &&
                 set_option(_easy.get(), CURLOPT_SSL_VERIFYHOST, verify_origin_host) &&
                 set_option(_easy.get(), CURLOPT_PROXY_SSL_VERIFYPEER, 1L) &&
                 set_option(_easy.get(), CURLOPT_PROXY_SSL_VERIFYHOST, 2L) &&
                 set_option(_easy.get(), CURLOPT_HTTPHEADER, _request_headers.get()) &&
                 set_option(_easy.get(), CURLOPT_WRITEFUNCTION, &CurlRequest::body_callback) &&
                 set_option(_easy.get(), CURLOPT_WRITEDATA, this) &&
                 set_option(_easy.get(), CURLOPT_HEADERFUNCTION, &CurlRequest::header_callback) &&
                 set_option(_easy.get(), CURLOPT_HEADERDATA, this) && set_option(_easy.get(), CURLOPT_PRIVATE, this);
    if (_policy.ca_bundle) {
        configured = configured && set_option(_easy.get(), CURLOPT_CAINFO, _policy.ca_bundle->c_str()) &&
                     set_option(_easy.get(), CURLOPT_PROXY_CAINFO, _policy.ca_bundle->c_str());
    }
    if (!configured) {
        return Result::failure(request_backend_unavailable("request.easy_configuration_failed"));
    }
    return Result::success();
}

auto CurlRequest::configure_remaining_timeout() -> jb::core::Result<void, jb::core::Error>
{
    using Result = jb::core::Result<void, jb::core::Error>;

    auto timeout = curl_timeout_milliseconds(_deadline - jb::core::Clock::now());
    if (!timeout) {
        return Result::failure(std::move(timeout).error());
    }
    if (!set_option(_easy.get(), CURLOPT_TIMEOUT_MS, *timeout) ||
        !set_option(_easy.get(), CURLOPT_CONNECTTIMEOUT_MS, *timeout)) {
        return Result::failure(request_backend_unavailable("request.timeout_configuration_failed"));
    }
    return Result::success();
}

auto CurlRequest::redirect_location(std::vector<HttpHeader> const& headers) const
    -> jb::core::Result<std::string_view, HttpError>
{
    using Result = jb::core::Result<std::string_view, HttpError>;

    auto location = std::optional<std::string_view>{};
    for (auto const& header : headers) {
        if (!ascii_equal(header.name, "Location")) {
            continue;
        }
        if (location) {
            return Result::failure(request_redirect_error("redirect.invalid_location"));
        }
        location = header.value;
    }
    if (!location) {
        return Result::failure(request_redirect_error("redirect.missing_location"));
    }
    return Result::success(*location);
}

auto CurlRequest::prepare_redirect_leg(std::string_view location) -> jb::core::Result<void, HttpError>
{
    using Result = jb::core::Result<void, HttpError>;

    auto const status = _final_header_block->status_code;
    if (_redirect_count >= _request.max_redirects) {
        return Result::failure(request_redirect_error("redirect.limit_exceeded"));
    }

    auto target = resolve_redirect_target(_request.url, location);
    if (!target) {
        return Result::failure(request_redirect_error(std::move(target).error()));
    }
    auto const changes_to_get = ((status == 301U || status == 302U) && _request.method == "POST") ||
                                (status == 303U && _request.method != "HEAD");
    if (changes_to_get) {
        _request.method = "GET";
        _request.body.reset();
    }

    if (target->cross_origin) {
        // Once a credential crosses an origin boundary it remains absent from every later leg, including a return hop.
        std::erase_if(_request.headers,
                      [](HttpHeader const& header) { return header.sensitive || is_credential_header(header); });
    }
    _request.url          = std::move(target->url);
    _current_leg_uses_tls = target->uses_tls;

    auto configured = configure_easy();
    if (configured) {
        configured = configure_remaining_timeout();
    }
    if (!configured) {
        return Result::failure(request_internal_error("request.redirect_configuration_failed"));
    }

    ++_redirect_count;
    reset_response_state();
    return Result::success();
}

void CurlRequest::reset_response_state()
{
    _parsed_response_header_bytes = 0U;
    static_cast<void>(_body_capture.take());
    static_cast<void>(_current_raw_header_capture.take());
    _current_status_code.reset();
    _current_headers.clear();
    _final_header_block.reset();
    _callback_error.reset();
    _callback_failed_without_error = false;
    _header_state                  = HeaderState::AwaitingStatus;
}

void CurlRequest::record_callback_error(HttpError error) noexcept
{
    if (_callback_error) {
        return;
    }
    try {
        _callback_error = std::move(error);
    }
    catch (...) {
        // Preserve a non-allocating fallback so the short callback result still maps to a safe request-local failure.
        _callback_failed_without_error = true;
    }
}

auto CurlRequest::elapsed() const noexcept -> jb::core::Duration
{
    if (!_accepted) {
        return {};
    }
    return jb::core::Clock::now() - _accepted_at;
}

void CurlRequest::populate_error_observation(HttpError& error)
{
    error.body           = _body_capture.take();
    error.redirect_count = _redirect_count;
    error.elapsed        = elapsed();
    error.tls_verified.reset();

    if (_current_status_code && (*_current_status_code < 100U || *_current_status_code >= 200U)) {
        error.status_code = _current_status_code;
        error.headers     = std::move(_current_headers);
        error.raw_headers = _current_raw_header_capture.take();
        return;
    }
    if (_final_header_block) {
        error.status_code = _final_header_block->status_code;
        error.headers     = std::move(_final_header_block->headers);
        error.raw_headers = std::move(_final_header_block->raw_headers);
    }
}

auto CurlRequest::transfer_result(CURLcode result) -> std::optional<HttpCompletionResult>
{
    if (_callback_failed_without_error) {
        auto error = request_internal_error("response.callback_failed");
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }
    if (_callback_error) {
        auto error = std::move(*_callback_error);
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }
    if (deadline_expired(jb::core::Clock::now())) {
        return timeout_result();
    }

    auto location = std::optional<std::string_view>{};
    if (_current_status_code && _request.follow_redirects && is_redirect_status(*_current_status_code) &&
        redirect_location_count(_current_headers) > 1U) {
        auto error = request_redirect_error("redirect.invalid_location");
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }
    if (_current_status_code && _request.follow_redirects && is_redirect_status(*_current_status_code) &&
        result == CURLE_WEIRD_SERVER_REPLY) {
        // Some libcurl builds reject duplicate or otherwise ambiguous Location fields before completing the header
        // block. Once a redirect status is observed, retain that as a redirect-policy failure rather than exposing a
        // backend-specific protocol classification.
        auto error = request_redirect_error("redirect.invalid_location");
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }
    if (_final_header_block && _request.follow_redirects && is_redirect_status(_final_header_block->status_code)) {
        auto candidate = redirect_location(_final_header_block->headers);
        if (!candidate) {
            auto error = std::move(candidate).error();
            populate_error_observation(error);
            return HttpCompletionResult::failure(std::move(error));
        }
        location = *candidate;
    }
    if (result != CURLE_OK) {
        auto error = map_curl_error(result);
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }
    if (!_final_header_block) {
        auto error = request_protocol_error("response.missing_final_header_block");
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }

    long status_code{0};
    if (curl_easy_getinfo(_easy.get(), CURLINFO_RESPONSE_CODE, &status_code) != CURLE_OK || status_code < 100L ||
        status_code > 599L || static_cast<std::uint16_t>(status_code) != _final_header_block->status_code) {
        auto error = request_protocol_error("response.status_mismatch");
        populate_error_observation(error);
        return HttpCompletionResult::failure(std::move(error));
    }

    if (location) {
        auto redirected = prepare_redirect_leg(*location);
        if (!redirected) {
            auto error = std::move(redirected).error();
            populate_error_observation(error);
            return HttpCompletionResult::failure(std::move(error));
        }
        return std::nullopt;
    }

    auto headers = std::move(*_final_header_block);
    return HttpCompletionResult::success({
        .status_code    = headers.status_code,
        .headers        = std::move(headers.headers),
        .body           = _body_capture.take(),
        .raw_headers    = std::move(headers.raw_headers),
        .redirect_count = _redirect_count,
        .elapsed        = elapsed(),
        .tls_verified   = _current_leg_uses_tls ? std::optional<bool>{_request.verify_tls} : std::nullopt,
    });
}

} // namespace jb::net::http::detail
