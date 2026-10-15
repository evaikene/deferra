#include "path_diagnostic_priv.hpp"

#include "text_validation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace {

using namespace jb::jobud::detail;

auto refused(std::string reason, PathRole role = PathRole::StateDirectory) -> PathFailure
{
    return {
        .error      = {.code = "jobud.path.unsafe", .message = "private-backend-canary", .detail = std::move(reason)},
        .diagnostic = PathDiagnostic{.role            = role,
                       .configured_path = "/prefix/var/lib/jobu.sqlite",
                       .failing_path    = "/prefix/var/lib",
                       .required_uid    = 1001}
    };
}

auto has_field(FormattedPathFailure const& formatted, std::string_view name) -> bool
{
    return std::ranges::any_of(formatted.fields, [name](auto const& field) { return field.name == name; });
}

auto field(FormattedPathFailure const& formatted, std::string_view name) -> PathDiagnosticField const&
{
    auto const found = std::ranges::find_if(formatted.fields, [name](auto const& value) { return value.name == name; });
    REQUIRE(found != formatted.fields.end());
    return *found;
}

} // namespace

TEST_CASE("Missing parents identify the original resource without inventing metadata", "[jobud][paths][diagnostic]")
{
    auto failure                     = refused("missing_parent");
    failure.diagnostic->failing_path = "/prefix/var";
    failure.diagnostic->system_errno = ENOENT;
    auto const formatted             = format_path_failure("paths", failure);
    CHECK(formatted.message.find("Directory '/prefix/var' is missing") != std::string::npos);
    CHECK(formatted.message.find("/prefix/var/lib/jobu.sqlite") != std::string::npos);
    CHECK(formatted.action.find("Provision") != std::string::npos);
    CHECK(std::get<std::string>(field(formatted, "resource").value) == "state_directory");
    CHECK(std::get<std::int64_t>(field(formatted, "errno").value) == ENOENT);
    CHECK_FALSE(has_field(formatted, "actual_mode"));
    CHECK_FALSE(has_field(formatted, "actual_uid"));
    CHECK_FALSE(has_field(formatted, "actual_gid"));
    CHECK_FALSE(has_field(formatted, "required_mode"));
}

TEST_CASE("Directory advice distinguishes modes from incorrect identities", "[jobud][paths][diagnostic]")
{
    auto  failure    = refused("leaf_owner_or_mode", GENERATE(PathRole::StateDirectory, PathRole::RuntimeDirectory));
    auto& diagnostic = *failure.diagnostic;
    diagnostic.required_mode = 0700;
    diagnostic.required_gid  = 1002;
    set_path_metadata(diagnostic, S_IFDIR | 0755U, 1001, 1002);
    auto formatted = format_path_failure("paths", failure);
    CHECK(formatted.message.find("permissions 0755; required permissions are 0700") != std::string::npos);
    CHECK(formatted.action.find("exact, inspected directory") != std::string::npos);
    CHECK(std::get<std::string>(field(formatted, "actual_mode").value) == "0755");
    CHECK(std::get<std::string>(field(formatted, "required_mode").value) == "0700");

    // Simultaneously wrong ownership and mode must not suggest chmod as a complete repair.
    diagnostic.actual_uid = 2001;
    diagnostic.actual_gid = 2002;
    formatted             = format_path_failure("paths", failure);
    CHECK(formatted.message.find("UID 2001 and GID 2002") != std::string::npos);
    CHECK(formatted.message.find("UID 1001 and GID 1002") != std::string::npos);
    CHECK(formatted.action.find("changing permissions alone is insufficient") != std::string::npos);

    diagnostic.actual_uid = diagnostic.required_uid;
    formatted             = format_path_failure("paths", failure);
    CHECK(formatted.message.find("GID 2002") != std::string::npos);
    CHECK(formatted.message.find("GID 1002") != std::string::npos);
}

