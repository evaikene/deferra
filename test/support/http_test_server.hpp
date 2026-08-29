#pragma once

#include "byte_buffer.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace jb::test {

struct HttpTestTlsContext;

enum class HttpTestTransport : std::uint8_t {
    Plain,
    Tls,
    TlsMismatchedIdentity,
};

enum class HttpTestAddressFamily : std::uint8_t {
    Ipv4,
    Ipv6,
};

struct HttpTestHeader {
    std::string name;
    std::string value;
};

struct HttpTestRequest {
    std::string                 method;
    std::string                 target;
    std::string                 version;
    std::vector<HttpTestHeader> headers;
    jb::core::ByteBuffer        body;
};

struct HttpTestInformationalResponse {
    std::uint16_t               status_code{103};
    std::string                 reason{"Early Hints"};
    std::vector<HttpTestHeader> headers;
};

enum class HttpTestBodyFraming : std::uint8_t {
    ContentLength,
    Chunked,
};

struct HttpTestResponse {
    std::vector<HttpTestInformationalResponse> informational;
    std::uint16_t                              status_code{200};
    std::string                                reason{"OK"};
    std::vector<HttpTestHeader>                headers;
    jb::core::ByteBuffer                       body;
    HttpTestBodyFraming                        framing{HttpTestBodyFraming::ContentLength};
    std::size_t                                chunk_size{3};
    std::optional<std::size_t>                 pause_after_response_bytes;
    std::optional<std::size_t>                 close_after_response_bytes;
    bool                                       keep_alive{false};
};

/// Scripted loopback HTTP/1.1 server with request recording and an explicit response barrier.
class HttpTestServer final {
public:
    explicit HttpTestServer(HttpTestTransport     transport      = HttpTestTransport::Plain,
                            HttpTestAddressFamily address_family = HttpTestAddressFamily::Ipv4);
    ~HttpTestServer();

    HttpTestServer(HttpTestServer const&)                    = delete;
    HttpTestServer(HttpTestServer&&)                         = delete;
    auto operator=(HttpTestServer const&) -> HttpTestServer& = delete;
    auto operator=(HttpTestServer&&) -> HttpTestServer&      = delete;

    [[nodiscard]] auto url(std::string path = "/") const -> std::string;
    void               enqueue_response(HttpTestResponse response);
    [[nodiscard]] auto wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) -> bool;
    [[nodiscard]] auto requests() const -> std::vector<HttpTestRequest>;
    [[nodiscard]] auto accepted_connection_count() const -> std::size_t;
    void               release_responses();
    void               reset_next_request_after_headers();
    [[nodiscard]] auto wait_for_request_headers(std::size_t count, std::chrono::milliseconds timeout) -> bool;
    [[nodiscard]] auto wait_for_response_segments(std::size_t count, std::chrono::milliseconds timeout) -> bool;
    void               release_response_segment();
    [[nodiscard]] auto wait_for_peer_closes(std::size_t count, std::chrono::milliseconds timeout) -> bool;

private:
    void accept_connections(int listen_fd);
    void handle_connection(int connection_fd);

    int                                 _listen_fd{-1};
    std::uint16_t                       _port{0};
    HttpTestTransport                   _transport{HttpTestTransport::Plain};
    HttpTestAddressFamily               _address_family{HttpTestAddressFamily::Ipv4};
    std::unique_ptr<HttpTestTlsContext> _tls_context;
    std::jthread                        _accept_thread;
    std::vector<std::jthread>           _connection_threads;
    mutable std::mutex                  _mutex;
    std::condition_variable             _condition;
    std::vector<HttpTestRequest>        _requests;
    std::vector<HttpTestResponse>       _responses;
    std::unordered_set<int>             _connection_fds;
    std::size_t                         _next_response{0};
    std::size_t                         _accepted_connections{0};
    std::size_t                         _request_headers_observed{0};
    std::size_t                         _response_segments_waiting{0};
    std::size_t                         _response_segments_released{0};
    std::size_t                         _peer_closes{0};
    bool                                _responses_released{false};
    bool                                _reset_next_request_after_headers{false};
    bool                                _stopping{false};
};

} // namespace jb::test
