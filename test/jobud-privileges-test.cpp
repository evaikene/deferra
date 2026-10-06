#include "privileges_priv.hpp"
#include "startup_priv.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace jb::jobud::detail;

namespace {

using jb::core::Error;
using jb::core::Result;

auto drop_failure() -> Error
{
    return {.category = jb::core::ErrorCategory::PermissionDenied,
            .code     = "jobud.privilege.drop_failed",
            .message  = "Injected identity operation failure"};
}

class TestPrivileges final : public PrivilegeOperations {
public:
    ProcessIdentity current{
        .real_user            = 1001,
        .effective_user       = 1001,
        .saved_user           = 1001,
        .real_group           = 1002,
        .effective_group      = 1002,
        .saved_group          = 1002,
        .supplementary_groups = {1003, 1002}
    };
    DaemonAccount            requested{.name = "target", .user = 1001, .primary_group = 1002};
    gid_t                    requested_group{1002};
    std::vector<gid_t>       target_groups{1004, 1002, 1004};
    std::vector<std::string> calls;
    std::string              fail_at;
    std::string              mismatch;

    void as_root()
    {
        current = {
            .real_user            = 0,
            .effective_user       = 0,
            .saved_user           = 0,
            .real_group           = 0,
            .effective_group      = 0,
            .saved_group          = 0,
            .supplementary_groups = {0, 99}
        };
    }

    auto identity() -> Result<ProcessIdentity, Error> override
    {
        calls.emplace_back("identity");
        if (fail_at == "identity" || (fail_at == "final_identity" && calls.size() > 1U)) {
            return Result<ProcessIdentity, Error>::failure(drop_failure());
        }
        auto value = current;
        if (std::ranges::find(calls, "set_user") != calls.end()) {
            if (mismatch == "real_user") {
                value.real_user = 0;
            }
            if (mismatch == "effective_user") {
                value.effective_user = 0;
            }
            if (mismatch == "saved_user") {
                value.saved_user = 0;
            }
            if (mismatch == "real_group") {
                value.real_group = 0;
            }
            if (mismatch == "effective_group") {
                value.effective_group = 0;
            }
            if (mismatch == "saved_group") {
                value.saved_group = 0;
            }
            if (mismatch == "groups") {
                value.supplementary_groups.push_back(99);
            }
        }
        return Result<ProcessIdentity, Error>::success(std::move(value));
    }

    auto account([[maybe_unused]] std::string const& name) -> Result<DaemonAccount, Error> override
    {
        calls.emplace_back("account");
        if (fail_at == "account") {
            return Result<DaemonAccount, Error>::failure(drop_failure());
        }
        return Result<DaemonAccount, Error>::success(requested);
    }

    auto group([[maybe_unused]] std::string const& name) -> Result<gid_t, Error> override
    {
        calls.emplace_back("group");
        if (fail_at == "group") {
            return Result<gid_t, Error>::failure(drop_failure());
        }
        return Result<gid_t, Error>::success(requested_group);
    }

    auto account_groups([[maybe_unused]] DaemonAccount const& account, gid_t primary)
        -> Result<std::vector<gid_t>, Error> override
    {
        calls.emplace_back("account_groups");
        if (fail_at == "account_groups") {
            return Result<std::vector<gid_t>, Error>::failure(drop_failure());
        }
        auto groups = target_groups;
        groups.push_back(primary);
        return Result<std::vector<gid_t>, Error>::success(std::move(groups));
    }

    auto initialize_groups(std::string const& name, gid_t primary) -> Result<void, Error> override
    {
        calls.emplace_back("initialize_groups");
        CHECK(name == requested.name);
        if (fail_at == "initialize_groups") {
            return Result<void, Error>::failure(drop_failure());
        }
        current.supplementary_groups = target_groups;
        current.supplementary_groups.push_back(primary);
        return Result<void, Error>::success();
    }

