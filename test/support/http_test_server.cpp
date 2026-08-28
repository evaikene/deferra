#include "http_test_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jb::test {

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

auto send_all(int fd, void const* data, std::size_t size) noexcept -> bool
{
    auto const* bytes = static_cast<char const*>(data);
    auto        sent  = std::size_t{0};
    while (sent < size) {
        auto const count = ::send(fd, bytes + sent, size - sent, kSendFlags);
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

auto receive_more(int fd, std::string& pending) -> bool
{
    for (;;) {
        auto       buffer = std::array<char, 4096>{};
        auto const count  = ::recv(fd, buffer.data(), buffer.size(), 0);
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

auto read_request(int fd, std::string& pending, HttpTestRequest& request) -> bool
{
    auto header_end = pending.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        if (pending.size() >= kMaximumTestRequestHeaderBytes || !receive_more(fd, pending)) {
            return false;
        }
        header_end = pending.find("\r\n\r\n");
    }
    if (header_end + 4U > kMaximumTestRequestHeaderBytes) {
        return false;
    }

    auto content_length = std::optional<std::size_t>{};
    if (!parse_request_headers(std::string_view{pending}.substr(0, header_end), request, content_length)) {
        return false;
    }
    auto const body_size    = content_length.value_or(0U);
    auto const message_size = header_end + 4U + body_size;
    while (pending.size() < message_size) {
        if (!receive_more(fd, pending)) {
            return false;
        }
    }

    auto const body  = std::string_view{pending}.substr(header_end + 4U, body_size);
    auto const bytes = jb::core::as_bytes(body);
    request.body.assign(bytes.begin(), bytes.end());
    pending.erase(0U, message_size);
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

auto encode_response(HttpTestResponse const& response, bool head_request) -> std::string
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

    if (head_request) {
        return output;
    }
    if (response.framing == HttpTestBodyFraming::Chunked) {
        append_chunked_body(output, response.body, response.chunk_size);
    }
    else {
        append_body_bytes(output, response.body);
    }
    return output;
}

auto default_response() -> HttpTestResponse
{
    auto response = HttpTestResponse{};
    auto body     = jb::core::as_bytes("test-response");
    response.body.assign(body.begin(), body.end());
    return response;
}

} // anonymous namespace

HttpTestServer::HttpTestServer()
{
    _listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listen_fd < 0) {
        throw socket_error("socket");
    }

    auto reuse_address = 1;
    if (::setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        auto error = socket_error("setsockopt");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }

    auto address            = sockaddr_in{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(_listen_fd, 8) < 0) {
        auto error = socket_error("bind/listen");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }

    auto address_size = socklen_t{sizeof(address)};
    if (::getsockname(_listen_fd, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        auto error = socket_error("getsockname");
        close_fd(std::exchange(_listen_fd, -1));
        throw error;
    }
    _port = ntohs(address.sin_port);

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
    return "http://127.0.0.1:" + std::to_string(_port) + path;
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

    auto pending = std::string{};
    for (;;) {
        auto request = HttpTestRequest{};
        if (!read_request(connection_fd, pending, request)) {
            break;
        }

        auto response = HttpTestResponse{};
        {
            std::unique_lock lock{_mutex};
            auto const       is_head = request.method == "HEAD";
            _requests.push_back(std::move(request));
            response = _next_response < _responses.size() ? _responses[_next_response++] : default_response();
            _condition.notify_all();
            _condition.wait(lock, [this]() -> bool { return _responses_released || _stopping; });
            if (_stopping) {
                break;
            }

            auto encoded = encode_response(response, is_head);
            lock.unlock();
            if (!send_all(connection_fd, encoded.data(), encoded.size())) {
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
    static_cast<void>(::shutdown(connection_fd, SHUT_RDWR));
    close_fd(connection_fd);
}

} // namespace jb::test
