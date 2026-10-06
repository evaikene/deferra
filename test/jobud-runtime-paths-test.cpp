#include "configuration_file_priv.hpp"
#include "privileges_priv.hpp"
#include "runtime_paths_priv.hpp"
#include "startup_priv.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace jb::jobud::detail;
using jb::test::TemporaryDirectory;

auto current_identity(PrivilegeOperations& operations) -> FinalIdentity
{
    StartupOptions options;
    options.allow_root_daemon = ::geteuid() == 0;
    auto identity             = resolve_final_identity(options, operations);
    REQUIRE(identity);
    return *identity;
}

auto options_for(std::filesystem::path const& directory) -> StartupOptions
{
    return {.socket_path = directory / "daemon.sock", .database_path = directory / "state.sqlite"};
}

auto metadata(std::filesystem::path const& path) -> struct stat {
    struct stat value{};
    REQUIRE(::lstat(path.c_str(), &value) == 0);
    return value;

}

auto descriptor_count() -> std::size_t

{
    std::size_t count{0};
    for ([[maybe_unused]] auto const& entry : std::filesystem::directory_iterator{"/proc/self/fd"}) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("configuration trust checks the invoking owner and protected parents", "[jobud][paths][config]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const         config = directory.path() / "daemon.ini";
    std::ofstream{config} << "cli.concurrency = 3\n";
    REQUIRE(read_configuration_file(config, false));

    auto const mode = GENERATE(0620, 0602, 0666);
    REQUIRE(::chmod(config.c_str(), static_cast<mode_t>(mode)) == 0);
    auto refused = read_configuration_file(config, false);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "jobud.config.read_failed");
    CHECK(refused.error().category == jb::core::ErrorCategory::PermissionDenied);
}

TEST_CASE("unsafe parents cannot be hidden by optional absence or alias traversal", "[jobud][paths][config]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const         parent = directory.path() / "writable";
    REQUIRE(std::filesystem::create_directory(parent));
    REQUIRE(::chmod(parent.c_str(), 0777) == 0);
    auto missing = read_configuration_file(parent / "missing.ini", true);
    REQUIRE_FALSE(missing);
    std::filesystem::create_directory_symlink(parent, directory.path() / "alias");
    CHECK_FALSE(read_configuration_file(directory.path() / "alias/missing.ini", true));
    CHECK_FALSE(read_configuration_file(parent / "../missing.ini", true));
}

TEST_CASE("protected parent traversal preserves aliases before dot-dot", "[jobud][paths]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const         target = directory.path() / "target";
    std::filesystem::create_directories(target / "child");
    REQUIRE(::chmod(target.c_str(), 0700) == 0);
    std::filesystem::create_directory_symlink(target / "child", directory.path() / "alias");
    auto parent = open_trusted_parent(directory.path() / "alias/../state.sqlite", ::geteuid(), false);
    REQUIRE(parent);
    REQUIRE(*parent);
    CHECK(parent->value().path == target);
    CHECK((::fcntl(parent->value().directory.get(), F_GETFD) & FD_CLOEXEC) != 0);

    // The state directory is the physical target, not the lexical sibling of the alias.
    auto operations = make_system_privilege_operations();
    auto identity   = current_identity(*operations);
    auto prepared   = prepare_runtime_paths(options_for(directory.path() / "alias/.."), identity, *operations);
    REQUIRE(prepared);
    CHECK(prepared->database_path() == target / "state.sqlite");
    REQUIRE(verify_runtime_paths(*prepared, identity.user));
}

