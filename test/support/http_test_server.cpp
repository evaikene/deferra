#include "http_test_server.hpp"

#include "http_test_certificates.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
// Supplies the POSIX signal-set API paired with pthread_sigmask below.
#include <csignal> // IWYU pragma: keep
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>

namespace jb::test {

struct HttpTestTlsContext {
    struct Deleter {
        void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
    };

    std::unique_ptr<SSL_CTX, Deleter> context;
};

namespace {

constexpr std::size_t kMaximumTestRequestHeaderBytes{std::size_t{1024} * 1024U};
constexpr std::size_t kMaximumTestRequestBodyBytes{std::size_t{8} * 1024U * 1024U};

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags{MSG_NOSIGNAL};
#else
constexpr int kSendFlags{0};
#endif

void close_fd(int fd) noexcept
{
    if (fd >= 0) {
        static_cast<void>(::close(fd));
    }
}

auto socket_error(std::string_view operation) -> std::system_error
{
    return {errno, std::generic_category(), std::string{operation}};
}

struct SslDeleter {
    void operator()(SSL* session) const noexcept { SSL_free(session); }
};

using SslSession = std::unique_ptr<SSL, SslDeleter>;

struct HttpTestStream {
    int  fd{-1};
    SSL* tls{nullptr};
};

auto make_tls_context(HttpTestTransport transport) -> std::unique_ptr<HttpTestTlsContext>
{
    if (transport == HttpTestTransport::Plain) {
        return {};
    }

    auto result = std::make_unique<HttpTestTlsContext>();
    result->context.reset(SSL_CTX_new(TLS_server_method()));
    auto const cert_pem =
        transport == HttpTestTransport::Tls ? kHttpTestServerCertificate : kHttpTestMismatchedCertificate;
    auto const key_pem =
        transport == HttpTestTransport::Tls ? kHttpTestServerPrivateKey : kHttpTestMismatchedPrivateKey;
    auto cert_bio =
        std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size())),
                                                  &BIO_free};
    auto key_bio =
        std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size())),
                                                  &BIO_free};
    auto certificate = std::unique_ptr<X509, decltype(&X509_free)>{
        cert_bio ? PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr) : nullptr,
        &X509_free};
    auto private_key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>{
        key_bio ? PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr) : nullptr,
        &EVP_PKEY_free};

    if (!result->context || !certificate || !private_key ||
        SSL_CTX_set_min_proto_version(result->context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate(result->context.get(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(result->context.get(), private_key.get()) != 1 ||
        SSL_CTX_check_private_key(result->context.get()) != 1) {
        throw std::runtime_error{"could not initialize loopback TLS fixture"};
    }
    return result;
}

auto accept_tls(HttpTestTlsContext& context, int fd) -> SslSession
{
    auto session = SslSession{SSL_new(context.context.get())};
    if (!session || SSL_set_fd(session.get(), fd) != 1 || SSL_accept(session.get()) != 1) {
        return {};
    }
    return session;
}

void block_tls_sigpipe() noexcept
{
    auto signals = sigset_t{};
    if (sigemptyset(&signals) == 0 && sigaddset(&signals, SIGPIPE) == 0) {
        static_cast<void>(pthread_sigmask(SIG_BLOCK, &signals, nullptr));
    }
}

auto send_all(HttpTestStream const& stream, void const* data, std::size_t size) noexcept -> bool
{
    auto const* bytes = static_cast<char const*>(data);
    auto        sent  = std::size_t{0};
    while (sent < size) {
        if (stream.tls) {
            auto written = std::size_t{0};
            auto result  = SSL_write_ex(stream.tls, bytes + sent, size - sent, &written);
            if (result == 1) {
                sent += written;
                continue;
            }
            auto const error = SSL_get_error(stream.tls, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
                (error == SSL_ERROR_SYSCALL && errno == EINTR)) {
                continue;
            }
            return false;
        }

        auto const count = ::send(stream.fd, bytes + sent, size - sent, kSendFlags);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

auto receive_more(HttpTestStream const& stream, std::string& pending) -> bool
{
    for (;;) {
        auto buffer = std::array<char, 4096>{};
        if (stream.tls) {
            auto received = std::size_t{0};
            auto result   = SSL_read_ex(stream.tls, buffer.data(), buffer.size(), &received);
            if (result == 1) {
                pending.append(buffer.data(), received);
                return true;
            }
            auto const error = SSL_get_error(stream.tls, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
                (error == SSL_ERROR_SYSCALL && errno == EINTR)) {
                continue;
            }
            return false;
        }

        auto const count = ::recv(stream.fd, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            pending.append(buffer.data(), static_cast<std::size_t>(count));
            return true;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
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

auto parse_request_headers(std::string_view block, HttpTestRequest& request, std::optional<std::size_t>& content_length)
    -> bool
{
    auto const request_line_end = block.find("\r\n");
    auto const request_line     = block.substr(0, request_line_end);
    auto const first_space      = request_line.find(' ');
    auto const second_space =
        first_space == std::string_view::npos ? std::string_view::npos : request_line.find(' ', first_space + 1U);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos || first_space == 0U ||
        second_space == first_space + 1U || second_space + 1U >= request_line.size()) {
        return false;
    }

    request.method  = request_line.substr(0, first_space);
    request.target  = request_line.substr(first_space + 1U, second_space - first_space - 1U);
    request.version = request_line.substr(second_space + 1U);

    if (request_line_end == std::string_view::npos) {
        return true;
    }
    auto remaining = block.substr(request_line_end + 2U);
    while (!remaining.empty()) {
        auto const line_end = remaining.find("\r\n");
        auto const line     = remaining.substr(0, line_end);
        auto const colon    = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return false;
        }

        auto header = HttpTestHeader{
            .name  = std::string{line.substr(0, colon)},
            .value = std::string{trim_optional_whitespace(line.substr(colon + 1U))},
        };
        if (ascii_equal(header.name, "Content-Length")) {
            if (content_length) {
                return false;
            }
            auto parsed = std::size_t{0};
            auto result = std::from_chars(header.value.data(), header.value.data() + header.value.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != header.value.data() + header.value.size() ||
                parsed > kMaximumTestRequestBodyBytes) {
                return false;
            }
            content_length = parsed;
        }
        else if (ascii_equal(header.name, "Transfer-Encoding")) {
            return false;
        }
        request.headers.push_back(std::move(header));

        if (line_end == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(line_end + 2U);
    }
    return true;
}

auto read_request_headers(HttpTestStream const&       stream,
                          std::string&                pending,
                          HttpTestRequest&            request,
                          std::optional<std::size_t>& content_length) -> bool
{
    auto header_end = pending.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        if (pending.size() >= kMaximumTestRequestHeaderBytes || !receive_more(stream, pending)) {
            return false;
        }
        header_end = pending.find("\r\n\r\n");
    }
    if (header_end + 4U > kMaximumTestRequestHeaderBytes) {
        return false;
    }

    if (!parse_request_headers(std::string_view{pending}.substr(0, header_end), request, content_length)) {
        return false;
    }
    pending.erase(0U, header_end + 4U);
    return true;
}

auto read_request_body(HttpTestStream const& stream,
                       std::string&          pending,
                       HttpTestRequest&      request,
                       std::size_t           body_size) -> bool
{
    while (pending.size() < body_size) {
        if (!receive_more(stream, pending)) {
            return false;
        }
    }

    auto const body  = std::string_view{pending}.substr(0U, body_size);
    auto const bytes = jb::core::as_bytes(body);
    request.body.assign(bytes.begin(), bytes.end());
    pending.erase(0U, body_size);
    return true;
}

auto contains_header(std::vector<HttpTestHeader> const& headers, std::string_view name) -> bool
{
    return std::ranges::any_of(headers,
                               [name](HttpTestHeader const& header) { return ascii_equal(header.name, name); });
}

void append_header_block(std::string&                       output,
                         std::uint16_t                      status_code,
                         std::string_view                   reason,
                         std::vector<HttpTestHeader> const& headers)
{
    output += "HTTP/1.1 ";
    output += std::to_string(status_code);
    output += ' ';
    output += reason;
    output += "\r\n";
    for (auto const& header : headers) {
        output += header.name;
        output += ": ";
        output += header.value;
        output += "\r\n";
    }
    output += "\r\n";
}

void append_body_bytes(std::string& output, jb::core::ByteView body)
{
    output.append(reinterpret_cast<char const*>(body.data()), body.size());
}

void append_chunked_body(std::string& output, jb::core::ByteView body, std::size_t requested_chunk_size)
{
    auto const chunk_size = std::max<std::size_t>(requested_chunk_size, 1U);
    while (!body.empty()) {
        auto const count     = std::min(chunk_size, body.size());
        auto       size      = std::array<char, 2U * sizeof(std::size_t)>{};
        auto const converted = std::to_chars(size.data(), size.data() + size.size(), count, 16);
        output.append(size.data(), converted.ptr);
        output += "\r\n";
        append_body_bytes(output, body.first(count));
        output += "\r\n";
        body    = body.subspan(count);
    }
    output += "0\r\n\r\n";
}

struct EncodedResponse {
    std::string bytes;
    std::size_t body_offset{0};
};

auto encode_response(HttpTestResponse const& response, bool head_request) -> EncodedResponse
{
    auto output = std::string{};
    for (auto const& informational : response.informational) {
        append_header_block(output, informational.status_code, informational.reason, informational.headers);
    }

    auto headers = response.headers;
    if (response.framing == HttpTestBodyFraming::Chunked) {
        if (!contains_header(headers, "Transfer-Encoding")) {
            headers.push_back({.name = "Transfer-Encoding", .value = "chunked"});
        }
    }
    else if (!contains_header(headers, "Content-Length")) {
        headers.push_back({.name = "Content-Length", .value = std::to_string(response.body.size())});
    }
    if (!contains_header(headers, "Connection")) {
        headers.push_back({
            .name  = "Connection",
            .value = response.keep_alive ? "keep-alive" : "close",
        });
    }
    append_header_block(output, response.status_code, response.reason, headers);
    auto const body_offset = output.size();

    if (head_request) {
        return {.bytes = std::move(output), .body_offset = body_offset};
    }
    if (response.framing == HttpTestBodyFraming::Chunked) {
        append_chunked_body(output, response.body, response.chunk_size);
    }
    else {
        append_body_bytes(output, response.body);
    }
    return {.bytes = std::move(output), .body_offset = body_offset};
}

auto default_response() -> HttpTestResponse
{
    auto response = HttpTestResponse{};
    auto body     = jb::core::as_bytes("test-response");
    response.body.assign(body.begin(), body.end());
    return response;
}

auto bind_loopback_port(int fd, HttpTestAddressFamily address_family) -> std::optional<std::uint16_t>
{
    if (address_family == HttpTestAddressFamily::Ipv4) {
        auto address            = sockaddr_in{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            return std::nullopt;
        }

        auto address_size = socklen_t{sizeof(address)};
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
            return std::nullopt;
        }
        return ntohs(address.sin_port);
    }

    auto address        = sockaddr_in6{};
    address.sin6_family = AF_INET6;
    address.sin6_addr   = in6addr_loopback;
    address.sin6_port   = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        return std::nullopt;
    }

    auto address_size = socklen_t{sizeof(address)};
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        return std::nullopt;
    }
    return ntohs(address.sin6_port);
}

} // anonymous namespace

HttpTestServer::HttpTestServer(HttpTestTransport transport, HttpTestAddressFamily address_family)
    : _transport{transport}
    , _address_family{address_family}
    , _tls_context{make_tls_context(transport)}
{
    auto const native_family = address_family == HttpTestAddressFamily::Ipv4 ? AF_INET : AF_INET6;
    _listen_fd               = ::socket(native_family, SOCK_STREAM, 0);
    if (_listen_fd < 0) {
        throw socket_error("socket");
    }

    auto reuse_address = 1;
    if (::setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        auto error = socket_error("setsockopt");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }

    auto const port = bind_loopback_port(_listen_fd, address_family);
    if (!port || ::listen(_listen_fd, 8) < 0) {
        auto error = socket_error(port ? "listen" : "bind/getsockname");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }
    _port = *port;

    auto const listen_fd = _listen_fd;
    _accept_thread       = std::jthread{[this, listen_fd]() -> void { accept_connections(listen_fd); }};
}

HttpTestServer::~HttpTestServer()
{
    {
        std::lock_guard lock{_mutex};
        _stopping           = true;
        _responses_released = true;
        // Keep membership locked so a handler cannot close and allow descriptor reuse before shutdown reaches it.
        for (auto fd : _connection_fds) {
            static_cast<void>(::shutdown(fd, SHUT_RDWR));
        }
    }
    _condition.notify_all();

    auto const listen_fd = std::exchange(_listen_fd, -1);
    if (listen_fd >= 0) {
        static_cast<void>(::shutdown(listen_fd, SHUT_RDWR));
        close_fd(listen_fd);
    }
    if (_accept_thread.joinable()) {
        _accept_thread.join();
    }
    for (auto& connection : _connection_threads) {
        if (connection.joinable()) {
            connection.join();
        }
    }
}

auto HttpTestServer::url(std::string path) const -> std::string
{
    if (path.empty() || path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    auto const* const scheme = _transport == HttpTestTransport::Plain ? "http://" : "https://";
    auto const* const host   = _address_family == HttpTestAddressFamily::Ipv4 ? "127.0.0.1" : "[::1]";
    return std::string{scheme} + host + ':' + std::to_string(_port) + path;
}

void HttpTestServer::enqueue_response(HttpTestResponse response)
{
    std::lock_guard lock{_mutex};
    _responses.push_back(std::move(response));
}

auto HttpTestServer::wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) -> bool
{
    std::unique_lock lock{_mutex};
    return _condition.wait_for(lock, timeout, [this, count]() -> bool {
        return _requests.size() >= count || _stopping;
    }) && _requests.size() >= count;
}

auto HttpTestServer::requests() const -> std::vector<HttpTestRequest>
{
    std::lock_guard lock{_mutex};
    return _requests;
}

auto HttpTestServer::accepted_connection_count() const -> std::size_t
{
    std::lock_guard lock{_mutex};
    return _accepted_connections;
}

void HttpTestServer::release_responses()
{
    {
        std::lock_guard lock{_mutex};
        _responses_released = true;
    }
    _condition.notify_all();
}

void HttpTestServer::reset_next_request_after_headers()
{
    std::lock_guard lock{_mutex};
    _reset_next_request_after_headers = true;
}

auto HttpTestServer::wait_for_request_headers(std::size_t count, std::chrono::milliseconds timeout) -> bool
{
    std::unique_lock lock{_mutex};
    return _condition.wait_for(lock, timeout, [this, count]() -> bool {
        return _request_headers_observed >= count || _stopping;
    }) && _request_headers_observed >= count;
}

auto HttpTestServer::wait_for_response_segments(std::size_t count, std::chrono::milliseconds timeout) -> bool
{
    std::unique_lock lock{_mutex};
    return _condition.wait_for(lock, timeout, [this, count]() -> bool {
        return _response_segments_waiting >= count || _stopping;
    }) && _response_segments_waiting >= count;
}

void HttpTestServer::release_response_segment()
{
    {
        std::lock_guard lock{_mutex};
        ++_response_segments_released;
    }
    _condition.notify_all();
}

auto HttpTestServer::wait_for_peer_closes(std::size_t count, std::chrono::milliseconds timeout) -> bool
{
    std::unique_lock lock{_mutex};
    return _condition.wait_for(lock, timeout, [this, count]() -> bool { return _peer_closes >= count || _stopping; }) &&
           _peer_closes >= count;
}

void HttpTestServer::accept_connections(int listen_fd)
{
    for (;;) {
        auto const connection_fd = ::accept(listen_fd, nullptr, nullptr);
        if (connection_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        {
            std::lock_guard lock{_mutex};
            if (_stopping) {
                close_fd(connection_fd);
                return;
            }
            _connection_fds.insert(connection_fd);
            ++_accepted_connections;
        }
        _connection_threads.emplace_back([this, connection_fd]() -> void { handle_connection(connection_fd); });
    }
}

void HttpTestServer::handle_connection(int connection_fd)
{
#if defined(SO_NOSIGPIPE)
    auto no_sigpipe = 1;
    static_cast<void>(::setsockopt(connection_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)));
#endif

    if (_tls_context) {
        // OpenSSL writes do not expose MSG_NOSIGNAL, so confine SIGPIPE blocking to this fixture connection thread.
        block_tls_sigpipe();
    }
    auto tls_session = _tls_context ? accept_tls(*_tls_context, connection_fd) : SslSession{};
    if (_tls_context && !tls_session) {
        std::lock_guard lock{_mutex};
        _connection_fds.erase(connection_fd);
        close_fd(connection_fd);
        ++_peer_closes;
        _condition.notify_all();
        return;
    }

    auto stream           = HttpTestStream{.fd = connection_fd, .tls = tls_session.get()};
    auto pending          = std::string{};
    auto reset_connection = false;
    for (;;) {
        auto request        = HttpTestRequest{};
        auto content_length = std::optional<std::size_t>{};
        if (!read_request_headers(stream, pending, request, content_length)) {
            break;
        }

        {
            std::lock_guard lock{_mutex};
            ++_request_headers_observed;
            _condition.notify_all();
            if (_reset_next_request_after_headers) {
                _reset_next_request_after_headers = false;
                reset_connection                  = true;
            }
        }
        if (reset_connection || !read_request_body(stream, pending, request, content_length.value_or(std::size_t{0}))) {
            break;
        }

        auto response = HttpTestResponse{};
        auto is_head  = false;
        {
            std::unique_lock lock{_mutex};
            is_head = request.method == "HEAD";
            _requests.push_back(std::move(request));
            response = _next_response < _responses.size() ? _responses[_next_response++] : default_response();
            _condition.notify_all();
            _condition.wait(lock, [this]() -> bool { return _responses_released || _stopping; });
            if (_stopping) {
                break;
            }
        }

        auto encoded     = encode_response(response, is_head);
        auto prefix_size = encoded.bytes.size();
        if (response.pause_after_response_bytes) {
            prefix_size = std::min(prefix_size, encoded.body_offset + *response.pause_after_response_bytes);
        }
        if (response.close_after_response_bytes) {
            prefix_size = std::min(prefix_size, encoded.body_offset + *response.close_after_response_bytes);
        }
        if (!send_all(stream, encoded.bytes.data(), prefix_size)) {
            break;
        }
        if (response.close_after_response_bytes) {
            break;
        }
        if (response.pause_after_response_bytes && prefix_size < encoded.bytes.size()) {
            std::unique_lock lock{_mutex};
            auto const       segment = ++_response_segments_waiting;
            _condition.notify_all();
            _condition.wait(lock,
                            [this, segment]() -> bool { return _response_segments_released >= segment || _stopping; });
            if (_stopping) {
                break;
            }
            lock.unlock();
            if (!send_all(stream, encoded.bytes.data() + prefix_size, encoded.bytes.size() - prefix_size)) {
                break;
            }
        }
        if (!response.keep_alive) {
            break;
        }
    }

    {
        std::lock_guard lock{_mutex};
        _connection_fds.erase(connection_fd);
    }
    if (reset_connection) {
        auto linger = ::linger{.l_onoff = 1, .l_linger = 0};
        static_cast<void>(::setsockopt(connection_fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)));
    }
    else {
        static_cast<void>(::shutdown(connection_fd, SHUT_RDWR));
    }
    close_fd(connection_fd);
    {
        std::lock_guard lock{_mutex};
        ++_peer_closes;
    }
    _condition.notify_all();
}

} // namespace jb::test
