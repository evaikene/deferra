#include "system_http_client.hpp"

#include "curl_multi_priv.hpp"
#include "curl_request_priv.hpp"
#include "curl_runtime_priv.hpp"
#include "http_validation_priv.hpp"
#include "object_priv.hpp"

#include <curl/curl.h>
#include <curl/urlapi.h>

#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jb::net::http {

namespace {

using ClientResult = jb::core::Result<std::unique_ptr<SystemHttpClient>, jb::core::Error>;
using VoidResult   = jb::core::Result<void, jb::core::Error>;

constexpr std::size_t kMaximumParsedResponseHeaderBytes{std::size_t{64} * 1024U * 1024U};

auto invalid_options(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "net.http.invalid_options",
        .message  = "The system HTTP client options are invalid",
        .detail   = std::string{reason},
    };
}

auto event_loop_unavailable() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.event_loop_unavailable",
        .message  = "The system HTTP client requires a valid current EventLoop",
    };
}

auto runtime_unavailable(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.runtime_unavailable",
        .message  = "The system HTTP runtime does not satisfy the requested configuration",
        .detail   = std::string{reason},
    };
}

auto unavailable() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = "net.http.unavailable",
        .message  = "The system HTTP transfer engine is unavailable",
    };
}

auto invalid_request(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "net.http.invalid_request",
        .message  = "HTTP request is invalid",
        .detail   = std::string{reason},
    };
}

auto identifier_exhausted() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::ResourceExhausted,
        .code     = "net.http.identifier_exhausted",
        .message  = "The system HTTP client exhausted its request identifiers",
    };
}

auto request_not_found() -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::NotFound,
        .code     = "net.http.request_not_found",
        .message  = "The HTTP request is not active",
    };
}

auto backend_completion(jb::core::Error const& error) -> jb::net::HttpCompletionResult
{
    return jb::net::HttpCompletionResult::failure({
        .kind  = jb::net::HttpErrorKind::Internal,
        .error = error,
    });
}

