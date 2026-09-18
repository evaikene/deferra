#include "cli_attempt_executor.hpp"

#include "attribute_registry.hpp"
#include "cli_capture_priv.hpp"
#include "cli_exit_policy_priv.hpp"
#include "cli_job_payload_priv.hpp"
#include "event_loop.hpp"
#include "json.hpp"
#include "object_priv.hpp"
#include "process_adapter_priv.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace jb::jobu::cli {

namespace {

template <typename T = void>
using ExecutorResult = jb::core::Result<T, jb::core::Error>;

struct AttemptKeyHash {
    auto operator()(AttemptKey const& key) const noexcept -> std::size_t
    {
        auto seed = std::hash<jb::core::Uuid>{}(key.run_id);
        seed ^= std::hash<AttemptNumber>{}(key.attempt_number) + std::size_t{0x9e3779b9U} + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct CliExecutionPolicy {
    jb::core::Duration               timeout;
    jb::core::Duration               termination_grace;
    jb::jobu::detail::CliExitCodeSet retry_exit_codes;
    jb::jobu::detail::CliCaptureMode capture_mode{jb::jobu::detail::CliCaptureMode::None};
    std::size_t                      stdout_limit{0};
    std::size_t                      stderr_limit{0};
};

struct PreparedAttempt {
    jb::core::ProcessStartInfo             start_info;
    jb::jobu::detail::CliExpectedExitCodes expected_exit_codes;
    jb::jobu::detail::CliExitCodeSet       retry_exit_codes;
    jb::jobu::detail::CliCaptureMode       capture_mode{jb::jobu::detail::CliCaptureMode::None};
    std::size_t                            stdout_limit{0};
    std::size_t                            stderr_limit{0};
};

auto executor_error(jb::core::ErrorCategory category, std::string code, std::string message, std::string detail = {})
    -> jb::core::Error
{
    return {
        .category = category,
        .code     = std::move(code),
        .message  = std::move(message),
        .detail   = std::move(detail),
    };
}

auto invalid_snapshot(std::string detail) -> jb::core::Error
{
    return executor_error(jb::core::ErrorCategory::Internal,
                          "jobu.cli.invalid_snapshot",
                          "The durable CLI attempt snapshot is invalid",
                          std::move(detail));
}

auto is_safe_process_error_code(std::string_view code) noexcept -> bool
{
    constexpr auto codes = std::array{
        std::string_view{"core.process.invalid_state"},
        std::string_view{"core.process.invalid_request"},
        std::string_view{"core.process.signal_configuration"},
        std::string_view{"core.process.event_loop_unavailable"},
        std::string_view{"core.process.resource_setup_failed"},
        std::string_view{"core.process.fork_failed"},
        std::string_view{"core.process.monitor_unsupported"},
        std::string_view{"core.process.watch_failed"},
        std::string_view{"core.process.child_setup_failed"},
        std::string_view{"core.process.chdir_failed"},
        std::string_view{"core.process.security_unsupported"},
        std::string_view{"core.process.security_failed"},
        std::string_view{"core.process.exec_failed"},
        std::string_view{"core.process.signal_failed"},
        std::string_view{"core.process.stop_conflict"},
    };
    return std::ranges::find(codes, code) != codes.end();
}

auto safe_process_error_code(jb::core::Error const& error, std::string_view fallback) -> std::string
{
    return std::string{is_safe_process_error_code(error.code) ? std::string_view{error.code} : fallback};
}

template <typename T>
auto snapshot_attribute(AttributeSet const& attributes, std::string_view name) -> ExecutorResult<T const*>
{
    auto const found = attributes.find(name);
    if (found == attributes.end()) {
        return ExecutorResult<T const*>::failure(invalid_snapshot("attribute.missing"));
    }
    auto const* value = std::get_if<T>(&found->second.data);
    if (value == nullptr) {
        return ExecutorResult<T const*>::failure(invalid_snapshot("attribute.invalid_type"));
    }
    return ExecutorResult<T const*>::success(value);
}

auto decode_capture_mode(std::string const& value) -> std::optional<jb::jobu::detail::CliCaptureMode>
{
    if (value == "none") {
        return jb::jobu::detail::CliCaptureMode::None;
    }
    if (value == "on_error") {
        return jb::jobu::detail::CliCaptureMode::OnError;
    }
    if (value == "always") {
        return jb::jobu::detail::CliCaptureMode::Always;
    }
    return std::nullopt;
}

auto decode_execution_policy(AttributeSet const& attributes, StandardAttributeRegistry const& registry)
    -> ExecutorResult<CliExecutionPolicy>
{
    auto validated = registry.validate_materialized(attributes);
    if (!validated) {
        return ExecutorResult<CliExecutionPolicy>::failure(invalid_snapshot("attributes.invalid"));
    }

    auto timeout           = snapshot_attribute<jb::core::Duration>(attributes, "job.timeout");
    auto termination_grace = snapshot_attribute<jb::core::Duration>(attributes, "cli.termination_grace");
    auto retry_exit_codes  = snapshot_attribute<AttributeValue::List>(attributes, "cli.retry_exit_codes");
    auto capture           = snapshot_attribute<std::string>(attributes, "output.capture");
    auto stdout_limit      = snapshot_attribute<std::int64_t>(attributes, "output.stdout_limit");
    auto stderr_limit      = snapshot_attribute<std::int64_t>(attributes, "output.stderr_limit");
    if (!timeout || !termination_grace || !retry_exit_codes || !capture || !stdout_limit || !stderr_limit) {
        return ExecutorResult<CliExecutionPolicy>::failure(invalid_snapshot("attributes.invalid"));
    }

    auto retry_set = jb::jobu::detail::decode_cli_retry_exit_codes(**retry_exit_codes);
    auto mode      = decode_capture_mode(**capture);
    if (!retry_set || !mode) {
        return ExecutorResult<CliExecutionPolicy>::failure(invalid_snapshot("attributes.invalid"));
    }

    return ExecutorResult<CliExecutionPolicy>::success({
        .timeout           = **timeout,
        .termination_grace = **termination_grace,
        .retry_exit_codes  = std::move(*retry_set),
        .capture_mode      = *mode,
        .stdout_limit      = static_cast<std::size_t>(**stdout_limit),
        .stderr_limit      = static_cast<std::size_t>(**stderr_limit),
    });
}

auto build_environment(jb::jobu::detail::CliEnvironmentPatch const& patch, AttemptStartRequest const& request)
    -> jb::core::ProcessEnvironment
{
    auto environment = jb::core::ProcessEnvironment{};
    for (auto const& [name, value] : patch) {
        if (value) {
            environment.insert_or_assign(name, *value);
        }
        else {
            environment.erase(name);
        }
    }

    // Metadata is the final layer so later secret/environment sources cannot override durable JobU identity.
    environment.insert_or_assign("JOBU_JOB_ID", request.job_id.to_string());
    environment.insert_or_assign("JOBU_RUN_ID", request.key.run_id.to_string());
    environment.insert_or_assign("JOBU_ATTEMPT", std::to_string(request.key.attempt_number));
    return environment;
}

auto empty_capture_result() -> jb::core::JsonValue
{
    auto result = jb::core::JsonValue::Object{
        {"captured_bytes", {.data = std::uint64_t{0}}},
        {"total_bytes",    {.data = std::uint64_t{0}}},
        {"truncated",      {.data = false}           },
    };
    return jb::core::JsonValue{.data = std::move(result)};
}

auto internal_completion(AttemptKey key) -> AttemptCompletion
{
    auto result = jb::core::JsonValue::Object{
        {"capture_lost", {.data = true}                                  },
        {"error_code",   {.data = std::string{"jobu.cli.invalid_result"}}},
        {"outcome",      {.data = std::string{"internal_error"}}         },
        {"stderr",       empty_capture_result()                          },
        {"stdout",       empty_capture_result()                          },
        {"type",         {.data = std::string{"cli"}}                    },
    };
    return {
        .key                 = key,
        .outcome             = AttemptOutcome::Failed,
        .failure_disposition = FailureDisposition::Terminal,
        .result              = jb::core::JsonValue{.data = std::move(result)},
    };
}

} // anonymous namespace

struct CliAttemptExecutor::Private : jb::core::priv::ObjectPrivate {
    struct ActiveAttempt {
        ActiveAttempt(Private&                               owner_value,
                      AttemptKey                             key_value,
                      jb::jobu::detail::CliExpectedExitCodes expected_value,
                      jb::jobu::detail::CliExitCodeSet       retry_value,
                      jb::jobu::detail::CliCaptureMode       capture_mode_value,
                      std::size_t                            stdout_limit,
                      std::size_t                            stderr_limit,
                      AttemptCompletionHandler               completion_value)
            : owner{&owner_value}
            , key{key_value}
            , expected_exit_codes{expected_value}
            , retry_exit_codes{std::move(retry_value)}
            , capture_mode{capture_mode_value}
            , standard_output{stdout_limit}
            , standard_error{stderr_limit}
            , completion{std::move(completion_value)}
        {}

