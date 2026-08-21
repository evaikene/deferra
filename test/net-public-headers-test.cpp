#include "local_server.hpp"
#include "local_socket.hpp"

#include <concepts>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

template <typename T>
concept LocalSocketApi = requires(T & socket, T const& const_socket, std::filesystem::path const& path)
{
    {socket.connect_to_server(path)}->std::same_as<void>;
    {socket.disconnect_from_server()}->std::same_as<void>;
    {socket.abort()}->std::same_as<void>;
    {const_socket.state()}->std::same_as<jb::net::LocalSocketState>;
    {const_socket.server_path()}->std::same_as<std::filesystem::path const&>;
    {const_socket.peer_credentials()}->std::same_as<jb::net::LocalPeerCredentials const&>;
    {socket.set_read_buffer_limit(std::size_t{1024})}->std::same_as<void>;
    {const_socket.read_buffer_limit()}->std::same_as<std::size_t>;
    {const_socket.is_open()}->std::same_as<bool>;
    {socket.close()}->std::same_as<void>;
    {socket.read(std::size_t{1})}->std::same_as<std::string>;
    {socket.read_all()}->std::same_as<std::string>;
    {socket.read_line()}->std::same_as<std::string>;
    {socket.can_read_line()}->std::same_as<bool>;
    {socket.write(std::string_view{})}->std::same_as<std::size_t>;
    {socket.bytes_available()}->std::same_as<std::size_t>;
};

template <typename T>
concept LocalServerApi =
    requires(T & server, T const& const_server, std::filesystem::path const& path, jb::net::LocalServerOptions options)
{
    {server.listen(path)}->std::same_as<bool>;
    {server.listen(path, options)}->std::same_as<bool>;
    {server.close()}->std::same_as<void>;
    {const_server.is_listening()}->std::same_as<bool>;
    {const_server.server_path()}->std::same_as<std::filesystem::path const&>;
    {const_server.pending_connection_count()}->std::same_as<std::size_t>;
    {server.take_next_connection()}->std::same_as<std::unique_ptr<jb::net::LocalSocket>>;
    {const_server.error()}->std::same_as<jb::core::IOError>;
    {const_server.error_string()}->std::same_as<std::string const&>;
};

} // anonymous namespace

auto main() -> int
{
    static_assert(std::is_base_of_v<jb::core::IODevice, jb::net::LocalSocket>);
    static_assert(std::is_final_v<jb::net::LocalSocket>);
    static_assert(!std::is_copy_constructible_v<jb::net::LocalSocket>);
    static_assert(!std::is_copy_assignable_v<jb::net::LocalSocket>);
    static_assert(!std::is_move_constructible_v<jb::net::LocalSocket>);
    static_assert(!std::is_move_assignable_v<jb::net::LocalSocket>);
    static_assert(LocalSocketApi<jb::net::LocalSocket>);

    static_assert(std::is_base_of_v<jb::core::Object, jb::net::LocalServer>);
    static_assert(std::is_final_v<jb::net::LocalServer>);
    static_assert(!std::is_copy_constructible_v<jb::net::LocalServer>);
    static_assert(!std::is_copy_assignable_v<jb::net::LocalServer>);
    static_assert(!std::is_move_constructible_v<jb::net::LocalServer>);
    static_assert(!std::is_move_assignable_v<jb::net::LocalServer>);
    static_assert(LocalServerApi<jb::net::LocalServer>);
    static_assert(std::same_as<decltype(jb::net::LocalServer::new_connection), jb::core::Signal<>>);
    static_assert(
        std::same_as<decltype(jb::net::LocalServer::accept_error), jb::core::Signal<jb::core::IOError, std::string>>);

    auto       credentials         = jb::net::LocalPeerCredentials{};
    auto       options             = jb::net::LocalServerOptions{};
    auto const default_permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    auto const defaults_are_valid  = options.permissions == default_permissions && options.backlog == 128 &&
                                     options.max_pending_connections == 64 &&
                                     options.accepted_read_buffer_limit == std::size_t{2} * 1024U * 1024U;
    return credentials.process_id || credentials.user_id || credentials.group_id || !defaults_are_valid ? 1 : 0;
}
