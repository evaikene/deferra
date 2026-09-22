#include "runtime_priv.hpp"

#include "connection.hpp"
#include "event_loop.hpp"
#include "jobu_version_priv.hpp"
#include "local_server.hpp"
#include "logging.hpp"
#include "management.hpp"
#include "management_rpc.hpp"
#include "object_priv.hpp"
#include "protocol.hpp"
#include "recovery_priv.hpp"
#include "secret_service.hpp"
#include "server.hpp"
#include "system_info.hpp"
#include "system_info_rpc.hpp"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace jb::jobud::detail {

namespace {

auto runtime_error(std::string code) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Unavailable,
            .code     = std::move(code),
            .message  = "The daemon runtime could not continue"};
}

} // namespace

struct DaemonRuntime::Private : jb::core::priv::ObjectPrivate {
    Private(jb::core::EventLoop&               loop_value,
            jb::db::Database&                  database_value,
            jb::jobu::AttributeRegistry const& attributes_value,
            jb::jobu::CronEngine const&        cron_value,
            jb::core::UuidGenerator&           uuid_value,
            jb::core::TimeSource&              time_value,
            StartupOptions                     options_value,
            std::function<bool()>              stop_value)
        : loop{loop_value}
        , database{database_value}
        , attributes{attributes_value}
        , cron{cron_value}
        , uuid_generator{uuid_value}
        , time_source{time_value}
        , options{std::move(options_value)}
        , should_stop{std::move(stop_value)}
        , execution{*this}
    {}

    /// The HTTP fatal notification may follow queued failed completions. Observe the stored failure
    /// at the daemon's execution boundary before any completion can persist or any runner can start.
    /// Group shutdown destroys child-owned forwarding closures before this borrowed boundary is destroyed.
    class ExecutionBoundary final : public jb::jobu::AttemptExecutor {
    public:
        explicit ExecutionBoundary(Private& runtime)
            : _runtime{runtime}
        {}

        auto is_available(jb::jobu::JobType type) const noexcept -> bool override
        {
            return !_runtime.poll_stop() && !_runtime.check_http_failure() &&
                   _runtime.runners.executors->is_available(type);
        }

        auto start(jb::jobu::AttemptStartRequest request, jb::jobu::AttemptCompletionHandler completion)
            -> jb::core::Result<void, jb::core::Error> override
        {
            if (_runtime.poll_stop() || _runtime.check_http_failure()) {
                return jb::core::Result<void, jb::core::Error>::failure(runtime_error("jobud.runtime.stopping"));
            }
            auto result = _runtime.runners.executors->start(
                std::move(request),
                [this, completion = std::move(completion)](jb::jobu::AttemptCompletion value) {
                    if (!_runtime.poll_stop() && !_runtime.check_http_failure()) {
                        completion(std::move(value));
                    }
                });
            // A rejected start can also be the first observation of a shared HTTP backend failure.
            static_cast<void>(_runtime.check_http_failure());
            return result;
        }

        auto cancel(jb::jobu::AttemptKey const& key) -> jb::core::Result<void, jb::core::Error> override
        {
            if (_runtime.poll_stop() || _runtime.check_http_failure()) {
                return jb::core::Result<void, jb::core::Error>::failure(runtime_error("jobud.runtime.stopping"));
            }
            return _runtime.runners.executors->cancel(key);
        }

    private:
        Private& _runtime;
    };

    auto stopping() const noexcept -> bool { return state == RuntimeState::Stopping || state == RuntimeState::Stopped; }

    void request_stop() noexcept
    {
        if (stopping()) {
            return;
        }
        state = RuntimeState::Stopping;
        // Admission is a predicate in the connection slot; do not close the listener or erase
        // service/executor state while one of their callbacks is still on the stack.
        if (management) {
            management->stop_mutations();
        }
        if (secrets) {
            secrets->stop_mutations();
        }
        if (scheduler) {
            scheduler->shutdown();
        }
        loop.request_quit();
    }

    void fail(std::string_view subsystem, jb::core::Error const& error)
    {
        auto const first_failure = exit_code == EXIT_SUCCESS;
        exit_code                = EXIT_FAILURE;
        request_stop();
        if (first_failure) {
            jb::core::log_error("JobU daemon failure: subsystem={} code={}", subsystem, error.code);
        }
    }

    auto poll_stop() -> bool
    {
        if (should_stop && should_stop()) {
            request_stop();
        }
        return stopping();
    }

