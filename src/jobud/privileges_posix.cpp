#include "privileges_priv.hpp"

#include "startup_priv.hpp"

#include <algorithm>
#include <utility>

namespace jb::jobud::detail {
namespace {

auto denied(char const* code, char const* message) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::PermissionDenied, .code = code, .message = message};
}

auto normalized_groups(std::vector<gid_t> groups) -> std::vector<gid_t>
{
    std::ranges::sort(groups);
    auto const duplicates = std::ranges::unique(groups);
    groups.erase(duplicates.begin(), duplicates.end());
    return groups;
}

} // namespace

auto resolve_final_identity(StartupOptions const& options, PrivilegeOperations& operations)
    -> jb::core::Result<FinalIdentity, jb::core::Error>
{
    using IdentityResult = jb::core::Result<FinalIdentity, jb::core::Error>;

    auto current = operations.identity();
    if (!current) {
        return IdentityResult::failure(std::move(current).error());
    }
    // Saved IDs must also agree: an apparently unprivileged invocation must not retain a root identity.
    if (current->real_user != current->effective_user || current->saved_user != current->effective_user ||
        current->real_group != current->effective_group || current->saved_group != current->effective_group) {
        return IdentityResult::failure(
            denied("jobud.privilege.identity_mismatch", "Mixed startup identities are refused"));
    }

    FinalIdentity final{.user                 = current->effective_user,
                        .group                = current->effective_group,
                        .supplementary_groups = normalized_groups(std::move(current->supplementary_groups))};
    if (options.run_as_group && !options.run_as_user) {
        return IdentityResult::failure(
            denied("jobud.privilege.identity_mismatch", "A daemon group requires a target account"));
    }
    if (options.run_as_user) {
        auto account = operations.account(*options.run_as_user);
        if (!account) {
            return IdentityResult::failure(std::move(account).error());
        }
        final.user         = account->user;
        final.group        = account->primary_group;
        final.account_name = account->name;
        if (options.run_as_group) {
            auto group = operations.group(*options.run_as_group);
            if (!group) {
                return IdentityResult::failure(std::move(group).error());
            }
            final.group = *group;
        }

        if (current->effective_user != 0) {
            // Even a supplementary group cannot be selected as a different primary group by non-root startup.
            if (final.user != current->effective_user || final.group != current->effective_group) {
                return IdentityResult::failure(
                    denied("jobud.privilege.identity_mismatch", "Daemon identity changes require root"));
            }
        }
        else {
            auto groups = operations.account_groups(*account, final.group);
            if (!groups) {
                return IdentityResult::failure(std::move(groups).error());
            }
            final.supplementary_groups = normalized_groups(std::move(groups).value());
            final.transition           = IdentityTransition::InitializeTarget;
        }
    }

    // A requested target wins over the unsafe flag. CLI permission never authorizes a root daemon.
    if (final.user == 0 && !options.allow_root_daemon) {
        return IdentityResult::failure(
            denied("jobud.privilege.root_disallowed",
                   "Root daemon startup requires an explicit unsafe override or a non-root target"));
    }
    return IdentityResult::success(std::move(final));
}

auto apply_final_identity(FinalIdentity const& final, PrivilegeOperations& operations)
    -> jb::core::Result<void, jb::core::Error>
{
    using ApplyResult = jb::core::Result<void, jb::core::Error>;

    // There is no rollback after this point. Each failure returns immediately and main exits before admission.
    if (final.transition == IdentityTransition::InitializeTarget) {
        auto groups = operations.initialize_groups(final.account_name, final.group);
        if (!groups) {
            return groups;
        }
        auto group = operations.set_group_ids(final.group);
        if (!group) {
            return group;
        }
        auto user = operations.set_user_ids(final.user);
        if (!user) {
            return user;
        }
    }

    auto actual = operations.identity();
    if (!actual) {
        return ApplyResult::failure(std::move(actual).error());
    }
    if (actual->real_user != final.user || actual->effective_user != final.user || actual->saved_user != final.user ||
        actual->real_group != final.group || actual->effective_group != final.group ||
        actual->saved_group != final.group ||
        normalized_groups(std::move(actual->supplementary_groups)) != final.supplementary_groups) {
        return ApplyResult::failure(
            denied("jobud.privilege.drop_failed", "Final daemon credentials did not match the resolved identity"));
    }

    // Unsafe root retains its privileges intentionally. Every non-root result must pass the capability gate.
    if (final.user != 0) {
        return operations.verify_unprivileged_capabilities();
    }
    return ApplyResult::success();
}

} // namespace jb::jobud::detail
