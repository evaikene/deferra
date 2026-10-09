#include "runtime_priv.hpp"

#ifdef __linux__
#  include "endpoint_guard_priv.hpp"
#endif

#include "connection.hpp"
#include "control_rpc.hpp"
#include "database.hpp"
#include "event_loop.hpp"
#include "execution_telemetry.hpp"
#include "history_rpc.hpp"
#include "history_service.hpp"
#include "jobu_version_priv.hpp"
#include "logging.hpp"
#include "management.hpp"
#include "management_rpc.hpp"
#include "object_priv.hpp"
#include "protocol.hpp"
#include "recovery.hpp"
#include "recovery_priv.hpp"
#include "retention.hpp"
#include "secret_provider_priv.hpp"
#include "secret_rpc.hpp"
#include "secret_service.hpp"
#include "server.hpp"
#include "statistics_rpc.hpp"
#include "statistics_service.hpp"
#include "system_info.hpp"
#include "system_info_rpc.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::jobud::detail {

namespace {

auto runtime_error(std::string code) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Unavailable,
        .code     = std::move(code),
        .message  = "The daemon runtime could not continue",
    };
}

void log_failure(std::string_view event, std::string_view subsystem, std::string_view code)
{
    auto const fields = std::array{
        jb::core::LogField{.name = "subsystem", .value = subsystem},
        jb::core::LogField{.name = "code",      .value = code     }
    };
    jb::core::log_event(jb::core::LogLevel::Error, event, fields);
}

void log_recovery(jb::jobu::RecoveryReport const& report)
{
    auto const fields = std::array{
        jb::core::LogField{.name = "interrupted_attempts", .value = report.interrupted_attempts},
        jb::core::LogField{.name = "retrying_runs",        .value = report.retrying_runs       },
        jb::core::LogField{.name = "terminal_runs",        .value = report.terminal_runs       },
        jb::core::LogField{.name = "finished_jobs",        .value = report.finished_jobs       },
        jb::core::LogField{.name = "inserted_successors",  .value = report.inserted_successors },
        jb::core::LogField{.name = "suspended_jobs",       .value = report.suspended_jobs      },
        jb::core::LogField{.name = "suspended_queues",     .value = report.suspended_queues    },
        jb::core::LogField{.name = "repaired_timing_rows", .value = report.repaired_timing_rows}
    };
    jb::core::log_event(jb::core::LogLevel::Info, "jobud.recovery.completed", fields);
}

void log_delayed(jb::jobu::DelayedRun const& delayed)
{
    auto const run_id   = delayed.run_id.to_string();
    auto const job_id   = delayed.job_id.to_string();
    auto const queue_id = delayed.queue_id.to_string();
    // The diagnostic intentionally makes no completeness claim: Partial wait is a known lower bound.
    auto const fields   = std::array{
        jb::core::LogField{.name = "run_id",           .value = std::string_view{run_id}                     },
        jb::core::LogField{.name = "job_id",           .value = std::string_view{job_id}                     },
        jb::core::LogField{.name = "queue_id",         .value = std::string_view{queue_id}                   },
        jb::core::LogField{.name  = "type",
                           .value = std::string_view{delayed.type == jb::jobu::JobType::Cli ? "cli" : "http"}},
        jb::core::LogField{.name = "runnable_wait_us", .value = delayed.runnable_wait.count()                },
        jb::core::LogField{.name = "threshold_ms",     .value = delayed.threshold.count()                    },
        jb::core::LogField{.name = "measurement",      .value = std::string_view{"monotonic_observed"}       }
    };
    jb::core::log_event(jb::core::LogLevel::Warning, "jobud.run.delayed", fields);
}

