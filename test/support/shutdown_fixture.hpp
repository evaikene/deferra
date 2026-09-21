#pragma once

#include "application.hpp"
#include "http_test_server.hpp"
#include "recovery_fixture.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sys/types.h>

namespace jb::test {

/// Linux-only fixture for observed shutdown. Destructors are fallback cleanup, never success evidence.
class ShutdownWork final {
public:
    explicit ShutdownWork(std::function<std::unique_ptr<db::Driver>(std::unique_ptr<db::Driver>)> wrap = {});
    ~ShutdownWork();
    ShutdownWork(ShutdownWork const&)                    = delete;
    auto operator=(ShutdownWork const&) -> ShutdownWork& = delete;

    void seed(jobu::RecoveryPolicy policy);
    void await_work();
    void require_cleanup();
    void require_reaped() const;
    /// Called only after unchanged shutdown state was asserted and exclusive database ownership reopened.
    void hold_recovery();
    void require_recovery(bool retry);
    void until(std::function<bool()> const& predicate);

    /// Independent read-only connection; no statement or ownership lock survives the call.
    auto snapshot() const -> std::vector<std::vector<std::string>>;
    auto count(std::string const& sql) const -> std::int64_t;

    core::Application    app{0, nullptr};
    RecoveryFixture      storage;
    HttpTestServer       server;
    std::array<pid_t, 2> identities{};

private:
    auto                  read_identities() -> bool;
    std::filesystem::path _report;
    int                   _report_fd{-1};
    std::array<int, 2>    _pidfds{-1, -1};
    core::TimePoint       _created_at{core::Clock::now()};
};

/// Preserve the explicit isolated-environment opt-in used by existing CLI integration tests.
void require_shutdown_execution_environment();

} // namespace jb::test