TEST_CASE("runtime preparation creates only explicit leaves and never repairs existing trees", "[jobud][paths]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    auto               options    = options_for(directory.path() / "leaf");
    auto               prepared   = prepare_runtime_paths(options, identity, *operations);
    REQUIRE(prepared);
    CHECK(metadata(directory.path() / "leaf").st_uid == identity.user);
    CHECK((metadata(directory.path() / "leaf").st_mode & 07777) == 0700);
    CHECK_FALSE(std::filesystem::exists(options.database_path));
    REQUIRE(verify_runtime_paths(*prepared, identity.user));

    auto missing = prepare_runtime_paths(options_for(directory.path() / "missing/leaf"), identity, *operations);
    REQUIRE_FALSE(missing);
    CHECK_FALSE(std::filesystem::exists(directory.path() / "missing"));

    REQUIRE(::chmod((directory.path() / "leaf").c_str(), 0755) == 0);
    auto insecure = prepare_runtime_paths(options, identity, *operations);
    REQUIRE_FALSE(insecure);
    CHECK((metadata(directory.path() / "leaf").st_mode & 07777) == 0755);
}

TEST_CASE("operational leaves reject symlinks and shared temporary paths", "[jobud][paths]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    std::filesystem::create_directory_symlink(directory.path(), directory.path() / "alias");
    CHECK_FALSE(prepare_runtime_paths(options_for(directory.path() / "alias"), identity, *operations));
    CHECK_FALSE(prepare_runtime_paths(options_for("/tmp"), identity, *operations));
    REQUIRE(prepare_runtime_paths(options_for(directory.path()), identity, *operations));
}

TEST_CASE("group runtime leaves require the authorized group and a separate private state leaf", "[jobud][paths]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                 std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                 std::filesystem::perms::others_exec};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    auto               options    = options_for(directory.path() / "state");
    options.socket_path           = directory.path() / "runtime/daemon.sock";
    options.socket_mode           = 0660;
    auto prepared                 = prepare_runtime_paths(options, identity, *operations);
    REQUIRE(prepared);
    auto const runtime = metadata(directory.path() / "runtime");
    CHECK((runtime.st_mode & 07777) == 0750);
    CHECK(runtime.st_gid == identity.group);
    REQUIRE(verify_runtime_paths(*prepared, identity.user));
    options.socket_path = options.database_path.parent_path() / "daemon.sock";
    CHECK_FALSE(prepare_runtime_paths(options, identity, *operations));
    options.socket_path = directory.path() / "runtime/daemon.sock";
    REQUIRE(::chmod(directory.path().c_str(), 0700) == 0);
    auto inaccessible = prepare_runtime_paths(options, identity, *operations);
    REQUIRE_FALSE(inaccessible);
    CHECK(inaccessible.error().detail == "runtime_parent_not_traversable");
}

TEST_CASE("SQLite artifact inspection refuses insecure modes without repairing them", "[jobud][paths][sqlite]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    auto               prepared   = prepare_runtime_paths(options_for(directory.path()), identity, *operations);
    REQUIRE(prepared);
    REQUIRE(validate_database_artifacts(*prepared, identity.user));

    auto const* suffix = GENERATE("", ".lock", "-wal", "-shm", "-journal");
    auto const  path   = std::filesystem::path{prepared->database_path().string() + suffix};
    std::ofstream{path} << "fixture";
    REQUIRE(::chmod(path.c_str(), 0600) == 0);
    REQUIRE(validate_database_artifacts(*prepared, identity.user));
    REQUIRE(::chmod(path.c_str(), 0644) == 0);
    CHECK_FALSE(validate_database_artifacts(*prepared, identity.user));
    CHECK((metadata(path).st_mode & 07777) == 0644);
}

TEST_CASE("SQLite artifact inspection refuses symlinks and nonregular files", "[jobud][paths][sqlite]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    auto               prepared   = prepare_runtime_paths(options_for(directory.path()), identity, *operations);
    REQUIRE(prepared);
    auto const path = std::filesystem::path{prepared->database_path().string() + GENERATE("", ".lock", "-wal", "-shm")};
    if (GENERATE(false, true)) {
        std::filesystem::create_symlink(directory.path() / "absent", path);
    }
    else {
        REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
    }
    CHECK_FALSE(validate_database_artifacts(*prepared, identity.user));
    CHECK(std::filesystem::symlink_status(path).type() != std::filesystem::file_type::not_found);
}

