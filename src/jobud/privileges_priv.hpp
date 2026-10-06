#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace jb::jobud::detail {

struct StartupOptions;

/// Owning credential snapshot; supplementary groups are compared as a set, not in NSS order.
struct ProcessIdentity {
    uid_t              real_user{};
    uid_t              effective_user{};
    uid_t              saved_user{};
    gid_t              real_group{};
    gid_t              effective_group{};
    gid_t              saved_group{};
    std::vector<gid_t> supplementary_groups;
};

/// Copies the NSS account name and numeric identity before further lookups invalidate their buffers.
struct DaemonAccount {
    std::string name;
    uid_t       user{};
    gid_t       primary_group{};
};

enum class IdentityTransition : std::uint8_t {
    Preserve,
    InitializeTarget
};

/// Resolved before credential changes; authorizes preparation of explicit protected directory leaves.
/// Database, listener and worker resources require application and verification of this identity first.
struct FinalIdentity {
    uid_t              user{};
    gid_t              group{};
    std::vector<gid_t> supplementary_groups;
    std::string        account_name;
    IdentityTransition transition{IdentityTransition::Preserve};
};

/// Private, synchronous boundary for account lookup and checked credential operations.
/// Production calls it on the startup thread before any worker-capable object exists.
class PrivilegeOperations {
public:
    virtual ~PrivilegeOperations() = default;

    virtual auto identity() -> jb::core::Result<ProcessIdentity, jb::core::Error>                     = 0;
    virtual auto account(std::string const& name) -> jb::core::Result<DaemonAccount, jb::core::Error> = 0;
    virtual auto group(std::string const& name) -> jb::core::Result<gid_t, jb::core::Error>           = 0;
    virtual auto account_groups(DaemonAccount const& account, gid_t primary)
        -> jb::core::Result<std::vector<gid_t>, jb::core::Error> = 0;
    virtual auto initialize_groups(std::string const& account, gid_t primary)
        -> jb::core::Result<void, jb::core::Error>                                             = 0;
    virtual auto set_group_ids(gid_t group) -> jb::core::Result<void, jb::core::Error>         = 0;
    virtual auto set_user_ids(uid_t user) -> jb::core::Result<void, jb::core::Error>           = 0;
    virtual auto verify_unprivileged_capabilities() -> jb::core::Result<void, jb::core::Error> = 0;
};

/// Resolves run-only authorization without changing credentials. Local check-config remains independent.
[[nodiscard]] auto resolve_final_identity(StartupOptions const& options, PrivilegeOperations& operations)
    -> jb::core::Result<FinalIdentity, jb::core::Error>;

/// Applies groups, permanent GIDs, then permanent UIDs; checks the resulting IDs/groups/capabilities.
/// On failure the caller must exit without constructing resources, retrying, or attempting to regain privilege.
[[nodiscard]] auto apply_final_identity(FinalIdentity const& final, PrivilegeOperations& operations)
    -> jb::core::Result<void, jb::core::Error>;

/// Linux implementation uses checked native IDs/capabilities, with no libcap dependency or keep-caps setting.
[[nodiscard]] auto make_system_privilege_operations() -> std::unique_ptr<PrivilegeOperations>;

/// Resolves, applies and verifies the Linux identity before returning the owning final result.
[[nodiscard]] auto finalize_process_identity(StartupOptions const& options)
    -> jb::core::Result<FinalIdentity, jb::core::Error>;

} // namespace jb::jobud::detail