auto ascii_starts_with(std::string_view value, std::string_view prefix) noexcept -> bool
{
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        auto left  = static_cast<unsigned char>(value[index]);
        auto right = static_cast<unsigned char>(prefix[index]);
        if (left >= static_cast<unsigned char>('A') && left <= static_cast<unsigned char>('Z')) {
            left = static_cast<unsigned char>(left + ('a' - 'A'));
        }
        if (right >= static_cast<unsigned char>('A') && right <= static_cast<unsigned char>('Z')) {
            right = static_cast<unsigned char>(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

auto validate_stage_5_6_scope(jb::net::HttpRequest const& request, SystemHttpClientOptions const& options) -> VoidResult
{
    if (!request.verify_tls) {
        return VoidResult::failure(invalid_request("stage_5_6.unsafe_tls_not_supported"));
    }
    if (!ascii_starts_with(request.url, "http://")) {
        return VoidResult::failure(invalid_request("stage_5_6.https_not_supported"));
    }
    if (options.proxy) {
        return VoidResult::failure(invalid_request("stage_5_6.proxy_not_supported"));
    }
    return VoidResult::success();
}

auto validate_ca_bundle(std::filesystem::path const& path) -> VoidResult
{
    std::error_code error;
    auto const      status = std::filesystem::status(path, error);
    if (error) {
        return VoidResult::failure(invalid_options("ca_bundle.unavailable"));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return VoidResult::failure(invalid_options("ca_bundle.not_regular_file"));
    }

    auto input = std::ifstream{path, std::ios::binary};
    if (!input.is_open()) {
        return VoidResult::failure(invalid_options("ca_bundle.not_readable"));
    }
    return VoidResult::success();
}

struct CurlUrlDeleter {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};

struct CurlStringDeleter {
    void operator()(char* value) const noexcept { curl_free(value); }
};

using CurlUrl    = std::unique_ptr<CURLU, CurlUrlDeleter>;
using CurlString = std::unique_ptr<char, CurlStringDeleter>;

constexpr auto ascii_lower(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(lhs[index])) !=
            ascii_lower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

auto get_url_part(CURLU* url, CURLUPart part, CurlString& value) -> bool
{
    char*      raw_value{nullptr};
    auto const result = curl_url_get(url, part, &raw_value, 0);
    value.reset(raw_value);
    return result == CURLUE_OK;
}

auto has_userinfo_part(CURLU* url, CURLUPart part) -> jb::core::Result<bool, jb::core::Error>
{
    char*      raw_value{nullptr};
    auto const result = curl_url_get(url, part, &raw_value, 0);
    auto       value  = CurlString{raw_value};
    if (result == CURLUE_OK) {
        return jb::core::Result<bool, jb::core::Error>::success(true);
    }
    auto const missing_part = part == CURLUPART_USER ? CURLUE_NO_USER : CURLUE_NO_PASSWORD;
    if (result == missing_part) {
        return jb::core::Result<bool, jb::core::Error>::success(false);
    }
    return jb::core::Result<bool, jb::core::Error>::failure(invalid_options("proxy.invalid_url"));
}

auto validate_proxy(std::string const& proxy, detail::CurlRuntimeCapabilities const& capabilities) -> VoidResult
{
    if (proxy.empty() || proxy.find('\0') != std::string::npos) {
        return VoidResult::failure(invalid_options("proxy.invalid_url"));
    }

    auto url = CurlUrl{curl_url()};
    if (!url) {
        return VoidResult::failure(runtime_unavailable("runtime.url_parser_unavailable"));
    }
    if (curl_url_set(url.get(), CURLUPART_URL, proxy.c_str(), 0) != CURLUE_OK) {
        return VoidResult::failure(invalid_options("proxy.invalid_url"));
    }

    auto scheme = CurlString{};
    auto host   = CurlString{};
    if (!get_url_part(url.get(), CURLUPART_SCHEME, scheme) || !get_url_part(url.get(), CURLUPART_HOST, host) || !host ||
        std::string_view{host.get()}.empty()) {
        return VoidResult::failure(invalid_options("proxy.invalid_absolute_url"));
    }

    auto const is_http  = scheme && ascii_equal(scheme.get(), "http");
    auto const is_https = scheme && ascii_equal(scheme.get(), "https");
    if (!is_http && !is_https) {
        return VoidResult::failure(invalid_options("proxy.unsupported_scheme"));
    }

    auto user_present = has_userinfo_part(url.get(), CURLUPART_USER);
    if (!user_present) {
        return VoidResult::failure(std::move(user_present).error());
    }
    auto password_present = has_userinfo_part(url.get(), CURLUPART_PASSWORD);
    if (!password_present) {
        return VoidResult::failure(std::move(password_present).error());
    }
    if (*user_present || *password_present) {
        return VoidResult::failure(invalid_options("proxy.userinfo_forbidden"));
    }

    if (is_https && !capabilities.supports_https_proxy) {
        return VoidResult::failure(runtime_unavailable("runtime.https_proxy_unavailable"));
    }
    return VoidResult::success();
}

} // anonymous namespace

struct SystemHttpClient::Private : jb::core::priv::ObjectPrivate {
    struct CallbackState {
        Private* owner;
    };

    explicit Private(jb::core::EventLoop& loop_value, SystemHttpClientOptions value)
        : loop{loop_value}
        , options{std::move(value)}
        , callback_state{std::make_shared<CallbackState>(CallbackState{.owner = this})}
    {
        auto adapter = detail::CurlMultiAdapter::create(
            loop,
            [this](CURL* easy, CURLcode result) -> void { complete(easy, result); },
            [this](jb::core::Error error) -> void { fail(std::move(error)); },
            [this]() -> void { expire_due_requests(); });
        if (!adapter) {
            initialization_failure = std::move(adapter).error();
            return;
        }
        multi = std::move(adapter).value();
    }

    ~Private() override { shutdown(); }

    void shutdown() noexcept
    {
        disarm_deadline_timer();
        callback_state->owner = nullptr;
        multi.reset();
        requests.clear();
        owner = nullptr;
    }

    void disarm_deadline_timer() noexcept
    {
        if (deadline_timer) {
            loop.cancel_timer(deadline_timer);
            deadline_timer = {};
        }
        deadline_timer_at.reset();
    }

    [[nodiscard]] auto refresh_deadline_timer(std::optional<jb::core::TimePoint> provisional = {}) -> bool
    {
        if (!multi || !multi->is_available()) {
            disarm_deadline_timer();
            return true;
        }

        // Include a provisional admission so timer registration can fail before start() creates a callback obligation,
        // while every accepted Running request still shares the single earliest-deadline timer.
        auto earliest = provisional;
        for (auto const& [request_id, request] : requests) {
            (void)request_id;
            if (!request->accepted() || request->state() != detail::CurlRequest::State::Running) {
                continue;
            }
            if (!earliest || request->deadline() < *earliest) {
                earliest = request->deadline();
            }
        }

        if (!earliest) {
            disarm_deadline_timer();
            return true;
        }
        if (deadline_timer && deadline_timer_at == earliest) {
            return true;
        }

        disarm_deadline_timer();
        auto callback = callback_state;
        auto timer    = loop.post_at(*earliest, [callback = std::move(callback)]() -> void {
            if (callback->owner) {
                callback->owner->handle_deadline_timer();
            }
        });
        if (!timer) {
            return false;
        }
        deadline_timer    = timer;
        deadline_timer_at = *earliest;
        return true;
    }

    void refresh_deadline_timer_or_fail()
    {
        if (!refresh_deadline_timer()) {
            multi->fail_backend("event_loop.deadline_timer_failed");
        }
    }

    void handle_deadline_timer()
    {
        deadline_timer = {};
        deadline_timer_at.reset();
        expire_due_requests();
    }

    [[nodiscard]] auto start(jb::net::HttpRequest request, jb::net::HttpCompletionHandler completion)
        -> jb::core::Result<jb::net::HttpRequestId, jb::core::Error>
    {
        auto validation = jb::net::detail::validate_http_request(request);
        if (!validation) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(std::move(validation).error());
        }
        if (!completion) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(
                invalid_request("completion.empty"));
        }
        auto stage_scope = validate_stage_5_6_scope(request, options);
        if (!stage_scope) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(std::move(stage_scope).error());
        }
        if (!is_available()) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(unavailable());
        }
        if (identifiers_exhausted) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(identifier_exhausted());
        }

        auto const request_id = next_request_id;
        auto       prepared   = detail::CurlRequest::create(request_id,
                                                            std::move(request),
                                                            std::move(completion),
                                                            options.maximum_parsed_response_header_bytes);
        if (!prepared) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(std::move(prepared).error());
        }

        auto request_state = std::move(prepared).value();
        auto admission     = request_state->prepare_admission(jb::core::Clock::now());
        if (!admission) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(std::move(admission).error());
        }
        auto* easy                  = request_state->easy();
        auto [request_it, inserted] = requests.emplace(request_id, std::move(request_state));
        if (!inserted) {
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(identifier_exhausted());
        }
        try {
            requests_by_easy.emplace(easy, request_id);
        }
        catch (...) {
            requests.erase(request_it);
            throw;
        }

        auto added = multi->add(easy);
        if (!added) {
            requests_by_easy.erase(easy);
            requests.erase(request_it);
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(std::move(added).error());
        }

        if (!refresh_deadline_timer(request_it->second->deadline())) {
            multi->fail_backend("event_loop.deadline_timer_failed");
            requests.erase(request_it);
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(
                stored_failure.value_or(unavailable()));
        }

        auto queued = multi->queue_initial_drive();
        if (!queued) {
            requests_by_easy.erase(easy);
            requests.erase(request_it);
            return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::failure(std::move(queued).error());
        }

        request_it->second->mark_accepted();
        if (next_request_id == std::numeric_limits<jb::net::HttpRequestId>::max()) {
            identifiers_exhausted = true;
        }
        else {
            ++next_request_id;
        }
        return jb::core::Result<jb::net::HttpRequestId, jb::core::Error>::success(request_id);
    }

    void expire_due_requests()
    {
        if (!multi || !multi->is_available()) {
            return;
        }

        // Remove every expired easy handle before queuing its timeout completion so a delayed initial drive cannot
        // create network effects after the accepted absolute deadline.
        auto const now = jb::core::Clock::now();
        for (auto const& [request_id, request] : requests) {
            (void)request_id;
            if (!request->accepted() || request->state() != detail::CurlRequest::State::Running ||
                !request->deadline_expired(now)) {
                continue;
            }

            auto* easy    = request->easy();
            auto  removed = multi->remove(easy);
            if (!removed) {
                return;
            }
            requests_by_easy.erase(easy);
            queue_completion(*request, request->timeout_result());
        }
        refresh_deadline_timer_or_fail();
    }

    [[nodiscard]] auto cancel(jb::net::HttpRequestId request_id) -> VoidResult
    {
        // A monotonic deadline that has already elapsed wins even when its EventLoop timer has not yet been dispatched.
        expire_due_requests();
        auto it = requests.find(request_id);
        if (it == requests.end() || !it->second->accepted() ||
            it->second->state() != detail::CurlRequest::State::Running) {
            return VoidResult::failure(request_not_found());
        }

        auto* easy    = it->second->easy();
        auto  removed = multi->remove(easy);
        if (!removed) {
            return VoidResult::failure(std::move(removed).error());
        }
        requests_by_easy.erase(easy);
        auto completion = it->second->cancellation_result();
        queue_completion(*it->second, std::move(completion));
        refresh_deadline_timer_or_fail();
        return VoidResult::success();
    }

    [[nodiscard]] auto is_available() const noexcept -> bool
    {
        return multi && multi->is_available() && !stored_failure;
    }

    [[nodiscard]] auto active_request_count() const noexcept -> std::size_t
    {
        auto count = std::size_t{0};
        for (auto const& [id, request] : requests) {
            (void)id;
            count += request->accepted() ? 1U : 0U;
        }
        return count;
    }

    void complete(CURL* easy, CURLcode result)
    {
        auto const easy_it = requests_by_easy.find(easy);
        if (easy_it == requests_by_easy.end()) {
            multi->fail_backend("completion.unknown_request");
            return;
        }

        auto request_it = requests.find(easy_it->second);
        if (request_it == requests.end() || !request_it->second->accepted() ||
            request_it->second->state() != detail::CurlRequest::State::Running) {
            multi->fail_backend("completion.invalid_request_state");
            return;
        }

        auto completion = request_it->second->transfer_result(result);
        if (!completion) {
            // The adapter detached the completed leg before this callback. Re-add the reconfigured handle without an
            // owner-loop interleaving point where cancellation could observe Running state outside the multi handle.
            auto added = multi->add(easy);
            if (!added) {
                return;
            }
            auto queued = multi->queue_initial_drive();
            if (!queued) {
                return;
            }
            refresh_deadline_timer_or_fail();
            return;
        }

        requests_by_easy.erase(easy_it);
        queue_completion(*request_it->second, std::move(*completion));
        refresh_deadline_timer_or_fail();
    }

    void queue_completion(detail::CurlRequest& request, jb::net::HttpCompletionResult result)
    {
        request.prepare_completion(std::move(result));
        auto const request_id = request.id();
        auto       callback   = callback_state;
        auto       task       = [callback = std::move(callback), request_id]() -> void {
            if (callback->owner) {
                callback->owner->deliver(request_id);
            }
        };

        // A completion must remain asynchronous even when EventLoop wakeup has failed. The owner-thread timer is a
        // safe fallback because post_delayed() queues without invoking the callback inline.
        if (loop.post(task)) {
            return;
        }
        auto timer = loop.post_delayed(jb::core::Duration::zero(), std::move(task));
        if (!timer && !stored_failure) {
            multi->fail_backend("event_loop.completion_queue_failed");
        }
    }

    void deliver(jb::net::HttpRequestId request_id)
    {
        auto it = requests.find(request_id);
        if (it == requests.end() || !it->second->accepted() ||
            it->second->state() != detail::CurlRequest::State::PendingCompletion) {
            return;
        }

        auto handler = it->second->take_handler();
        auto result  = it->second->take_result();
        requests.erase(it);
        handler(request_id, std::move(result));
    }

    void fail(jb::core::Error error)
    {
        if (stored_failure) {
            return;
        }
        stored_failure = std::move(error);
        requests_by_easy.clear();
        disarm_deadline_timer();

        std::vector<jb::net::HttpRequestId> active;
        active.reserve(requests.size());
        for (auto const& [request_id, request] : requests) {
            if (request->accepted() && request->state() == detail::CurlRequest::State::Running) {
                active.push_back(request_id);
            }
        }
        for (auto request_id : active) {
            queue_completion(*requests.at(request_id), backend_completion(*stored_failure));
        }

        auto callback = callback_state;
        auto task     = [callback = std::move(callback)]() -> void {
            if (callback->owner) {
                callback->owner->emit_failure();
            }
        };
        if (!loop.post(task)) {
            static_cast<void>(loop.post_delayed(jb::core::Duration::zero(), std::move(task)));
        }
    }

    void emit_failure()
    {
        if (failure_emitted || !stored_failure || !owner) {
            return;
        }
        failure_emitted = true;
        owner->emit(owner->failed, *stored_failure);
    }

    // Retain the validated snapshot so transfer admission never borrows caller-owned configuration.
    SystemHttpClient*                                                                owner{nullptr};
    jb::core::EventLoop&                                                             loop;
    SystemHttpClientOptions                                                          options;
    std::shared_ptr<CallbackState>                                                   callback_state;
    std::unique_ptr<detail::CurlMultiAdapter>                                        multi;
    std::optional<jb::core::Error>                                                   initialization_failure;
    std::optional<jb::core::Error>                                                   stored_failure;
    jb::core::TimerHandle                                                            deadline_timer;
    std::optional<jb::core::TimePoint>                                               deadline_timer_at;
    std::unordered_map<jb::net::HttpRequestId, std::unique_ptr<detail::CurlRequest>> requests;
    std::unordered_map<CURL*, jb::net::HttpRequestId>                                requests_by_easy;
    jb::net::HttpRequestId                                                           next_request_id{1};
    bool                                                                             identifiers_exhausted{false};
    bool                                                                             failure_emitted{false};
};

