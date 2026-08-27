#include "http/system_http_client.hpp"

#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>

using Client       = jb::net::http::SystemHttpClient;
using ClientResult = jb::core::Result<std::unique_ptr<Client>, jb::core::Error>;

static_assert(std::is_final_v<Client>);
static_assert(std::derived_from<Client, jb::net::HttpClient>);
static_assert(!std::is_copy_constructible_v<Client>);
static_assert(!std::is_move_constructible_v<Client>);
static_assert(std::is_same_v<decltype(&Client::create),
                             ClientResult (*)(jb::core::EventLoop&, jb::net::http::SystemHttpClientOptions)>);

auto main() -> int
{
    auto options = jb::net::http::SystemHttpClientOptions{};
    return !options.ca_bundle && !options.proxy &&
                   options.maximum_parsed_response_header_bytes == std::size_t{8} * 1024U * 1024U
             ? 0
             : 1;
}