    auto check_http_failure() -> bool
    {
        if (runners.http) {
            if (auto failure = runners.http->failure()) {
                fail("http", *failure);
                return true;
            }
        }
        return false;
    }

    auto recover() -> bool
    {
        if (poll_stop()) {
            return false;
        }
        state = RuntimeState::Recovering;
        auto recovered =
            jb::jobu::detail::recover_startup(database, attributes, cron, uuid_generator, time_source, {}, [this] {
                return poll_stop();
            });
        if (!recovered) {
            // Only the explicit cancellation result is a normal signal stop. A rollback/storage
            // error wins even if the signal predicate has already latched stopping.
            if (recovered.error().code != "jobu.recovery.cancelled" || !stopping()) {
                fail("recovery", recovered.error());
            }
            return false;
        }
        return !poll_stop();
    }

    auto start_services() -> bool
    {
        using namespace jb::jobu;

        // Recovery is complete. Establish all failure receivers before start() can dispatch synchronously.
        scheduler  = std::make_unique<Scheduler>(database,
                                                 attributes,
                                                 cron,
                                                 uuid_generator,
                                                 time_source,
                                                 execution,
                                                 scheduler_options(options));
        management = std::make_unique<ManagementService>(database, attributes, cron, uuid_generator, time_source);
        secrets    = std::make_unique<SecretService>(database, time_source);
        listener   = std::make_unique<jb::net::LocalServer>();
        rpc        = std::make_unique<jb::rpc::Server>();

        scheduler->failed.connect(owner, [this](jb::core::Error const& error) { fail("scheduler", error); });
        management->failed.connect(owner, [this](jb::core::Error const& error) { fail("management", error); });
        secrets->failed.connect(owner, [this](jb::core::Error const& error) { fail("secrets", error); });
        runners.http->failed.connect(owner, [this](jb::core::Error const& error) { fail("http", error); });
        management->mutation_committed.connect(scheduler.get(), [this] { scheduler->request_rescan(); });
        secrets->mutation_committed.connect(scheduler.get(), [this] { scheduler->request_rescan(); });

        auto capabilities = std::vector<std::string>{std::string{system_info_rpc_method_name()}};
        for (auto method : management_rpc_method_names()) {
            capabilities.emplace_back(method);
        }
        auto info = SystemInfo{
            .daemon_version = std::string{jb::jobu::detail::project_version},
            .api_version    = {.major = 1, .minor = 2},
            .capabilities   = std::move(capabilities)
        };
        if (!register_system_info_method(*rpc, std::move(info)) ||
            !register_management_methods(*rpc, *management, attributes)) {
            fail("rpc_registration", runtime_error("jobud.rpc.registration_failed"));
            return false;
        }

        admission = listener->new_connection.connect(owner, [this] {
            while (!poll_stop()) {
                auto socket = listener->take_next_connection();
                if (!socket) {
                    break;
                }
                auto const credentials    = socket->peer_credentials();
                auto       operation      = jb::rpc::OperationContext{};
                operation.peer.process_id = credentials.process_id;
                operation.peer.user_id    = credentials.user_id;
                operation.peer.group_id   = credentials.group_id;
                auto added                = rpc->add_connection(std::move(socket), std::move(operation));
                if (!added) {
                    jb::core::log_error("JobU RPC admission failed: code={}", added.error().code);
                }
            }
        });
        listener->accept_error.connect(
            [](jb::core::IOError, std::string const&) { jb::core::log_error("JobU local listener accept failed"); });
        rpc->connection_error.connect([](jb::rpc::ConnectionId, jb::core::Error const& error) {
            jb::core::log_error("JobU RPC connection failed: code={}", error.code);
        });

        // Readiness requires successful scheduler startup; listening must not expose a partially started runtime.
        if (poll_stop() || check_http_failure()) {
            return false;
        }
        auto started = scheduler->start();
        if (!started) {
            if (!stopping() || started.error().code != "jobu.scheduler.stopping") {
                fail("scheduler_start", started.error());
            }
            return false;
        }
        if (poll_stop() || check_http_failure()) {
            return false;
        }
        if (!listener->listen(options.socket_path)) {
            fail("listener", runtime_error("jobud.listen.failed"));
            return false;
        }
        state = RuntimeState::Serving;
        return !poll_stop();
    }