auto SystemHttpClient::create(jb::core::EventLoop& loop, SystemHttpClientOptions options) -> ClientResult
{
    if (&loop != jb::core::EventLoop::current() || !loop.is_valid()) {
        return ClientResult::failure(event_loop_unavailable());
    }
    if (options.maximum_parsed_response_header_bytes == 0U ||
        options.maximum_parsed_response_header_bytes > kMaximumParsedResponseHeaderBytes) {
        return ClientResult::failure(invalid_options("maximum_parsed_response_header_bytes.out_of_range"));
    }
    if (options.ca_bundle) {
        auto ca_bundle = validate_ca_bundle(*options.ca_bundle);
        if (!ca_bundle) {
            return ClientResult::failure(std::move(ca_bundle).error());
        }
    }

    auto capabilities = detail::preflight_curl_runtime();
    if (!capabilities) {
        return ClientResult::failure(std::move(capabilities).error());
    }
    if (options.proxy) {
        auto proxy = validate_proxy(*options.proxy, *capabilities);
        if (!proxy) {
            return ClientResult::failure(std::move(proxy).error());
        }
    }

    auto client = std::unique_ptr<SystemHttpClient>{
        new SystemHttpClient{loop, std::move(options)}
    };
    auto* data = client->d_ptr<Private>();
    if (data->initialization_failure) {
        return ClientResult::failure(std::move(*data->initialization_failure));
    }
    return ClientResult::success(std::move(client));
}