TEST_CASE("retained directory descriptors detect pathname replacement", "[jobud][paths]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    auto               prepared = prepare_runtime_paths(options_for(directory.path() / "leaf"), identity, *operations);
    REQUIRE(prepared);
    std::filesystem::rename(directory.path() / "leaf", directory.path() / "original");
    REQUIRE(std::filesystem::create_directory(directory.path() / "leaf"));
    REQUIRE(::chmod((directory.path() / "leaf").c_str(), 0700) == 0);
    auto verified = verify_runtime_paths(*prepared, identity.user);
    REQUIRE_FALSE(verified);
    CHECK(verified.error().detail == "parent_replaced");
}

TEST_CASE("filesystem failures release every retained descriptor", "[jobud][paths][lifetime]")
{
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto               operations = make_system_privilege_operations();
    auto               identity   = current_identity(*operations);
    auto const         config     = directory.path() / "oversized.ini";
    std::ofstream{config} << std::string(65'537, 'x');
    auto const fifo = directory.path() / "fifo.ini";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    std::filesystem::create_symlink("cycle", directory.path() / "cycle");
    auto       partial = options_for(directory.path() / "state");
    auto const runtime = directory.path() / "insecure-runtime";
    REQUIRE(std::filesystem::create_directory(runtime));
    REQUIRE(::chmod(runtime.c_str(), 0755) == 0);
    partial.socket_path = runtime / "daemon.sock";
    auto const before   = descriptor_count();
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        CHECK_FALSE(prepare_runtime_paths(options_for(directory.path() / "missing/leaf"), identity, *operations));
        CHECK_FALSE(read_configuration_file(directory.path() / "missing.ini", false));
        CHECK_FALSE(read_configuration_file(config, false));
        CHECK_FALSE(read_configuration_file(fifo, false));
        CHECK_FALSE(open_trusted_parent(directory.path() / "cycle/file", identity.user, false));
        CHECK_FALSE(prepare_runtime_paths(partial, identity, *operations));
        {
            auto prepared = prepare_runtime_paths(options_for(directory.path()), identity, *operations);
            REQUIRE(prepared);
        }
    }
    CHECK(descriptor_count() == before);
}

TEST_CASE("root configuration and state ownership are distinct", "[jobud][paths][root]")
{
    auto const* target = std::getenv("JOBU_TEST_PRIVILEGE_USER");
    if (::geteuid() != 0 || target == nullptr) {
        SKIP("requires root and an explicitly provisioned disposable target account");
    }
    auto operations = make_system_privilege_operations();
    auto account    = operations->account(target);
    REQUIRE(account);
    REQUIRE(account->user != 0);
    TemporaryDirectory directory{std::filesystem::perms::owner_all};
    auto const         config = directory.path() / "daemon.ini";
    std::ofstream{config} << "cli.concurrency = 3\n";
    REQUIRE(::chown(config.c_str(), account->user, account->primary_group) == 0);
    CHECK_FALSE(read_configuration_file(config, false));
    REQUIRE(::chown(config.c_str(), 0, 0) == 0);
    REQUIRE(::chown(directory.path().c_str(), account->user, account->primary_group) == 0);
    CHECK_FALSE(read_configuration_file(config, false));
    REQUIRE(::chown(directory.path().c_str(), 0, 0) == 0);

    auto options        = options_for(directory.path());
    options.run_as_user = target;
    auto final          = resolve_final_identity(options, *operations);
    REQUIRE(final);
    CHECK_FALSE(prepare_runtime_paths(options, *final, *operations));
    CHECK(metadata(directory.path()).st_uid == 0);

    // Existing foreign-owned artifacts are refused even when root could open or repair them.
    REQUIRE(::chown(directory.path().c_str(), account->user, account->primary_group) == 0);
    auto prepared = prepare_runtime_paths(options, *final, *operations);
    REQUIRE(prepared);
    std::ofstream{options.database_path} << "foreign state";
    REQUIRE(::chmod(options.database_path.c_str(), 0600) == 0);
    CHECK_FALSE(validate_database_artifacts(*prepared, final->user));
    CHECK(metadata(options.database_path).st_uid == 0);
}