        Private*                                  owner{nullptr};
        AttemptKey                                key;
        std::optional<detail::ProcessOperationId> operation_id;
        std::unique_ptr<detail::ProcessOperation> operation;
        jb::jobu::detail::CliExpectedExitCodes    expected_exit_codes;
        jb::jobu::detail::CliExitCodeSet          retry_exit_codes;
        jb::jobu::detail::CliCaptureMode          capture_mode{jb::jobu::detail::CliCaptureMode::None};
        jb::jobu::detail::CliCaptureBuffer        standard_output;
        jb::jobu::detail::CliCaptureBuffer        standard_error;
        std::array<bool, 2>                       capture_disabled{false, false};
        bool                                      capture_lost{false};
        AttemptCompletionHandler                  completion;
    };

    Private(CliAttemptExecutorOptions                       options_value,
            std::unique_ptr<detail::ProcessAdapter>         adapter_value,
            std::unique_ptr<detail::EffectiveIdentityProbe> identity_value)
        : options{options_value}
        , adapter{std::move(adapter_value)}
        , identity{std::move(identity_value)}
    {}

    void bind_owner(CliAttemptExecutor& value) noexcept { owner = &value; }

    CliAttemptExecutor*                                                            owner{nullptr};
    CliAttemptExecutorOptions                                                      options;
    StandardAttributeRegistry                                                      attributes;
    std::unique_ptr<detail::ProcessAdapter>                                        adapter;
    std::unique_ptr<detail::EffectiveIdentityProbe>                                identity;
    std::unordered_map<AttemptKey, std::shared_ptr<ActiveAttempt>, AttemptKeyHash> active_by_attempt;
    std::unordered_map<detail::ProcessOperationId, std::shared_ptr<ActiveAttempt>> active_by_operation;

