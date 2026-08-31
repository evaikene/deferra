#include "management_rpc.hpp"

#include <span>
#include <string_view>
#include <type_traits>

using RegistrationFunction = bool (*)(jb::rpc::Server&,
                                      jb::jobu::ManagementService&,
                                      jb::jobu::AttributeRegistry const&,
                                      jb::jobu::ManagementMutationHandler);

static_assert(std::is_same_v<decltype(&jb::jobu::register_management_methods), RegistrationFunction>);
static_assert(std::is_invocable_r_v<void, jb::jobu::ManagementMutationHandler>);
static_assert(std::is_same_v<decltype(jb::jobu::management_rpc_method_names()), std::span<std::string_view const>>);

int main()
{
    return jb::jobu::management_rpc_method_names().empty() ? 1 : 0;
}
