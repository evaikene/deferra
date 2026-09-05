#include "process.hpp"

#include "byte_buffer.hpp"
#include "error.hpp"
#include "event_loop.hpp"
#include "event_loop_types.hpp"
#include "object.hpp"
#include "process_request_priv.hpp"
#include "result.hpp"
#include "signal.hpp"
#include "support/fake_event_loop_backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <memory>
#include <string>

using namespace jb::core;
using namespace jb::core::priv;
using namespace std::chrono_literals;

namespace {

/// Restore process-global state even if a REQUIRE aborts the current test section.
class ScopedSigchld final {
public:
    ScopedSigchld() { REQUIRE(sigaction(SIGCHLD, nullptr, &_previous) == 0); }

    ~ScopedSigchld() { CHECK(sigaction(SIGCHLD, &_previous, nullptr) == 0); }

    ScopedSigchld(ScopedSigchld const&)                    = delete;
    auto operator=(ScopedSigchld const&) -> ScopedSigchld& = delete;

    void install(bool ignored, bool no_wait)
    {
        struct sigaction disposition{};
        disposition.sa_handler = ignored ? SIG_IGN : SIG_DFL;
        disposition.sa_flags   = no_wait ? SA_NOCLDWAIT : 0;
        REQUIRE(sigemptyset(&disposition.sa_mask) == 0);
        REQUIRE(sigaction(SIGCHLD, &disposition, nullptr) == 0);
    }

private:
    struct sigaction _previous{};
};

} // namespace

TEST_CASE("Process defaults and Object parenting remain idle", "[core][process]")
{
    auto                   fake = make_fake_event_loop();
    ScopedCurrentEventLoop current{fake.loop.get()};
    ProcessStartInfo       info;
    CHECK(info.executable.empty());
    CHECK(info.arguments.empty());
    CHECK(info.environment.empty());
    CHECK(info.working_directory == "/");
    CHECK_FALSE(info.timeout);
    CHECK(info.termination_grace == 5s);
    CHECK_FALSE(info.require_non_root);
    CHECK_FALSE(info.prevent_privilege_gain);
    ProcessExit exit;
    CHECK(exit.kind == ProcessExitKind::StartFailed);
    CHECK_FALSE(exit.exit_code);
    CHECK_FALSE(exit.signal_number);
    CHECK_FALSE(exit.start_error);
    CHECK_FALSE(exit.stdout_lost);
    CHECK_FALSE(exit.stderr_lost);

    int   destroyed{0};
    auto  parent = std::make_unique<Object>();
    auto* child  = new Process{parent.get()};
    CHECK(child->parent() == parent.get());
    CHECK(child->event_loop() == fake.loop.get());
    CHECK(child->thread_ctx() == parent->thread_ctx());
    CHECK(child->state() == ProcessState::NotRunning);
    CHECK_FALSE(child->process_id());
    REQUIRE(parent->children().size() == 1);
    // The counter outlives the complete parent/child destruction sequence.
    auto connection = child->destroyed.connect([&destroyed] { ++destroyed; });
    parent.reset();
    CHECK(destroyed == 1);

    Object other_parent;
    auto   early_child = std::make_unique<Process>(&other_parent);
    REQUIRE(other_parent.children().size() == 1);
    early_child.reset();
    CHECK(other_parent.children().empty());
}

TEST_CASE("Process rejects absent or different owner EventLoop", "[core][process]")
{
    ScopedCurrentEventLoop no_loop{nullptr};
    Process                process;
    auto                   result = process.start({.executable = "/target"});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "core.process.event_loop_unavailable");
    CHECK(result.error().category == ErrorCategory::Unavailable);

    auto                   first  = make_fake_event_loop();
    auto                   second = make_fake_event_loop();
    ScopedCurrentEventLoop first_current{first.loop.get()};
    Process                owned;
    {
        ScopedCurrentEventLoop second_current{second.loop.get()};
        result = owned.start({.executable = "/target"});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.process.event_loop_unavailable");
    }
    CHECK(owned.state() == ProcessState::NotRunning);
    CHECK_FALSE(owned.process_id());
}