TEST_CASE("Ancestor and administrative traversal advice preserves protected access", "[jobud][paths][diagnostic]")
{
    auto failure = refused("parent_writable", PathRole::RuntimeDirectory);
    set_path_metadata(*failure.diagnostic, S_IFDIR | 0775U, 1001, 2002);
    auto formatted = format_path_failure("paths", failure);
    CHECK(formatted.message.find("Ancestor '/prefix/var/lib' has permissions 0775") != std::string::npos);
    CHECK(formatted.message.find("control the protected") != std::string::npos);

    failure.error.detail             = "runtime_parent_not_traversable";
    failure.diagnostic->required_gid = 1002;
    formatted                        = format_path_failure("paths", failure);
    CHECK(formatted.message.find("administrative group traversal (GID 1002)") != std::string::npos);
    CHECK(formatted.message.find("actual GID 2002") != std::string::npos);
    CHECK(formatted.action.find("do not grant group write access") != std::string::npos);
}

TEST_CASE("Collision and artifact advice describes the inspected entry", "[jobud][paths][diagnostic]")
{
    auto failure = refused("artifact_owner_type_or_mode", PathRole::DatabaseArtifact);
    set_path_metadata(*failure.diagnostic, S_IFREG | 0644U, 1001, 1002);
    auto formatted = format_path_failure("database_paths", failure);
    CHECK(formatted.message.find("permissions 0644") != std::string::npos);
    CHECK(formatted.message.find("at most owner read/write access (0600)") != std::string::npos);
    CHECK_FALSE(has_field(formatted, "required_mode"));

    set_path_metadata(*failure.diagnostic, GENERATE(S_IFLNK, S_IFIFO, S_IFDIR) | 0700U, 1001, 1002);
    formatted = format_path_failure("database_paths", failure);
    CHECK(formatted.message.find("JobU preserved the entry") != std::string::npos);
    CHECK(formatted.action.find("required regular file type") != std::string::npos);
    CHECK(formatted.action.find("unlink") == std::string::npos);
    CHECK(formatted.action.find("delete") == std::string::npos);
}

TEST_CASE("Captured errno and zero-valued inspected metadata survive formatting", "[jobud][paths][diagnostic]")
{
    auto failure                     = refused("directory_open");
    failure.diagnostic->system_errno = EACCES;
    set_path_metadata(*failure.diagnostic, S_IFDIR, 0, 0);
    errno                = EBUSY;
    auto const formatted = format_path_failure("paths", failure);
    CHECK(std::get<std::int64_t>(field(formatted, "errno").value) == EACCES);
    CHECK(std::get<std::string>(field(formatted, "actual_mode").value) == "0000");
    CHECK(std::get<std::uint64_t>(field(formatted, "actual_uid").value) == 0);
    CHECK(std::get<std::uint64_t>(field(formatted, "actual_gid").value) == 0);
    CHECK(formatted.message.find("errno " + std::to_string(EACCES)) != std::string::npos);
    CHECK(formatted.action.find("search access") != std::string::npos);

    // A descriptor inspection failure keeps its historic reason, but must not claim open failed.
    failure.error.detail          = "root_open";
    failure.diagnostic->operation = PathOperation::Inspect;
    CHECK(format_path_failure("paths", failure).message.find("Unable to inspect") != std::string::npos);
}

TEST_CASE("Pre-logger diagnostics escape hostile paths and reject arbitrary backend text", "[jobud][paths][diagnostic]")
{
    auto failure                     = refused("missing_parent", PathRole::Configuration);
    failure.diagnostic->failing_path = std::string{"/path with 'quotes'/\n\r\x1b\x7f\xff\xc2\x85\xe2\x80\xa8"};
    auto text                        = path_failure_text(failure);
    CHECK(jb::core::is_valid_utf8(text));
    CHECK(text.find("path with 'quotes'") != std::string::npos);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find("\\x0a") != std::string::npos);
    CHECK(text.find("\\xff") != std::string::npos);
    CHECK(text.find("private-backend-canary") == std::string::npos);

    failure.error.detail = "private-backend-canary";
    text                 = path_failure_text(failure);
    CHECK(text.find("private-backend-canary") == std::string::npos);
    failure.diagnostic.reset();
    auto const formatted = format_path_failure("identity", failure);
    CHECK(formatted.fields.size() == 2);
    CHECK(formatted.message == "Daemon startup failed.");
}
