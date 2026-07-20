#include "local_socket.hpp"

#include <concepts>
#include <cstddef>
#include <filesystem>
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

    auto credentials = jb::net::LocalPeerCredentials{};
    return credentials.process_id || credentials.user_id || credentials.group_id ? 1 : 0;
}