TEST_CASE("Process rejects an invalid native owner EventLoop", "[core][process]")
{
    auto                   loop = EventLoopTestAccess::make_event_loop(nullptr);
    ScopedCurrentEventLoop current{loop.get()};
    Process                process;
    auto                   result = process.start({.executable = "/target"});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "core.process.event_loop_unavailable");
    CHECK(process.state() == ProcessState::NotRunning);
    CHECK_FALSE(process.process_id());
}

TEST_CASE("Process rejected launches emit no Process signals or native registrations", "[core][process]")
{
    auto                   fake = make_fake_event_loop();
    ScopedCurrentEventLoop current{fake.loop.get()};
    ScopedSigchld          disposition;
    disposition.install(false, false);
    int    signals{0};
    Object receiver;
    {
        Process process;
        auto    started    = process.started.connect(&receiver, [&signals] { ++signals; });
        auto    output     = process.standard_output.connect(&receiver, [&signals](ByteBuffer const&) { ++signals; });
        auto    diagnostic = process.standard_error.connect(&receiver, [&signals](ByteBuffer const&) { ++signals; });
        auto    finished   = process.finished.connect(&receiver, [&signals](ProcessExit const&) { ++signals; });
        auto    result     = process.start({});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.process.invalid_request");
        for (int attempt = 0; attempt < 3; ++attempt) {
            result = process.start({.executable = "/target"});
            REQUIRE_FALSE(result);
            CHECK(result.error().code == "core.process.monitor_unsupported");
            CHECK(result.error().category == ErrorCategory::Unsupported);
            CHECK(result.error().detail == "backend.not_implemented");
            CHECK(process.state() == ProcessState::NotRunning);
            CHECK_FALSE(process.process_id());
        }
        for (auto reason : {ProcessStopReason::Cancelled, ProcessStopReason::Interrupted}) {
            auto stopped = process.stop(reason);
            REQUIRE_FALSE(stopped);
            CHECK(stopped.error().code == "core.process.invalid_state");
            CHECK(stopped.error().category == ErrorCategory::Conflict);
        }
        CHECK(fake.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
        CHECK(signals == 0);
    }
    CHECK(fake.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    CHECK(signals == 0);
    CHECK(fake.backend->add_fd_calls == 0);
    CHECK(fake.backend->remove_fd_calls == 0);
    CHECK(EventLoopTestAccess::active_timer_count(*fake.loop) == 0);
}

TEST_CASE("Process observes SIGCHLD disposition without changing host policy", "[core][process]")
{
    auto                   fake = make_fake_event_loop();
    ScopedCurrentEventLoop current{fake.loop.get()};
    ScopedSigchld          guard;
    for (bool ignored : {false, true}) {
        for (bool no_wait : {false, true}) {
            guard.install(ignored, no_wait);
            auto result = validate_process_signal_configuration();
            CHECK(result.has_value() == (!ignored && !no_wait));
            Process process;
            auto    started = process.start({.executable = "/target"});
            REQUIRE_FALSE(started);
            if (ignored || no_wait) {
                REQUIRE_FALSE(result);
                CHECK(result.error().code == "core.process.signal_configuration");
                CHECK(result.error().category == ErrorCategory::Conflict);
                CHECK(result.error().detail == "sigchld.not_waitable");
                CHECK(started.error() == result.error());
            }
            else {
                CHECK(started.error().code == "core.process.monitor_unsupported");
            }
            struct sigaction observed{};
            REQUIRE(sigaction(SIGCHLD, nullptr, &observed) == 0);
            CHECK(observed.sa_handler == (ignored ? SIG_IGN : SIG_DFL));
            CHECK(((observed.sa_flags & SA_NOCLDWAIT) != 0) == no_wait);
        }
    }
}