void log_retention(jb::jobu::RetentionPurgeCounts const& counts)
{
    auto const fields = std::array{
        jb::core::LogField{.name = "runs",                 .value = static_cast<std::uint64_t>(counts.runs)  },
        jb::core::LogField{.name  = "idempotency_records",
                           .value = static_cast<std::uint64_t>(counts.idempotency_records)                   },
        jb::core::LogField{.name = "jobs",                 .value = static_cast<std::uint64_t>(counts.jobs)  },
        jb::core::LogField{.name = "queues",               .value = static_cast<std::uint64_t>(counts.queues)}
    };
    jb::core::log_event(jb::core::LogLevel::Info, "jobud.retention.completed", fields);
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
            std::function<bool()>              stop_value,
            jb::net::LocalServerOptions        listener_value,
            EndpointGuard const*               endpoint_value)
        : loop{loop_value}
        , database{database_value}
        , attributes{attributes_value}
        , cron{cron_value}
        , uuid_generator{uuid_value}
        , time_source{time_value}
        , options{std::move(options_value)}
        , should_stop{std::move(stop_value)}
        , listener_options{listener_value}
        , endpoint{endpoint_value}
        , execution{*this}
        , secret_provider{database_value}
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
        if (retention) {
            retention->stop();
        }
        if (telemetry) {
            telemetry->request_stop();
        }
        // Admission is a predicate in the connection slot; do not close the listener or erase
        // service/executor state while one of their callbacks is still on the stack.
        if (management) {
            management->stop_mutations();
        }
        if (secrets) {
            secrets->stop_mutations();
        }
        if (statistics) {
            statistics->shutdown();
        }
        if (history) {
            history->shutdown();
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
            log_failure("jobud.failed", subsystem, error.code);
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

    auto check_endpoint() -> bool
    {
#ifdef __linux__
        if (endpoint) {
            auto verified = endpoint->verify();
            if (!verified) {
                fail("endpoint", verified.error());
                return false;
            }
        }
#endif
        return true;
    }

    auto recover() -> bool
    {
        if (poll_stop()) {
            return false;
        }
        state                      = RuntimeState::Recovering;
        // New recovery successors have no online wait before the owner activates below.
        // Existing unmeasured or abandoned intervals retain their recovery quality rules.
        auto const recovery_policy = jb::jobu::RecoveryOptions{.telemetry_covers_creation = true};
        auto       recovered       = jb::jobu::detail::recover_startup(database,
                                                                       attributes,
                                                                       cron,
                                                                       uuid_generator,
                                                                       time_source,
                                                                       recovery_policy,
                                                                       [this] { return poll_stop(); });
        if (!recovered) {
            // Only the explicit cancellation result is a normal signal stop. A rollback/storage
            // error wins even if the signal predicate has already latched stopping.
            if (recovered.error().code != "jobu.recovery.cancelled" || !stopping()) {
                fail("recovery", recovered.error());
            }
            return false;
        }
        log_recovery(*recovered);
        return !poll_stop();
    }

    auto start_services() -> bool
    {
        using namespace jb::jobu;

        auto rpc_options                      = jb::rpc::ServerOptions{};
        rpc_options.framing.max_header_bytes  = options.rpc_header_limit_bytes;
        rpc_options.framing.max_body_bytes    = options.rpc_body_limit_bytes;
        rpc_options.max_batch_entries         = options.rpc_max_batch_entries;
        rpc_options.max_connections           = options.rpc_max_connections;
        rpc_options.max_queued_output_bytes   = options.rpc_queued_output_bytes;
        rpc_options.response_limit_error_code = "jobu.response.too_large";

        // Scheduler construction registers the actual executor capabilities with the inactive
        // telemetry owner. Every failure receiver is installed before activation or dispatch.
        telemetry = std::make_unique<ExecutionTelemetry>(
            database,
            attributes,
            time_source,
            uuid_generator,
            TelemetryOptions{.checkpoint_interval = options.telemetry_checkpoint_interval});
        auto scheduler_policy      = scheduler_options(options);
        scheduler_policy.telemetry = telemetry.get();
        scheduler                  = std::make_unique<Scheduler>(database,
                                                                 attributes,
                                                                 cron,
                                                                 uuid_generator,
                                                                 time_source,
                                                                 execution,
                                                                 secret_provider,
                                                                 scheduler_policy);
        management = std::make_unique<ManagementService>(database,
                                                         attributes,
                                                         cron,
                                                         uuid_generator,
                                                         time_source,
                                                         ManagementServiceOptions{
                                                             .daemon_defaults  = options.daemon_defaults,
                                                             .default_timezone = options.default_timezone,
                                                             .telemetry        = telemetry.get(),
                                                         });
        secrets    = std::make_unique<SecretService>(database, time_source);
        statistics = std::make_unique<StatisticsService>(database,
                                                         uuid_generator,
                                                         time_source,
                                                         StatisticsServiceOptions{.runnable_wait_available = true});
        history    = std::make_unique<HistoryService>(database, attributes, uuid_generator, time_source);
        // Retention borrows the same database and starts asynchronously only after listening.
        retention =
            std::make_unique<RetentionService>(database,
                                               attributes,
                                               time_source,
                                               RetentionOptions{.default_retention = options.default_retention,
                                                                .sweep_interval    = options.history_sweep_interval,
                                                                .batch_size        = options.history_batch_size});
        listener = std::make_unique<jb::net::LocalServer>();
        rpc      = std::make_unique<jb::rpc::Server>(std::move(rpc_options));

        scheduler->failed.connect(owner, [this](jb::core::Error const& error) { fail("scheduler", error); });
        management->failed.connect(owner, [this](jb::core::Error const& error) { fail("management", error); });
        secrets->failed.connect(owner, [this](jb::core::Error const& error) { fail("secrets", error); });
        statistics->failed.connect(owner, [this](jb::core::Error const& error) { fail("statistics", error); });
        history->failed.connect(owner, [this](jb::core::Error const& error) { fail("history", error); });
        retention->failed.connect(owner, [this](jb::core::Error const& error) { fail("retention", error); });
        telemetry->failed.connect(owner, [this](jb::core::Error const& error) { fail("telemetry", error); });
        telemetry->delayed.connect(owner, [](jb::jobu::DelayedRun const& value) { log_delayed(value); });
        retention->sweep_completed.connect(owner,
                                           [](jb::jobu::RetentionPurgeCounts const& value) { log_retention(value); });
        runners.http->failed.connect(owner, [this](jb::core::Error const& error) { fail("http", error); });
        management->mutation_committed.connect(scheduler.get(), [this] { scheduler->request_rescan(); });
        secrets->mutation_committed.connect(scheduler.get(), [this] { scheduler->request_rescan(); });

        auto capabilities = std::vector<std::string>{std::string{system_info_rpc_method_name()}};
        for (auto method : management_rpc_method_names()) {
            capabilities.emplace_back(method);
        }
        for (auto method : control_rpc_method_names()) {
            capabilities.emplace_back(method);
        }
        for (auto method : secret_rpc_method_names()) {
            capabilities.emplace_back(method);
        }
        for (auto method : history_rpc_method_names()) {
            capabilities.emplace_back(method);
        }
        for (auto method : statistics_rpc_method_names()) {
            capabilities.emplace_back(method);
        }
        auto info = SystemInfo{
            .daemon_version = std::string{jb::jobu::detail::project_version},
            .api_version    = {.major = 1, .minor = 4},
            .capabilities   = std::move(capabilities),
        };
        if (!register_system_info_method(*rpc, std::move(info)) ||
            !register_management_methods(*rpc, *management, attributes) ||
            !register_control_methods(*rpc, *management, *scheduler, cron, attributes, options.default_timezone) ||
            !register_secret_methods(*rpc, *secrets) || !register_history_methods(*rpc, *history, attributes) ||
            !register_statistics_methods(*rpc, *statistics, *management)) {
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
                    log_failure("jobud.rpc.admission_failed", "rpc", added.error().code);
                }
            }
        });
        listener->accept_error.connect(owner, [](jb::core::IOError, std::string const&) {
            log_failure("jobud.listener.accept_failed", "listener", "jobud.listen.accept_failed");
        });
        rpc->connection_error.connect(owner, [](jb::rpc::ConnectionId, jb::core::Error const& error) {
            log_failure("jobud.rpc.connection_failed", "rpc", error.code);
        });

        // Activate accounting before the first scheduler pass and any executor start.
        // Listening must not expose a partially started runtime.
        if (poll_stop() || check_http_failure() || !check_endpoint()) {
            return false;
        }
        auto measured = telemetry->start();
        if (!measured) {
            fail("telemetry_start", measured.error());
            return false;
        }
        if (poll_stop() || check_http_failure() || !check_endpoint()) {
            return false;
        }
        auto started = scheduler->start();
        if (!started) {
            if (!stopping() || started.error().code != "jobu.scheduler.stopping") {
                fail("scheduler_start", started.error());
            }
            return false;
        }
        if (poll_stop() || check_http_failure() || !check_endpoint()) {
            return false;
        }
        // The transport must accept a full configured frame. Preserve main's already
        // authorized ownership and mode while replacing the independent buffer default.
        listener_options.accepted_read_buffer_limit = options.rpc_read_buffer_capacity;
        if (!listener->listen(options.socket_path, listener_options)) {
            fail("listener", runtime_error("jobud.listen.failed"));
            return false;
        }
        state = RuntimeState::Serving;
        if (poll_stop()) {
            return false;
        }
        auto maintained = retention->start();
        if (!maintained) {
            fail("retention_start", maintained.error());
            return false;
        }
        if (poll_stop() || check_http_failure()) {
            return false;
        }
        jb::core::log_event(jb::core::LogLevel::Info, "jobud.ready");
        return true;
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
        // Runner shutdown can reveal a fatal HTTP error after an ordinary stop. Only the
        // healthy path may persist timing, and it must do so before destroying its borrowers.
        if (telemetry && exit_code == EXIT_SUCCESS && !database.is_poisoned()) {
            auto settled = telemetry->finish_stop();
            if (!settled) {
                fail("telemetry_stop", settled.error());
            }
        }
        runners.http.reset();
        rpc.reset();
        listener.reset();
        retention.reset();
        statistics.reset();
        history.reset();
        secrets.reset();
        management.reset();
        scheduler.reset();
        telemetry.reset();
        runners.executors.reset();
        state = RuntimeState::Stopped;
    }

    DaemonRuntime*                                owner{};
    jb::core::EventLoop&                          loop;
    jb::db::Database&                             database;
    jb::jobu::AttributeRegistry const&            attributes;
    jb::jobu::CronEngine const&                   cron;
    jb::core::UuidGenerator&                      uuid_generator;
    jb::core::TimeSource&                         time_source;
    StartupOptions                                options;
    std::function<bool()>                         should_stop;
    jb::net::LocalServerOptions                   listener_options;
    EndpointGuard const*                          endpoint;
    RuntimeState                                  state{RuntimeState::Starting};
    int                                           exit_code{EXIT_SUCCESS};
    RuntimeRunners                                runners;
    ExecutionBoundary                             execution;
    // The provider borrows the database and outlives every scheduler dispatch.
    jb::jobu::detail::DatabaseSecretProvider      secret_provider;
    std::unique_ptr<jb::jobu::ExecutionTelemetry> telemetry;
    std::unique_ptr<jb::jobu::Scheduler>          scheduler;
    std::unique_ptr<jb::jobu::ManagementService>  management;
    std::unique_ptr<jb::jobu::SecretService>      secrets;
    std::unique_ptr<jb::jobu::StatisticsService>  statistics;
    std::unique_ptr<jb::jobu::HistoryService>     history;
    std::unique_ptr<jb::jobu::RetentionService>   retention;
    std::unique_ptr<jb::net::LocalServer>         listener;
    std::unique_ptr<jb::rpc::Server>              rpc;
    jb::core::Connection                          admission;
};

