#pragma once

#include <filesystem>
#include <sys/types.h>

namespace jb::test {

// Linked only into the isolated listener setup test, never the production net library.
auto local_server_group(std::filesystem::path const& path, gid_t group) -> int;
auto local_server_permissions(std::filesystem::path const& path, mode_t mode) -> int;
auto local_server_listen(int descriptor, int backlog) -> int;

} // namespace jb::test