    void finish()
    {
        if (state == RuntimeState::Stopped) {
            return;
        }
        // The active callback stack has unwound. Close RPC before destroying runners, while persistence stays gated.
        request_stop();
        admission.disconnect();
        if (listener) {
            listener->close();
        }
        if (rpc) {
            rpc->close();
        }
        if (runners.executors) {
            // The scheduler remains alive with an invalid completion token. Runner destruction
            // cancels transfers/kills children without manufacturing durable cancelled outcomes.
            runners.executors->shutdown();
        }
        static_cast<void>(check_http_failure());
        runners.http.reset();
        rpc.reset();
        listener.reset();
        secrets.reset();
        management.reset();
        scheduler.reset();
        runners.executors.reset();
        state = RuntimeState::Stopped;
    }

    DaemonRuntime*                               owner{};
    jb::core::EventLoop&                         loop;
    jb::db::Database&                            database;
    jb::jobu::AttributeRegistry const&           attributes;
    jb::jobu::CronEngine const&                  cron;
    jb::core::UuidGenerator&                     uuid_generator;
    jb::core::TimeSource&                        time_source;
    StartupOptions                               options;
    std::function<bool()>                        should_stop;
    RuntimeState                                 state{RuntimeState::Starting};
    int                                          exit_code{EXIT_SUCCESS};
    RuntimeRunners                               runners;
    ExecutionBoundary                            execution;
    std::unique_ptr<jb::jobu::Scheduler>         scheduler;
    std::unique_ptr<jb::jobu::ManagementService> management;
    std::unique_ptr<jb::jobu::SecretService>     secrets;
    std::unique_ptr<jb::net::LocalServer>        listener;
    std::unique_ptr<jb::rpc::Server>             rpc;
    jb::core::Connection                         admission;
};

DaemonRuntime::DaemonRuntime(jb::core::EventLoop&               loop,
                             jb::db::Database&                  database,
                             jb::jobu::AttributeRegistry const& attributes,
                             jb::jobu::CronEngine const&        cron,
                             jb::core::UuidGenerator&           uuid_generator,
                             jb::core::TimeSource&              time_source,
                             StartupOptions                     options,
                             std::function<bool()>              should_stop)
    : Object{
          *new Private{loop,
                       database, attributes,
                       cron, uuid_generator,
                       time_source, std::move(options),
                       std::move(should_stop)}
}
{
    // Bind only after Object owns the private block; service connections are installed later by run().
    d_ptr<Private>()->owner = this;
}

DaemonRuntime::~DaemonRuntime()
{
    d_ptr<Private>()->finish();
}

auto DaemonRuntime::run(std::function<jb::core::Result<RuntimeRunners, jb::core::Error>()> const& make_runners,
                        std::function<int()> const&                                               execute) -> int
{
    auto& data = *d_ptr<Private>();
    if (data.state == RuntimeState::Starting && data.recover()) {
        auto runners = make_runners();
        if (!runners) {
            data.fail("runners", runners.error());
        }
        else {
            data.runners = std::move(runners).value();
            if (!data.runners.http || !data.runners.executors) {
                data.fail("runners", runtime_error("jobud.runners.invalid"));
            }
            else if (!data.poll_stop() && data.start_services()) {
                auto const result = execute();
                if (result != EXIT_SUCCESS) {
                    data.fail("event_loop", runtime_error("jobud.event_loop.failed"));
                }
            }
        }
    }
    data.finish();
    return data.exit_code;
}

void DaemonRuntime::request_stop() noexcept
{
    d_ptr<Private>()->request_stop();
}

void DaemonRuntime::fail(std::string_view subsystem, jb::core::Error const& error)
{
    d_ptr<Private>()->fail(subsystem, error);
}

auto DaemonRuntime::state() const noexcept -> RuntimeState
{
    return d_ptr<Private const>()->state;
}

auto DaemonRuntime::exit_code() const noexcept -> int
{
    return d_ptr<Private const>()->exit_code;
}

auto DaemonRuntime::management() -> jb::jobu::ManagementService*
{
    return d_ptr<Private>()->management.get();
}

auto DaemonRuntime::secrets() -> jb::jobu::SecretService*
{
    return d_ptr<Private>()->secrets.get();
}

auto DaemonRuntime::scheduler() -> jb::jobu::Scheduler*
{
    return d_ptr<Private>()->scheduler.get();
}

auto DaemonRuntime::rpc_server() -> jb::rpc::Server*
{
    return d_ptr<Private>()->rpc.get();
}

} // namespace jb::jobud::detail
