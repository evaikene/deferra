#pragma once

#include "event_loop_types.hpp"
#include "io_device_priv.hpp"
#include "tcp_socket.hpp"

namespace jb::net::priv {

struct TcpSocketPrivate : jb::core::priv::IODevicePrivate {
    SocketState       state{SocketState::Unconnected};
    std::string       peer_address;
    std::uint16_t     peer_port{0};
    std::string       input_buffer;
    std::string       output_buffer;
    int               fd{-1};
    jb::core::FdWatch watch;
};

} // namespace jb::net::priv