DaemonRuntime::DaemonRuntime(jb::core::EventLoop&               loop,
                             jb::db::Database&                  database,
                             jb::jobu::AttributeRegistry const& attributes,
                             jb::jobu::CronEngine const&        cron,
                             jb::core::UuidGenerator&           uuid_generator,
                             jb::core::TimeSource&              time_source,
                             StartupOptions                     options,
                             std::function<bool()>              should_stop,
                             jb::net::LocalServerOptions        listener_options,
                             EndpointGuard const*               endpoint)
    : Object{
          *new Private{loop,
                       database, attributes,
                       cron, uuid_generator,
                       time_source, std::move(options),
                       std::move(should_stop),
                       listener_options, endpoint}
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

auto DaemonRuntime::statistics() -> jb::jobu::StatisticsService*
{
    return d_ptr<Private>()->statistics.get();
}

auto DaemonRuntime::history() -> jb::jobu::HistoryService*
{
    return d_ptr<Private>()->history.get();
}

auto DaemonRuntime::scheduler() -> jb::jobu::Scheduler*
{
    return d_ptr<Private>()->scheduler.get();
}

auto DaemonRuntime::rpc_server() -> jb::rpc::Server*
{
    return d_ptr<Private>()->rpc.get();
}

auto DaemonRuntime::retention() -> jb::jobu::RetentionService*
{
    return d_ptr<Private>()->retention.get();
}

auto DaemonRuntime::telemetry() -> jb::jobu::ExecutionTelemetry*
{
    return d_ptr<Private>()->telemetry.get();
}

} // namespace jb::jobud::detail
