#pragma once

#include "event_loop_types.hpp"
#include "local_server.hpp"
#include "object_priv.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace jb::net {

struct LocalServer::Private : jb::core::priv::ObjectPrivate {
    std::filesystem::path                    server_path;
    LocalServerOptions                       options;
    std::deque<std::unique_ptr<LocalSocket>> pending_connections;
    jb::core::IOError                        error{jb::core::IOError::NoError};
    std::string                              error_string;
    int                                      fd{-1};
    jb::core::FdWatch                        watch;
    jb::core::FdCallback                     accept_callback;
    std::uintmax_t                           path_device{0};
    std::uintmax_t                           path_inode{0};
    std::uint64_t                            generation{0};
    bool                                     listening{false};
    bool                                     owns_path{false};
};

} // namespace jb::net