    auto set_group_ids(gid_t group) -> Result<void, Error> override
    {
        calls.emplace_back("set_group");
        if (fail_at == "set_group") {
            return Result<void, Error>::failure(drop_failure());
        }
        current.real_group      = group;
        current.effective_group = group;
        current.saved_group     = group;
        return Result<void, Error>::success();
    }

    auto set_user_ids(uid_t user) -> Result<void, Error> override
    {
        calls.emplace_back("set_user");
        if (fail_at == "set_user") {
            return Result<void, Error>::failure(drop_failure());
        }
        current.real_user      = user;
        current.effective_user = user;
        current.saved_user     = user;
        return Result<void, Error>::success();
    }

    auto verify_unprivileged_capabilities() -> Result<void, Error> override
    {
        calls.emplace_back("capabilities");
        if (fail_at == "capabilities") {
            return Result<void, Error>::failure(drop_failure());
        }
        return Result<void, Error>::success();
    }
};

} // namespace

TEST_CASE("non-root daemon identity is preserved and matching overrides do not transition", "[jobud][privileges]")
{
    TestPrivileges operations;
    StartupOptions options;
    if (GENERATE(false, true)) {
        options.run_as_user  = "target";
        options.run_as_group = "primary";
    }
    auto final = resolve_final_identity(options, operations);
    REQUIRE(final);
    CHECK(final->user == 1001);
    CHECK(final->group == 1002);
    CHECK(final->supplementary_groups == std::vector<gid_t>{1002, 1003});
    CHECK(final->transition == IdentityTransition::Preserve);

    operations.calls.clear();
    REQUIRE(apply_final_identity(*final, operations));
    CHECK(operations.calls == std::vector<std::string>{"identity", "capabilities"});
}

TEST_CASE("non-root daemon cannot select another user or primary group", "[jobud][privileges]")
{
    TestPrivileges operations;
    StartupOptions options;
    options.run_as_user       = "target";
    auto const different_user = GENERATE(false, true);
    if (different_user) {
        operations.requested.user = 2001;
    }
    else {
        operations.requested.primary_group = 1003; // A currently held supplementary group is still different.
    }
    options.allow_root_daemon = true;
    options.allow_root_cli    = true;

    auto final = resolve_final_identity(options, operations);
    REQUIRE_FALSE(final);
    CHECK(final.error().code == "jobud.privilege.identity_mismatch");
    CHECK(operations.calls == std::vector<std::string>{"identity", "account"});
}

TEST_CASE("mixed startup IDs including saved privilege are rejected", "[jobud][privileges]")
{
    TestPrivileges operations;
    auto const     field = GENERATE(0, 1, 2, 3);
    if (field == 0) {
        operations.current.real_user = 0;
    }
    if (field == 1) {
        operations.current.saved_user = 0;
    }
    if (field == 2) {
        operations.current.real_group = 0;
    }
    if (field == 3) {
        operations.current.saved_group = 0;
    }
    auto final = resolve_final_identity({}, operations);
    REQUIRE_FALSE(final);
    CHECK(final.error().code == "jobud.privilege.identity_mismatch");
    CHECK(operations.calls == std::vector<std::string>{"identity"});
}

TEST_CASE("a primary group alone cannot request an identity transition", "[jobud][privileges]")
{
    TestPrivileges operations;
    StartupOptions options;
    options.run_as_group = "primary";
    auto final           = resolve_final_identity(options, operations);
    REQUIRE_FALSE(final);
    CHECK(final.error().code == "jobud.privilege.identity_mismatch");
    CHECK(operations.calls == std::vector<std::string>{"identity"});
}

TEST_CASE("root daemon and CLI permissions are independent", "[jobud][privileges]")
{
    TestPrivileges operations;
    operations.as_root();
    StartupOptions options;
    options.allow_root_cli    = GENERATE(false, true);
    options.allow_root_daemon = GENERATE(false, true);
    auto final                = resolve_final_identity(options, operations);
    if (!options.allow_root_daemon) {
        REQUIRE_FALSE(final);
        CHECK(final.error().code == "jobud.privilege.root_disallowed");
        CHECK(final.error().category == jb::core::ErrorCategory::PermissionDenied);
    }
    else {
        REQUIRE(final);
        CHECK(final->user == 0);
        CHECK(final->transition == IdentityTransition::Preserve);
        operations.calls.clear();
        REQUIRE(apply_final_identity(*final, operations));
        CHECK(operations.calls == std::vector<std::string>{"identity"});
    }
}

