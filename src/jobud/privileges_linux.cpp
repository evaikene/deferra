#include "privileges_priv.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace jb::jobud::detail {
namespace {

using OperationResult = jb::core::Result<void, jb::core::Error>;

auto failed(char const* message) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::PermissionDenied,
            .code     = "jobud.privilege.drop_failed",
            .message  = message};
}

// Bound NSS buffer growth and group projections, without retaining borrowed libc storage.
constexpr std::size_t kMaximumAccountBuffer = 1'048'576U;
constexpr int         kMaximumGroups        = 65'536;

// Linux's fixed capability v3 syscall ABI has two 32-bit data words. Declare only that ABI here:
// minimal musl installations provide syscall/prctl declarations without the optional kernel-header package.
struct CapabilityHeader {
    std::uint32_t version{0x20080522U};
    std::int32_t  pid{0};
};

struct CapabilityData {
    std::uint32_t effective{};
    std::uint32_t permitted{};
    std::uint32_t inheritable{};
};

static_assert(sizeof(CapabilityHeader) == 8U);
static_assert(sizeof(CapabilityData) == 12U);

class SystemPrivilegeOperations final : public PrivilegeOperations {
public:
    auto identity() -> jb::core::Result<ProcessIdentity, jb::core::Error> override
    {
        using IdentityResult = jb::core::Result<ProcessIdentity, jb::core::Error>;
        ProcessIdentity value;
        if (::getresuid(&value.real_user, &value.effective_user, &value.saved_user) != 0 ||
            ::getresgid(&value.real_group, &value.effective_group, &value.saved_group) != 0) {
            return IdentityResult::failure(failed("Unable to inspect daemon IDs"));
        }

        auto const count = ::getgroups(0, nullptr);
        if (count < 0 || count > kMaximumGroups) {
            return IdentityResult::failure(failed("Unable to inspect daemon supplementary groups"));
        }
        value.supplementary_groups.resize(static_cast<std::size_t>(count));
        auto const read = ::getgroups(count, value.supplementary_groups.data());
        if (read < 0 || read != count) {
            return IdentityResult::failure(failed("Daemon supplementary groups changed during inspection"));
        }
        return IdentityResult::success(std::move(value));
    }

    auto account(std::string const& name) -> jb::core::Result<DaemonAccount, jb::core::Error> override
    {
        using AccountResult = jb::core::Result<DaemonAccount, jb::core::Error>;
        std::vector<char> buffer(1024U);
        while (buffer.size() <= kMaximumAccountBuffer) {
            passwd     value{};
            passwd*    found{};
            auto const status = ::getpwnam_r(name.c_str(), &value, buffer.data(), buffer.size(), &found);
            if (status == ERANGE && buffer.size() < kMaximumAccountBuffer) {
                buffer.resize(buffer.size() * 2U);
                continue;
            }
            if (status != 0 || found == nullptr || found->pw_name == nullptr) {
                return AccountResult::failure(failed("Unable to resolve the requested daemon account"));
            }
            return AccountResult::success(
                {.name = found->pw_name, .user = found->pw_uid, .primary_group = found->pw_gid});
        }
        return AccountResult::failure(failed("Daemon account lookup exceeded its buffer limit"));
    }

    auto group(std::string const& name) -> jb::core::Result<gid_t, jb::core::Error> override
    {
        using GroupResult = jb::core::Result<gid_t, jb::core::Error>;
        std::vector<char> buffer(1024U);
        while (buffer.size() <= kMaximumAccountBuffer) {
            struct group  value{};
            struct group* found{};
            auto const    status = ::getgrnam_r(name.c_str(), &value, buffer.data(), buffer.size(), &found);
            if (status == ERANGE && buffer.size() < kMaximumAccountBuffer) {
                buffer.resize(buffer.size() * 2U);
                continue;
            }
            if (status != 0 || found == nullptr) {
                return GroupResult::failure(failed("Unable to resolve the requested daemon group"));
            }
            return GroupResult::success(found->gr_gid);
        }
        return GroupResult::failure(failed("Daemon group lookup exceeded its buffer limit"));
    }

