#pragma once

#include "attempt_executor_group.hpp"
#include "http_client.hpp"
#include "object.hpp"
#include "startup_priv.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace jb::core {
class EventLoop;
class TimeSource;
class UuidGenerator;
} // namespace jb::core

namespace jb::db {
class Database;
}

namespace jb::jobu {
class AttributeRegistry;
class CronEngine;
class ManagementService;
class SecretService;
} // namespace jb::jobu

namespace jb::rpc {
class Server;
}

namespace jb::jobud::detail {

enum class RuntimeState : std::uint8_t {
    Starting,
    Recovering,
    Serving,
    Stopping,
    Stopped
};

/// Sole-owned runner infrastructure. Reverse destruction preserves the client through executor cleanup.
struct RuntimeRunners {
    std::unique_ptr<jb::net::HttpClient>            http;
    std::unique_ptr<jb::jobu::AttemptExecutorGroup> executors;
};

struct RuntimeTestAccess;

/// Owner-thread daemon composition. Borrowed dependencies and the signal predicate's captures must outlive it.
/// Recovery precedes runner construction, synchronous dispatch, and listening. Terminal requests only latch
/// admission/persistence gates and request loop exit; run() performs destructive cleanup after stack unwinding.
/// The signal predicate is polled synchronously and must not throw, mutate storage, or reenter the runtime.
/// No Object parent is assigned to uniquely owned services. Destruction is an idempotent cleanup fallback and
/// must not occur inside a service, executor, or signal callback.
class DaemonRuntime final : public jb::core::Object {
public:
    DaemonRuntime(jb::core::EventLoop&               loop,
                  jb::db::Database&                  database,
                  jb::jobu::AttributeRegistry const& attributes,
                  jb::jobu::CronEngine const&        cron,
                  jb::core::UuidGenerator&           uuid_generator,
                  jb::core::TimeSource&              time_source,
                  StartupOptions                     options,
                  std::function<bool()>              should_stop = {});
    ~DaemonRuntime() override;

    /// Runs once against an open, schema-prepared, exclusively owned database. The factory is invoked only
    /// after recovery; it must not launch attempts. execute is called only after scheduler/listener startup.
    /// These synchronous composition seams are never retained and are also used by deterministic tests.
    /// Do not reenter run() from either callback. Returns the latched exit status after cleanup;
    /// subsequent calls return it without restarting.
    [[nodiscard]] auto run(std::function<jb::core::Result<RuntimeRunners, jb::core::Error>()> const& make_runners,
                           std::function<int()> const&                                               execute) -> int;

    /// Irreversible, idempotent and safe on a synchronous failure/signal stack; does not destroy services.
    void request_stop() noexcept;

    /// Latches failure even after an ordinary stop. Logs the first subsystem and stable code only; never logs
    /// borrowed error messages/details. subsystem must be a daemon-owned token, not request or database data.
    void fail(std::string_view subsystem, jb::core::Error const& error);

    [[nodiscard]] auto state() const noexcept -> RuntimeState;
    [[nodiscard]] auto exit_code() const noexcept -> int;

private:
    friend struct RuntimeTestAccess;
    struct Private;
    auto management() -> jb::jobu::ManagementService*;
    auto secrets() -> jb::jobu::SecretService*;
    auto scheduler() -> jb::jobu::Scheduler*;
    auto rpc_server() -> jb::rpc::Server*;
};

} // namespace jb::jobud::detail
