#pragma once

#include "protocol.hpp"
#include "system_info.hpp"

#include <optional>

namespace jb::jobu::detail {

[[nodiscard]] auto handle_system_info(SystemInfo const& info, std::optional<jb::rpc::JsonValue> const& params)
    -> jb::rpc::MethodResult;

} // namespace jb::jobu::detail