    [[nodiscard]] auto owner_loop_available() const noexcept -> bool
    {
        auto* loop = owner->event_loop();
        return loop != nullptr && loop == jb::core::EventLoop::current() && loop->is_valid();
    }

    [[nodiscard]] auto is_available(JobType type) const noexcept -> bool
    {
        return type == JobType::Cli && adapter != nullptr && owner_loop_available() &&
               (options.allow_root || identity->effective_user_id() != 0U);
    }

    [[nodiscard]] auto prepare_attempt(AttemptStartRequest const& request) const -> ExecutorResult<PreparedAttempt>
    {
        auto policy = decode_execution_policy(request.attributes, attributes);
        if (!policy) {
            return ExecutorResult<PreparedAttempt>::failure(std::move(policy).error());
        }
        auto payload = jb::jobu::detail::decode_cli_job_payload(request.payload);
        if (!payload) {
            return ExecutorResult<PreparedAttempt>::failure(
                invalid_snapshot(std::string{jb::jobu::detail::job_payload_issue_text(payload.error())}));
        }

        auto start_info = jb::core::ProcessStartInfo{
            .executable        = std::move(payload->command),
            .arguments         = std::move(payload->arguments),
            .environment       = build_environment(payload->environment, request),
            .working_directory = std::filesystem::path{std::move(payload->working_directory)},
            .timeout           = policy->timeout,
            .termination_grace = policy->termination_grace,
            .require_non_root  = !options.allow_root,
#if defined(__linux__)
            .prevent_privilege_gain = true,
#else
            .prevent_privilege_gain = false,
#endif
        };
        return ExecutorResult<PreparedAttempt>::success({
            .start_info          = std::move(start_info),
            .expected_exit_codes = payload->expected_exit_codes,
            .retry_exit_codes    = std::move(policy->retry_exit_codes),
            .capture_mode        = policy->capture_mode,
            .stdout_limit        = policy->stdout_limit,
            .stderr_limit        = policy->stderr_limit,
        });
    }

