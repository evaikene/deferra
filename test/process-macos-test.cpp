#include "process.hpp"

#include "byte_buffer.hpp"
#include "error.hpp"
#include "event_loop.hpp"
#include "event_loop_types.hpp"
#include "object.hpp"
#include "process_posix_priv.hpp"
#include "result.hpp"
#include "signal.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_event_loop_backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace jb::core;
using namespace jb::core::priv;

namespace {

struct NativeRun {
    EventLoop                  loop;
    ScopedCurrentEventLoop     current{&loop};
    Object                     receiver;
    Process                    process;
    int                        starts{0};
    int                        finishes{0};
    ByteBuffer                 output;
    std::optional<ProcessExit> result;

    NativeRun()
    {
        auto started           = process.started.connect(&receiver, [this] { ++starts; });
        auto stdout_connection = process.standard_output.connect(&receiver, [this](ByteBuffer const& chunk) {
            CHECK(starts == 1);
            CHECK(finishes == 0);
            output.insert(output.end(), chunk.begin(), chunk.end());
        });
        auto finished          = process.finished.connect(&receiver, [this](ProcessExit const& exit) {
            ++finishes;
            result = exit;
            CHECK(process.state() == ProcessState::NotRunning);
            CHECK_FALSE(process.process_id());
        });
    }

    /// Native readiness determines completion; the deadline only bounds a broken test.
    /// @throws Catch::TestFailureException when launch or readiness validation fails.
    auto execute(ProcessStartInfo info) -> ProcessExit
    {
        REQUIRE(loop.is_valid());
        auto accepted = process.start(std::move(info));
        if (!accepted) {
            UNSCOPED_INFO(accepted.error().code << ": " << accepted.error().detail);
        }
        REQUIRE(accepted);
        CHECK(starts == 0);
        CHECK(finishes == 0);
        REQUIRE(process.process_id());
        auto const pid = static_cast<pid_t>(*process.process_id());

        auto const deadline = Clock::now() + std::chrono::seconds{5};
        while (!result && Clock::now() < deadline) {
            REQUIRE(loop.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }

        REQUIRE(result);
        CHECK(finishes == 1);
        CHECK(::waitpid(pid, nullptr, WNOHANG) == -1);
        CHECK(errno == ECHILD);
        return *result;
    }
};

class SetupFailure final : public ProcessOperations {
public:
    bool fail_pipe{false};
    bool fail_fork{false};
    bool report_root{false};
    int  pipe_calls{0};
    int  fork_calls{0};

    auto make_pipe(int* descriptors) noexcept -> int override
    {
        ++pipe_calls;
        if (fail_pipe) {
            errno = EMFILE;
            return -1;
        }
        return ProcessOperations::make_pipe(descriptors);
    }

    auto create_child() noexcept -> pid_t override
    {
        ++fork_calls;
        if (fail_fork) {
            errno = EAGAIN;
            return -1;
        }
        return ProcessOperations::create_child();
    }

    auto child_options() noexcept -> ProcessChildOptions override
    {
        auto options = ProcessOperations::child_options();
        if (report_root) {
            options.effective_uid = []() -> uid_t { return 0; };
        }
        return options;
    }
};

/// Restores the host descriptor limit and owns the intentionally non-CLOEXEC descriptor even on assertion failure.
struct InheritedDescriptor {
    int           fd{-1};
    struct rlimit original{};

    InheritedDescriptor() { REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0); }

