#pragma once

#include "event_loop_types.hpp"
#include "io_device_priv.hpp"
#include "local_socket.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace jb::net {

struct LocalSocket::Private : jb::core::priv::IODevicePrivate {
    LocalSocketState      state{LocalSocketState::Unconnected};
    std::filesystem::path server_path;
    LocalPeerCredentials  peer_credentials;
    std::string           input_buffer;
    std::string           output_buffer;
    std::size_t           read_buffer_limit{0};
    int                   fd{-1};
    jb::core::FdWatch     watch;
    std::uint64_t         generation{0};

    [[nodiscard]] auto release_socket(LocalSocket& socket) -> bool;
    void               close_lifecycle(LocalSocket& socket, bool emit_disconnected);
    void fail_lifecycle(LocalSocket& socket, jb::core::IOError error, std::string message, bool emit_disconnected);
    void
    handle_fd_event(LocalSocket& socket, int ready_fd, std::uint64_t callback_generation, jb::core::FdEvents events);
    void               complete_connection(LocalSocket& socket);
    void               read_available(LocalSocket& socket);
    void               write_pending(LocalSocket& socket);
    [[nodiscard]] auto update_watch(LocalSocket& socket) -> bool;
};

} // namespace jb::net