    [[nodiscard]] auto matches_active_operation(std::shared_ptr<ActiveAttempt> const& active,
                                                detail::ProcessOperationId            id) const -> bool
    {
        if (active->owner != this || !active->operation_id || *active->operation_id != id) {
            return false;
        }
        auto const by_key = active_by_attempt.find(active->key);
        auto const by_id  = active_by_operation.find(id);
        return by_key != active_by_attempt.end() && by_key->second == active && by_id != active_by_operation.end() &&
               by_id->second == active;
    }

    void append_output(std::shared_ptr<ActiveAttempt> const& active,
                       detail::ProcessOperationId            id,
                       jb::core::ByteView                    bytes,
                       std::size_t                           channel) const
    {
        if (!matches_active_operation(active, id) || active->capture_disabled[channel]) {
            return;
        }

        auto appended = channel == 0U ? active->standard_output.append(bytes) : active->standard_error.append(bytes);
        if (!appended) {
            // Capture failure must not become process backpressure or discard the accepted completion obligation.
            active->capture_disabled[channel] = true;
            active->capture_lost              = true;
        }
    }

    void finish(std::shared_ptr<ActiveAttempt> const& active,
                detail::ProcessOperationId            id,
                jb::core::ProcessExit const&          process_exit)
    {
        if (!matches_active_operation(active, id)) {
            return;
        }

        auto capture = jb::jobu::detail::CliCaptureSnapshot{
            .standard_output = active->standard_output.take(),
            .standard_error  = active->standard_error.take(),
            .capture_lost    = active->capture_lost,
        };
        auto mapped = jb::jobu::detail::map_cli_completion(process_exit,
                                                           active->expected_exit_codes,
                                                           active->retry_exit_codes,
                                                           active->capture_mode,
                                                           std::move(capture));
        auto completion = mapped ? AttemptCompletion{
                                       .key                 = active->key,
                                       .outcome             = mapped->outcome,
                                       .failure_disposition = mapped->failure_disposition,
                                       .result              = std::move(mapped->result),
                                       .output              = std::move(mapped->output),
                                   }
                                 : internal_completion(active->key);

        // Both indexes, the adapter handle, and the callback gate are retired before reentrant user code can run.
        auto handler            = std::move(active->completion);
        auto operation          = std::move(active->operation);
        active->owner           = nullptr;
        auto const operation_id = *active->operation_id;
        active_by_operation.erase(operation_id);
        active_by_attempt.erase(active->key);
        operation->retire();
        operation.reset();

        handler(std::move(completion));
    }

    void abandon(std::shared_ptr<ActiveAttempt> const& active) noexcept
    {
        active->owner      = nullptr;
        active->completion = {};
        if (active->operation) {
            active->operation->shutdown();
            active->operation.reset();
        }
        if (active->operation_id) {
            active_by_operation.erase(*active->operation_id);
        }
        active_by_attempt.erase(active->key);
    }