    ~InheritedDescriptor()
    {
        CHECK(::setrlimit(RLIMIT_NOFILE, &original) == 0);
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

} // namespace

TEST_CASE("macOS Process launches a literal command and drains its output before completion", "[core][process][macos]")
{
    NativeRun  run;
    auto const exit = run.execute({
        .executable = "/usr/bin/printf",
        .arguments  = {"%s", "literal $value; *"}
    });
    CHECK(exit.kind == ProcessExitKind::Exited);
    CHECK(exit.exit_code == 0);
    CHECK(run.starts == 1);
    CHECK(as_string_view(run.output) == "literal $value; *");
    CHECK_FALSE(exit.stdout_lost);
    CHECK_FALSE(exit.stderr_lost);
}

TEST_CASE("macOS Process reports asynchronous setup failures without request data", "[core][process][macos]")
{
    NativeRun        run;
    ProcessStartInfo info{.executable = "/usr/bin/true"};
    std::string      expected;
    SECTION("exec failure")
    {
        info.executable = "/dev/null/private-command";
        expected        = "core.process.exec_failed";
    }
    SECTION("directory failure")
    {
        info.working_directory = "/dev/null/private-directory";
        expected               = "core.process.chdir_failed";
    }
    SECTION("authoritative child identity rejection")
    {
        auto operations         = std::make_shared<SetupFailure>();
        operations->report_root = true;
        ProcessTestAccess::set_operations(run.process, operations);
        info.require_non_root = true;
        expected              = "core.process.security_failed";
    }
    auto const exit = run.execute(std::move(info));
    CHECK(exit.kind == ProcessExitKind::StartFailed);
    REQUIRE(exit.start_error);
    CHECK(exit.start_error->code == expected);
    CHECK(exit.start_error->message.find("private-") == std::string::npos);
    CHECK(exit.start_error->detail.find("private-") == std::string::npos);
    CHECK(run.starts == 0);
    CHECK(run.output.empty());
}

TEST_CASE("macOS Process rejects unsupported hardening and parent setup failures before acceptance",
          "[core][process][macos]")
{
    NativeRun run;
    auto      operations = std::make_shared<SetupFailure>();
    ProcessTestAccess::set_operations(run.process, operations);
    ProcessStartInfo info{.executable = "/usr/bin/true"};
    std::string      expected;
    SECTION("strict privilege hardening is unsupported before resource setup")
    {
        info.prevent_privilege_gain = true;
        expected                    = "core.process.security_unsupported";
    }
    SECTION("pipe failure")
    {
        operations->fail_pipe = true;
        expected              = "core.process.resource_setup_failed";
    }
    SECTION("fork failure")
    {
        operations->fail_fork = true;
        expected              = "core.process.fork_failed";
    }
    auto const accepted = run.process.start(info);
    REQUIRE_FALSE(accepted);
    CHECK(accepted.error().code == expected);
    if (info.prevent_privilege_gain) {
        CHECK(accepted.error().category == ErrorCategory::Unsupported);
        CHECK(operations->pipe_calls == 0);
        CHECK(operations->fork_calls == 0);
    }
    CHECK(run.process.state() == ProcessState::NotRunning);
    CHECK_FALSE(run.process.process_id());
    CHECK(run.loop.process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(run.starts == 0);
    CHECK(run.finishes == 0);
    CHECK(EventLoopTestAccess::active_process_count(run.loop) == 0);
    CHECK(EventLoopTestAccess::active_timer_count(run.loop) == 0);
}

TEST_CASE("macOS Process closes inherited descriptors above a lowered allocation limit", "[core][process][macos]")
{
    NativeRun           run;
    InheritedDescriptor inherited;
    auto                source = ::open("/dev/null", O_RDONLY);
    REQUIRE(source >= 0);
    inherited.fd = ::fcntl(source, F_DUPFD, 100);
    ::close(source);
    REQUIRE(inherited.fd >= 100);
    auto lowered     = inherited.original;
    lowered.rlim_cur = 64;
    REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);

    ProcessOperations operations;
    unsigned int      limit{0};
    REQUIRE(operations.descriptor_close_limit(limit) == 0);
    CHECK(limit > static_cast<unsigned int>(inherited.fd));
    auto const exit = run.execute({
        .executable = "/bin/test",
        .arguments  = {"!", "-e", "/dev/fd/" + std::to_string(inherited.fd)}
    });
    CHECK(exit.kind == ProcessExitKind::Exited);
    CHECK(exit.exit_code == 0);
    // Cleanup belongs only to the child; the parent's descriptor remains valid.
    CHECK(::fcntl(inherited.fd, F_GETFD) >= 0);
}