SystemHttpClient::SystemHttpClient(jb::core::EventLoop& loop, SystemHttpClientOptions options)
    : HttpClient(*new Private{loop, std::move(options)})
{
    // Complete the back-reference only after the Object and HttpClient subobjects are fully constructed.
    d_ptr<Private>()->owner = this;
}

SystemHttpClient::~SystemHttpClient()
{
    // Disable backend callbacks before HttpClient destroys its signal members.
    d_ptr<Private>()->shutdown();
}

auto SystemHttpClient::is_available() const noexcept -> bool
{
    return d_ptr<Private const>()->is_available();
}

auto SystemHttpClient::start(jb::net::HttpRequest request, jb::net::HttpCompletionHandler completion)
    -> jb::core::Result<jb::net::HttpRequestId, jb::core::Error>
{
    return d_ptr<Private>()->start(std::move(request), std::move(completion));
}

auto SystemHttpClient::cancel(jb::net::HttpRequestId request_id) -> jb::core::Result<void, jb::core::Error>
{
    return d_ptr<Private>()->cancel(request_id);
}

auto SystemHttpClient::active_request_count() const noexcept -> std::size_t
{
    return d_ptr<Private const>()->active_request_count();
}

auto SystemHttpClient::failure() const -> std::optional<jb::core::Error>
{
    return d_ptr<Private const>()->stored_failure;
}

} // namespace jb::net::http