    [[nodiscard]] auto start(AttemptStartRequest const& request, AttemptCompletionHandler completion)
        -> ExecutorResult<>
    {
        if (request.type != JobType::Cli) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Unsupported,
                                                            "jobu.cli.unsupported_type",
                                                            "The CLI executor supports only CLI attempts"));
        }
        if (!completion || request.key.run_id.is_nil() || request.key.attempt_number == 0) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::InvalidArgument,
                                                            "jobu.cli.invalid_start",
                                                            "The CLI attempt start input is invalid"));
        }
        if (active_by_attempt.contains(request.key)) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Conflict,
                                                            "jobu.cli.duplicate_attempt",
                                                            "The CLI attempt is already active"));
        }

        auto prepared = prepare_attempt(request);
        if (!prepared) {
            return ExecutorResult<>::failure(std::move(prepared).error());
        }
        if (!owner_loop_available()) {
            return ExecutorResult<>::failure(
                executor_error(jb::core::ErrorCategory::Unavailable,
                               "jobu.cli.event_loop_unavailable",
                               "The CLI executor requires a valid current owner EventLoop"));
        }
        if (!adapter) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Unavailable,
                                                            "jobu.cli.start_failed",
                                                            "The process adapter rejected the CLI attempt start",
                                                            "core.process.monitor_unsupported"));
        }

        auto active = std::make_shared<ActiveAttempt>(*this,
                                                      request.key,
                                                      prepared->expected_exit_codes,
                                                      std::move(prepared->retry_exit_codes),
                                                      prepared->capture_mode,
                                                      prepared->stdout_limit,
                                                      prepared->stderr_limit,
                                                      std::move(completion));
        active_by_attempt.emplace(active->key, active);
        auto sink = detail::ProcessEventSink{
            .standard_output =
                [active](detail::ProcessOperationId id, jb::core::ByteView bytes) {
                    if (active->owner != nullptr) {
                        active->owner->append_output(active, id, bytes, 0U);
                    }
                },
            .standard_error =
                [active](detail::ProcessOperationId id, jb::core::ByteView bytes) {
                    if (active->owner != nullptr) {
                        active->owner->append_output(active, id, bytes, 1U);
                    }
                },
            .finished =
                [active](detail::ProcessOperationId id, jb::core::ProcessExit const& process_exit) {
                    if (active->owner != nullptr) {
                        active->owner->finish(active, id, process_exit);
                    }
                },
        };

        // This is the last policy check before the adapter can create external work; availability alone is not a
        // security boundary because process credentials may have changed since scheduling admission.
        if (!options.allow_root && identity->effective_user_id() == 0U) {
            abandon(active);
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::PermissionDenied,
                                                            "jobu.cli.root_forbidden",
                                                            "CLI execution as root is not enabled"));
        }

        auto started = adapter->start(std::move(prepared->start_info), std::move(sink));
        if (!started) {
            auto detail_code = safe_process_error_code(started.error(), "core.process.resource_setup_failed");
            abandon(active);
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Unavailable,
                                                            "jobu.cli.start_failed",
                                                            "The process adapter rejected the CLI attempt start",
                                                            std::move(detail_code)));
        }

        auto operation = std::move(started).value();
        if (!operation || operation->id() == 0U || active_by_operation.contains(operation->id())) {
            if (operation) {
                operation->shutdown();
            }
            abandon(active);
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::Internal,
                                                            "jobu.cli.start_failed",
                                                            "The process adapter returned an invalid operation",
                                                            "core.process.invalid_state"));
        }

        active->operation_id = operation->id();
        active->operation    = std::move(operation);
        active_by_operation.emplace(*active->operation_id, active);
        return ExecutorResult<>::success();
    }

    [[nodiscard]] auto cancel(AttemptKey const& key) -> ExecutorResult<>
    {
        auto const found = active_by_attempt.find(key);
        if (found == active_by_attempt.end()) {
            return ExecutorResult<>::failure(executor_error(jb::core::ErrorCategory::NotFound,
                                                            "jobu.cli.attempt_not_found",
                                                            "The CLI attempt is not active"));
        }

        auto stopped = found->second->operation->stop(jb::core::ProcessStopReason::Cancelled);
        if (!stopped) {
            return ExecutorResult<>::failure(
                executor_error(jb::core::ErrorCategory::Unavailable,
                               "jobu.cli.cancel_failed",
                               "The process adapter rejected CLI attempt cancellation",
                               safe_process_error_code(stopped.error(), "core.process.signal_failed")));
        }
        return ExecutorResult<>::success();
    }

    void shutdown() noexcept
    {
        // Disable every callback gate before the first cleanup request; an adapter must never re-enter scheduler code
        // while executor destruction is unwinding the active set.
        for (auto& [key, active] : active_by_attempt) {
            active->owner      = nullptr;
            active->completion = {};
        }
        active_by_operation.clear();

        for (auto& [key, active] : active_by_attempt) {
            if (active->operation) {
                active->operation->shutdown();
                active->operation.reset();
            }
        }
        active_by_attempt.clear();
    }
};