TEST_CASE("a requested target wins over unsafe root permission and owns its lookup results", "[jobud][privileges]")
{
    TestPrivileges operations;
    operations.as_root();
    StartupOptions options;
    options.run_as_user        = "target";
    options.run_as_group       = "override";
    options.allow_root_daemon  = GENERATE(false, true);
    operations.requested_group = 1005;
    auto final                 = resolve_final_identity(options, operations);
    REQUIRE(final);
    CHECK(final->user == 1001);
    CHECK(final->group == 1005);
    CHECK(final->supplementary_groups == std::vector<gid_t>{1002, 1004, 1005});

    // Replacing the lookup storage after resolution cannot change the owning plan.
    operations.requested.user  = 2001;
    operations.requested_group = 2005;
    operations.calls.clear();
    REQUIRE(apply_final_identity(*final, operations));
    CHECK(operations.calls ==
          std::vector<std::string>{"initialize_groups", "set_group", "set_user", "identity", "capabilities"});
}

TEST_CASE("a root target requires explicit daemon permission", "[jobud][privileges]")
{
    TestPrivileges operations;
    operations.as_root();
    operations.requested.user = 0;
    StartupOptions options;
    options.run_as_user       = "root-alias";
    options.allow_root_daemon = GENERATE(false, true);
    auto final                = resolve_final_identity(options, operations);
    CHECK(static_cast<bool>(final) == options.allow_root_daemon);
    if (!final) {
        CHECK(final.error().code == "jobud.privilege.root_disallowed");
    }
}

TEST_CASE("identity resolution failure never starts a credential transition", "[jobud][privileges]")
{
    TestPrivileges operations;
    operations.as_root();
    operations.fail_at = GENERATE("identity", "account", "group", "account_groups");
    StartupOptions options;
    options.run_as_user  = "target";
    options.run_as_group = "override";
    auto final           = resolve_final_identity(options, operations);
    REQUIRE_FALSE(final);
    CHECK(final.error().code == "jobud.privilege.drop_failed");
    CHECK(std::ranges::find(operations.calls, "initialize_groups") == operations.calls.end());
}

TEST_CASE("each transition failure exits without later identity operations or admission", "[jobud][privileges]")
{
    TestPrivileges operations;
    operations.as_root();
    StartupOptions options;
    options.run_as_user = "target";
    auto final          = resolve_final_identity(options, operations);
    REQUIRE(final);
    operations.calls.clear();
    operations.fail_at = GENERATE("initialize_groups", "set_group", "set_user", "final_identity", "capabilities");
    auto applied       = apply_final_identity(*final, operations);
    REQUIRE_FALSE(applied);
    CHECK(applied.error().code == "jobud.privilege.drop_failed");

    std::vector<std::string> expected{"initialize_groups", "set_group", "set_user", "identity", "capabilities"};
    auto const               last = operations.fail_at == "final_identity" ? "identity" : operations.fail_at;
    expected.erase(std::ranges::find(expected, last) + 1, expected.end());
    CHECK(operations.calls == expected);
}

TEST_CASE("every final ID and supplementary group must match before capabilities are checked", "[jobud][privileges]")
{
    TestPrivileges operations;
    operations.as_root();
    StartupOptions options;
    options.run_as_user = "target";
    auto final          = resolve_final_identity(options, operations);
    REQUIRE(final);
    operations.calls.clear();
    operations.mismatch =
        GENERATE("real_user", "effective_user", "saved_user", "real_group", "effective_group", "saved_group", "groups");
    REQUIRE_FALSE(apply_final_identity(*final, operations));
    CHECK(operations.calls.back() == "identity");
}
