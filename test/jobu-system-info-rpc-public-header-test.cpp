#include "system_info_rpc.hpp"

#include <string_view>
#include <type_traits>

using RegistrationFunction = bool (*)(jb::rpc::Server&, jb::jobu::SystemInfo);

static_assert(std::is_same_v<decltype(&jb::jobu::register_system_info_method), RegistrationFunction>);
static_assert(std::is_same_v<decltype(jb::jobu::system_info_rpc_method_name()), std::string_view>);

int main()
{
    return jb::jobu::system_info_rpc_method_name() == "system.info" ? 0 : 1;
}