CliAttemptExecutor::CliAttemptExecutor(CliAttemptExecutorOptions options, jb::core::Object* parent)
    : CliAttemptExecutor(std::make_unique<Private>(options, nullptr, detail::make_system_identity_probe()), parent)
{
    // Production Process children may borrow this executor only after Object owns the private block and bind_owner()
    // has established the back-reference used by receiver-aware signal delivery.
    d_ptr<Private>()->adapter = detail::make_system_process_adapter(*this);
}

CliAttemptExecutor::CliAttemptExecutor(std::unique_ptr<Private> data, jb::core::Object* parent)
    : Object(*data, parent)
{
    // The parameter retains the block if base construction throws. Object owns it before the back-reference is bound.
    data.release()->bind_owner(*this);
}

CliAttemptExecutor::~CliAttemptExecutor()
{
    d_ptr<Private>()->shutdown();
}

auto CliAttemptExecutor::is_available(JobType type) const noexcept -> bool
{
    auto const* data = d_ptr<Private>();
    return data->is_available(type);
}

auto CliAttemptExecutor::start(AttemptStartRequest request, AttemptCompletionHandler completion) -> ExecutorResult<>
{
    return d_ptr<Private>()->start(request, std::move(completion));
}

auto CliAttemptExecutor::cancel(AttemptKey const& key) -> ExecutorResult<>
{
    return d_ptr<Private>()->cancel(key);
}

auto detail::CliAttemptExecutorTestAccess::create(CliAttemptExecutorOptions               options,
                                                  std::unique_ptr<ProcessAdapter>         adapter,
                                                  std::unique_ptr<EffectiveIdentityProbe> identity)
    -> std::unique_ptr<CliAttemptExecutor>
{
    if (!identity) {
        identity = make_system_identity_probe();
    }
    auto data = std::make_unique<CliAttemptExecutor::Private>(options, std::move(adapter), std::move(identity));
    return std::unique_ptr<CliAttemptExecutor>{
        new CliAttemptExecutor{std::move(data), nullptr}
    };
}

#if defined(__linux__)
auto detail::CliAttemptExecutorTestAccess::create_with_system_process_adapter(
    CliAttemptExecutorOptions                          options,
    std::unique_ptr<EffectiveIdentityProbe>            identity,
    std::shared_ptr<jb::core::priv::ProcessOperations> process_operations) -> std::unique_ptr<CliAttemptExecutor>
{
    if (!identity) {
        identity = make_system_identity_probe();
    }

    auto data     = std::make_unique<CliAttemptExecutor::Private>(options, nullptr, std::move(identity));
    auto executor = std::unique_ptr<CliAttemptExecutor>{
        new CliAttemptExecutor{std::move(data), nullptr}
    };
    executor->d_ptr<CliAttemptExecutor::Private>()->adapter =
        make_system_process_adapter_for_test(*executor, std::move(process_operations));
    return executor;
}
#endif

} // namespace jb::jobu::cli
