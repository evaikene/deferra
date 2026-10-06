#include "privileges_priv.hpp"
#include "startup_priv.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <cstdio> // IWYU pragma: keep stderr and stdout.
#include <cstdlib>
#include <string_view>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

auto cannot_regain_root() -> bool
{
    // These intentionally destructive probes run only in this disposable process, never in production.
    errno = 0;
    if (::setuid(0) == 0 || errno != EPERM) {
        return false;
    }
    errno = 0;
    if (::seteuid(0) == 0 || errno != EPERM) {
        return false;
    }
    errno = 0;
    if (::setresuid(0, 0, 0) == 0 || errno != EPERM) {
        return false;
    }
    errno = 0;
    if (::setgid(0) == 0 || errno != EPERM) {
        return false;
    }
    errno = 0;
    if (::setegid(0) == 0 || errno != EPERM) {
        return false;
    }
    errno = 0;
    return ::setresgid(0, 0, 0) != 0 && errno == EPERM;
}

} // namespace

auto main(int argc, char* argv[]) -> int
{
    using namespace jb::jobud::detail;
    if (argc < 2) {
        return 2;
    }
    auto const     mode = std::string_view{argv[1]};
    StartupOptions options;
    if (mode == "target" || mode == "retain-caps") {
        if (argc != 3 && argc != 4) {
            return 2;
        }
        options.run_as_user = argv[2];
        if (argc == 4) {
            options.run_as_group = argv[3];
        }
    }
    else if (mode == "unsafe-root" && argc == 2) {
        options.allow_root_daemon = true;
    }
    else if (mode != "preserve" || argc != 2) {
        return 2;
    }

    // Exercise inherited retention separately: a successful UID change alone must not permit admission.
    if (mode == "retain-caps" && ::prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) != 0) {
        return 3;
    }
    auto final = finalize_process_identity(options);
    if (!final) {
        fmt::print(stderr, "{}\n", final.error().code);
        return mode == "retain-caps" && final.error().code == "jobud.privilege.drop_failed" && ::geteuid() != 0 ? 0 : 1;
    }
    if (mode == "retain-caps") {
        return 4;
    }
    if (final->user != 0 && !cannot_regain_root()) {
        return 5;
    }

    auto operations = make_system_privilege_operations();
    auto actual     = operations->identity();
    if (!actual) {
        return 6;
    }
    fmt::print(stdout,
               "verified uid={} gid={} supplementary_groups={}\n",
               actual->effective_user,
               actual->effective_group,
               actual->supplementary_groups.size());
    return EXIT_SUCCESS;
}