    auto account_groups(DaemonAccount const& value, gid_t primary)
        -> jb::core::Result<std::vector<gid_t>, jb::core::Error> override
    {
        using GroupsResult       = jb::core::Result<std::vector<gid_t>, jb::core::Error>;
        auto               count = 16;
        std::vector<gid_t> groups(static_cast<std::size_t>(count));
        for (;;) {
            auto const capacity = count;
            if (::getgrouplist(value.name.c_str(), primary, groups.data(), &count) >= 0) {
                if (count < 0 || count > capacity) {
                    return GroupsResult::failure(failed("Daemon account group lookup returned an invalid size"));
                }
                groups.resize(static_cast<std::size_t>(count));
                return GroupsResult::success(std::move(groups));
            }
            if (count <= capacity || count > kMaximumGroups) {
                return GroupsResult::failure(failed("Unable to resolve daemon account supplementary groups"));
            }
            groups.resize(static_cast<std::size_t>(count));
        }
    }

    auto initialize_groups(std::string const& name, gid_t primary) -> OperationResult override
    {
        if (::initgroups(name.c_str(), primary) != 0) {
            return OperationResult::failure(failed("Unable to initialize daemon supplementary groups"));
        }
        return OperationResult::success();
    }

    auto set_group_ids(gid_t group) -> OperationResult override
    {
        if (::setresgid(group, group, group) != 0) {
            return OperationResult::failure(failed("Unable to permanently set daemon group IDs"));
        }
        return OperationResult::success();
    }

    auto set_user_ids(uid_t user) -> OperationResult override
    {
        if (::setresuid(user, user, user) != 0) {
            return OperationResult::failure(failed("Unable to permanently set daemon user IDs"));
        }
        return OperationResult::success();
    }

    auto verify_unprivileged_capabilities() -> OperationResult override
    {
        // Saved IDs alone are insufficient when inherited securebits retain capabilities across setresuid.
        CapabilityHeader              header;
        std::array<CapabilityData, 2> capabilities{};
        if (::syscall(SYS_capget, &header, capabilities.data()) != 0) {
            return OperationResult::failure(failed("Unable to inspect daemon capabilities"));
        }
        for (auto const& value : capabilities) {
            if (value.effective != 0U || value.permitted != 0U) {
                return OperationResult::failure(failed("The final daemon identity retained capabilities"));
            }
        }

        // Query the kernel's supported range rather than assuming the build headers name every capability.
        for (unsigned capability = 0; capability < 64U; ++capability) {
            auto const ambient = ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, capability, 0, 0);
            if (ambient < 0) {
                if (errno == EINVAL && capability != 0U) {
                    return OperationResult::success();
                }
                return OperationResult::failure(failed("Unable to inspect daemon ambient capabilities"));
            }
            if (ambient != 0) {
                return OperationResult::failure(failed("The final daemon identity retained ambient capabilities"));
            }
        }
        return OperationResult::success();
    }
};

} // namespace

auto make_system_privilege_operations() -> std::unique_ptr<PrivilegeOperations>
{
    return std::make_unique<SystemPrivilegeOperations>();
}

auto finalize_process_identity(StartupOptions const& options) -> jb::core::Result<FinalIdentity, jb::core::Error>
{
    using IdentityResult = jb::core::Result<FinalIdentity, jb::core::Error>;
    SystemPrivilegeOperations operations;
    auto                      final = resolve_final_identity(options, operations);
    if (!final) {
        return final;
    }
    auto applied = apply_final_identity(*final, operations);
    if (!applied) {
        return IdentityResult::failure(std::move(applied).error());
    }
    return final;
}

} // namespace jb::jobud::detail
