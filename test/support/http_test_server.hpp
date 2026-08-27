#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jb::test {

/// Minimal Stage 5.3 loopback HTTP server with an explicit response barrier.
class HttpTestServer final {
public:
    HttpTestServer();
    ~HttpTestServer();

    HttpTestServer(HttpTestServer const&)                    = delete;
    HttpTestServer(HttpTestServer&&)                         = delete;
    auto operator=(HttpTestServer const&) -> HttpTestServer& = delete;
    auto operator=(HttpTestServer&&) -> HttpTestServer&      = delete;

    [[nodiscard]] auto url(std::string path = "/") const -> std::string;
    [[nodiscard]] auto wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) -> bool;
    void               release_responses();

private:
    void accept_connections(int listen_fd);
    void handle_connection(int connection_fd);

    int                       _listen_fd{-1};
    std::uint16_t             _port{0};
    std::jthread              _accept_thread;
    std::vector<std::jthread> _connection_threads;
    std::mutex                _mutex;
    std::condition_variable   _condition;
    std::size_t               _request_count{0};
    bool                      _responses_released{false};
    bool                      _stopping{false};
};

} // namespace jb::test
