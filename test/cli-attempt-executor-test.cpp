#include "cli/cli_attempt_executor.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "cli/process_adapter_priv.hpp"
#include "json.hpp"
#include "support/fake_process_adapter.hpp"
#include "uuid.hpp"

#if defined(__linux__) || defined(__APPLE__)
#  include "process_posix_priv.hpp"
#  include "support/temporary_directory.hpp"
#endif

#if defined(__linux__)
#  include "event_loop_backend_epoll_priv.hpp"
#elif defined(__APPLE__)
#  include <sys/event.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::cli;
using namespace jb::jobu::cli::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

auto json_string(std::string value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_uint(std::uint64_t value) -> JsonValue
{
    return JsonValue{.data = value};
}

auto json_array(JsonValue::Array value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto json_object(JsonValue::Object value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto uuid(std::string_view value) -> Uuid
{
    auto parsed = Uuid::parse(value);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto job_id() -> Uuid
{
    return uuid("11111111-1111-4111-8111-111111111111");
}

auto run_id() -> Uuid
{
    return uuid("22222222-2222-4222-8222-222222222222");
}

auto queue_id() -> Uuid
{
    return uuid("33333333-3333-4333-8333-333333333333");
}

auto string_list(std::initializer_list<std::string_view> values) -> AttributeValue::List
{
    auto result = AttributeValue::List{};
    result.reserve(values.size());
    for (auto value : values) {
        result.push_back({.data = std::string{value}});
    }
    return result;
}

auto materialized_attributes(AttributeSet const& overrides = {}) -> AttributeSet
{
    StandardAttributeRegistry registry;
    auto                      attributes = materialize_attributes(registry, {}, {}, overrides);
    REQUIRE(attributes);
    return std::move(attributes).value();
}

auto minimal_payload(std::string command = "/bin/test-command") -> JsonValue
{
    return json_object({
        {"command", json_string(std::move(command))}
    });
}

auto complete_payload() -> JsonValue
{
    return json_object({
        {"arguments",           json_array({json_string("first"), json_string("--second")})},
        {"command",             json_string("test-command")                                },
        {"environment",
         json_object({
             {"KEEP", json_string("literal-value")},
             {"PATH", json_string("/bin:/usr/bin")},
             {"REMOVE", JsonValue{}},
         })                                                                                },
        {"expected_exit_codes", json_array({json_uint(0), json_uint(7)})                   },
        {"working_directory",   json_string("/work")                                       },
    });
}

auto start_request(AttemptNumber       attempt_number = 1,
                   AttributeSet const& overrides      = {},
                   JsonValue           payload        = minimal_payload()) -> AttemptStartRequest
{
    return {
        .key        = {.run_id = run_id(), .attempt_number = attempt_number},
        .job_id     = job_id(),
        .queue_id   = queue_id(),
        .type       = JobType::Cli,
        .attributes = materialized_attributes(overrides),
        .payload    = std::move(payload),
        .started_at = UtcTimePoint{10s},
    };
}

auto byte_text(ByteBuffer const& value) -> std::string_view
{
    return as_string_view(ByteView{value});
}

auto result_string(AttemptCompletion const& completion, std::string_view name) -> std::string const&
{
    return completion.result.as_object().at(std::string{name}).as_string();
}

struct TestApplication {
    std::array<char const*, 2> arguments{"cli-attempt-executor-test", nullptr};
    Application                application{1, arguments.data()};
};

struct ExecutorFixture {
    explicit ExecutorFixture(CliAttemptExecutorOptions options = {}, std::uint64_t effective_user_id = 1000U)
    {
        auto adapter_owner  = std::make_unique<FakeProcessAdapter>();
        adapter             = adapter_owner.get();
        observation         = adapter_owner->observation();
        auto identity_owner = std::make_unique<FakeEffectiveIdentityProbe>(effective_user_id);
        identity            = identity_owner.get();
        executor = CliAttemptExecutorTestAccess::create(options, std::move(adapter_owner), std::move(identity_owner));
    }

    TestApplication                                application;
    FakeProcessAdapter*                            adapter{nullptr};
    FakeEffectiveIdentityProbe*                    identity{nullptr};
    std::shared_ptr<FakeProcessAdapterObservation> observation;
    std::unique_ptr<CliAttemptExecutor>            executor;
};

auto exited(int code, bool stdout_lost = false, bool stderr_lost = false) -> ProcessExit
{
    return {
        .kind        = ProcessExitKind::Exited,
        .exit_code   = code,
        .stdout_lost = stdout_lost,
        .stderr_lost = stderr_lost,
    };
}

#if defined(__linux__) || defined(__APPLE__)
struct RealCliPayload {
    std::string                          command{PROCESS_TEST_HELPER};
    std::vector<std::string>             arguments;
    std::optional<std::filesystem::path> working_directory;
    JsonValue::Object                    environment;
    std::vector<std::uint64_t>           expected_exit_codes{0U};
};

auto real_cli_payload(RealCliPayload input) -> JsonValue
{
    auto arguments = JsonValue::Array{};
    arguments.reserve(input.arguments.size());
    for (auto& argument : input.arguments) {
        arguments.push_back(json_string(std::move(argument)));
    }

    auto expected_exit_codes = JsonValue::Array{};
    expected_exit_codes.reserve(input.expected_exit_codes.size());
    for (auto code : input.expected_exit_codes) {
        expected_exit_codes.push_back(json_uint(code));
    }

    auto payload = JsonValue::Object{
        {"arguments",           json_array(std::move(arguments))          },
        {"command",             json_string(std::move(input.command))     },
        {"expected_exit_codes", json_array(std::move(expected_exit_codes))},
    };
    if (input.working_directory) {
        payload.emplace("working_directory", json_string(input.working_directory->string()));
    }
    if (!input.environment.empty()) {
        payload.emplace("environment", json_object(std::move(input.environment)));
    }
    return json_object(std::move(payload));
}

auto real_start_request(AttemptNumber attempt_number, RealCliPayload payload, AttributeSet const& overrides = {})
    -> AttemptStartRequest
{
    return start_request(attempt_number, overrides, real_cli_payload(std::move(payload)));
}

auto helper_start_request(AttemptNumber              attempt_number,
                          std::vector<std::string>   arguments,
                          AttributeSet const&        overrides           = {},
                          std::vector<std::uint64_t> expected_exit_codes = {0U}) -> AttemptStartRequest
{
    auto payload                = RealCliPayload{};
    payload.arguments           = std::move(arguments);
    payload.expected_exit_codes = std::move(expected_exit_codes);
    return real_start_request(attempt_number, std::move(payload), overrides);
}

struct RealExecutorFixture {
    TestApplication    application;
    CliAttemptExecutor executor{CliAttemptExecutorOptions{.allow_root = true}};

    /// @throws Catch::TestFailureException when native readiness fails or the watchdog expires.
    void until(std::function<bool()> const& predicate)
    {
        auto const deadline = Clock::now() + 5s;
        while (!predicate() && Clock::now() < deadline) {
            REQUIRE(application.application.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
        }
        REQUIRE(predicate());
    }

    /// @throws Catch::TestFailureException when the attempt is rejected or does not complete exactly once.
    auto execute(AttemptStartRequest request) -> AttemptCompletion
    {
        auto completion     = std::optional<AttemptCompletion>{};
        auto callback_count = std::size_t{0};
        auto accepted       = executor.start(std::move(request), [&](AttemptCompletion result) {
            ++callback_count;
            completion = std::move(result);
        });
        if (!accepted) {
            UNSCOPED_INFO("start error: " << accepted.error().code << " " << accepted.error().detail);
        }
        REQUIRE(accepted);
        CHECK(callback_count == 0U);

        until([&] { return completion.has_value(); });
        CHECK(callback_count == 1U);
        until([&] { return executor.children().empty(); });
        REQUIRE(application.application.process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(callback_count == 1U);
        return std::move(*completion);
    }
};

auto reported_root_identity() noexcept -> uid_t
{
    return 0;
}

class RootChildIdentityOperations final : public jb::core::priv::ProcessOperations {
public:
    auto child_options() noexcept -> jb::core::priv::ProcessChildOptions override
    {
        auto options          = ProcessOperations::child_options();
        options.effective_uid = reported_root_identity;
        return options;
    }
};

/// The target opens this FIFO after exec, making helper readiness independent of elapsed time.
class PostExecReport {
public:
    PostExecReport()
        : _path{_directory.path() / "report"}
    {
        REQUIRE(::mkfifo(_path.c_str(), 0600) == 0);
        _descriptor = ::open(_path.c_str(), O_RDWR | O_CLOEXEC);
        REQUIRE(_descriptor >= 0);
    }

    ~PostExecReport()
    {
        if (_descriptor >= 0) {
            ::close(_descriptor);
        }
    }

    PostExecReport(PostExecReport const&)                    = delete;
    auto operator=(PostExecReport const&) -> PostExecReport& = delete;

    [[nodiscard]] auto path() const -> std::string { return _path.string(); }

    /// @throws Catch::TestFailureException when the helper report does not arrive before the watchdog.
    void read(void* destination, std::size_t size) const
    {
        auto*       bytes    = static_cast<char*>(destination);
        std::size_t received = 0;
        while (received < size) {
            pollfd item{.fd = _descriptor, .events = POLLIN, .revents = 0};
            REQUIRE(::poll(&item, 1, 5000) == 1);
            auto const count = ::read(_descriptor, bytes + received, size - received);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            REQUIRE(count > 0);
            received += static_cast<std::size_t>(count);
        }
    }

private:
    TemporaryDirectory    _directory;
    std::filesystem::path _path;
    int                   _descriptor{-1};
};

/// Identity-stable observation for a helper process that may be reaped by Process or adopted elsewhere.
class ProcessTerminationWatch final {
public:
    explicit ProcessTerminationWatch(pid_t process_id)
        : _process_id{process_id}
    {
        REQUIRE(process_id > 0);
#  if defined(__APPLE__)
        // Register while the coordinated helper is alive; NOTE_EXIT remains observable after its owner reaps it.
        _descriptor = ::kqueue();
        REQUIRE(_descriptor >= 0);
        struct kevent change;
        EV_SET(&change, process_id, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        int registered;
        do {
            registered = ::kevent(_descriptor, &change, 1, nullptr, 0, nullptr);
        } while (registered < 0 && errno == EINTR);
        if (registered < 0) {
            auto const error = errno;
            ::close(_descriptor);
            _descriptor = -1;
            FAIL("kevent registration failed with errno " << error);
        }
#  else
        jb::core::priv::EpollProcessOperations operations;
        _descriptor = operations.open_pidfd(process_id);
        REQUIRE(_descriptor >= 0);
#  endif
    }

    ~ProcessTerminationWatch()
    {
        if (_descriptor >= 0) {
            ::close(_descriptor);
        }
    }

    ProcessTerminationWatch(ProcessTerminationWatch const&)                    = delete;
    auto operator=(ProcessTerminationWatch const&) -> ProcessTerminationWatch& = delete;

    /// @throws Catch::TestFailureException when the watched identity remains alive past the watchdog.
    void check_terminated() const
    {
#  if defined(__APPLE__)
        struct kevent   event;
        struct timespec timeout{.tv_sec = 5, .tv_nsec = 0};
        int             ready;
        do {
            ready = ::kevent(_descriptor, nullptr, 0, &event, 1, &timeout);
        } while (ready < 0 && errno == EINTR);
        REQUIRE(ready == 1);
        CHECK(event.filter == EVFILT_PROC);
        CHECK(event.ident == static_cast<std::uintptr_t>(_process_id));
        CHECK((event.fflags & NOTE_EXIT) != 0);
#  else
        pollfd item{.fd = _descriptor, .events = POLLIN, .revents = 0};
        int    ready;
        do {
            ready = ::poll(&item, 1, 5000);
        } while (ready < 0 && errno == EINTR);

        if (ready < 0) {
            UNSCOPED_INFO("poll(pidfd for " << _process_id << ") failed with errno " << errno);
        }
        REQUIRE(ready == 1);
        CHECK((item.revents & POLLIN) != 0);
#  endif
    }

private:
    pid_t _process_id;
    int   _descriptor{-1};
};

auto captured_pattern(std::size_t total, std::size_t limit, std::size_t channel) -> ByteBuffer
{
    auto full = ByteBuffer{};
    full.reserve(total);
    for (std::size_t offset = 0; offset < total; ++offset) {
        full.push_back(static_cast<std::byte>((offset + (channel * 73U)) % 251U));
    }
    if (total <= limit) {
        return full;
    }

    auto const prefix = (limit + 1U) / 2U;
    auto const suffix = limit / 2U;
    auto       result = ByteBuffer{full.begin(), full.begin() + static_cast<std::ptrdiff_t>(prefix)};
    result.insert(result.end(), full.end() - static_cast<std::ptrdiff_t>(suffix), full.end());
    return result;
}
#endif

} // anonymous namespace

TEST_CASE("CLI attempt executor exposes Object ownership and dynamic availability", "[jobu][cli][executor]")
{
    SECTION("public construction follows platform Process and identity availability")
    {
        TestApplication    application;
        CliAttemptExecutor executor;

#if defined(__linux__) || defined(__APPLE__)
        CHECK(executor.is_available(JobType::Cli) == (::geteuid() != 0));
#else
        CHECK_FALSE(executor.is_available(JobType::Cli));
        auto rejected = executor.start(start_request(), [](AttemptCompletion const&) {});
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "jobu.cli.start_failed");
        CHECK(rejected.error().detail == "core.process.monitor_unsupported");
#endif
        CHECK_FALSE(executor.is_available(JobType::Http));
    }

    SECTION("private fake construction follows type loop and identity policy")
    {
        ExecutorFixture fixture;

        CHECK(fixture.executor->is_available(JobType::Cli));
        CHECK_FALSE(fixture.executor->is_available(JobType::Http));
        CHECK_FALSE(fixture.executor->is_available(static_cast<JobType>(255)));
        fixture.identity->set_effective_user_id(0);
        CHECK_FALSE(fixture.executor->is_available(JobType::Cli));
    }

    SECTION("unsafe override permits an injected root identity")
    {
        ExecutorFixture fixture{{.allow_root = true}, 0};
        CHECK(fixture.executor->is_available(JobType::Cli));
        REQUIRE(fixture.executor->start(start_request(), [](AttemptCompletion const&) {}));
        REQUIRE(fixture.observation->starts.size() == 1U);
        CHECK_FALSE(fixture.observation->starts.front().start_info.require_non_root);
    }

    SECTION("Object parent owns a heap-allocated public executor")
    {
        Object parent;
        auto*  child = new CliAttemptExecutor(CliAttemptExecutorOptions{}, &parent);
        REQUIRE(parent.children().size() == 1U);
        CHECK(parent.children().front() == child);
        CHECK(child->parent() == &parent);
    }
}

TEST_CASE("CLI attempt executor requires its current owner EventLoop", "[jobu][cli][executor]")
{
    auto adapter_owner  = std::make_unique<FakeProcessAdapter>();
    auto identity_owner = std::make_unique<FakeEffectiveIdentityProbe>();
    auto executor       = CliAttemptExecutorTestAccess::create({}, std::move(adapter_owner), std::move(identity_owner));

    CHECK_FALSE(executor->is_available(JobType::Cli));
    auto result = executor->start(start_request(), [](AttemptCompletion const&) {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.cli.event_loop_unavailable");
}

TEST_CASE("CLI attempt executor prepares explicit process policy and metadata", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            overrides = AttributeSet{
        {"cli.retry_exit_codes",  {.data = string_list({"2-4"})} },
        {"cli.termination_grace", {.data = 3s}                   },
        {"job.timeout",           {.data = 42s}                  },
        {"output.capture",        {.data = std::string{"always"}}},
        {"output.stderr_limit",   {.data = std::int64_t{5}}      },
        {"output.stdout_limit",   {.data = std::int64_t{7}}      },
    };
    auto completions = std::vector<AttemptCompletion>{};

    REQUIRE(fixture.executor->start(start_request(1, overrides, complete_payload()), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    }));
    CHECK(completions.empty());
    REQUIRE(fixture.observation->starts.size() == 1U);

    auto const& accepted = fixture.observation->starts.front();
    CHECK(accepted.start_info.executable == "test-command");
    CHECK(accepted.start_info.arguments == std::vector<std::string>{"first", "--second"});
    CHECK(accepted.start_info.working_directory == "/work");
    CHECK(accepted.start_info.timeout == 42s);
    CHECK(accepted.start_info.termination_grace == 3s);
    CHECK(accepted.start_info.require_non_root);
#if defined(__linux__)
    CHECK(accepted.start_info.prevent_privilege_gain);
#else
    CHECK_FALSE(accepted.start_info.prevent_privilege_gain);
#endif
    CHECK(accepted.start_info.environment.size() == 5U);
    CHECK(accepted.start_info.environment.at("KEEP") == "literal-value");
    CHECK(accepted.start_info.environment.at("PATH") == "/bin:/usr/bin");
    CHECK_FALSE(accepted.start_info.environment.contains("REMOVE"));
    CHECK(accepted.start_info.environment.at("JOBU_JOB_ID") == job_id().to_string());
    CHECK(accepted.start_info.environment.at("JOBU_RUN_ID") == run_id().to_string());
    CHECK(accepted.start_info.environment.at("JOBU_ATTEMPT") == "1");
    CHECK_FALSE(accepted.start_info.environment.contains("HOME"));

    REQUIRE(fixture.adapter->emit_standard_output(accepted.id, as_bytes("0123456789")));
    REQUIRE(fixture.adapter->emit_standard_error(accepted.id, as_bytes("abcdefgh")));
    REQUIRE(fixture.adapter->finish(accepted.id, exited(7)));

    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].key == AttemptKey{.run_id = run_id(), .attempt_number = 1});
    CHECK(completions[0].outcome == AttemptOutcome::Succeeded);
    REQUIRE(completions[0].output);
    REQUIRE(completions[0].output->primary);
    REQUIRE(completions[0].output->diagnostic);
    CHECK(byte_text(completions[0].output->primary->bytes) == "0123789");
    CHECK(byte_text(completions[0].output->diagnostic->bytes) == "abcgh");
    CHECK(fixture.observation->retired == std::vector<ProcessOperationId>{accepted.id});
}

TEST_CASE("CLI attempt executor rejects invalid starts without retaining callbacks", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            callback_count = std::size_t{0};
    auto            callback       = [&](AttemptCompletion const&) { ++callback_count; };

    SECTION("unsupported type")
    {
        auto request = start_request();
        request.type = JobType::Http;
        auto result  = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.unsupported_type");
    }

    SECTION("zero attempt number")
    {
        auto request               = start_request();
        request.key.attempt_number = 0;
        auto result                = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_start");
    }

    SECTION("nil run ID")
    {
        auto request       = start_request();
        request.key.run_id = {};
        auto result        = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_start");
    }

    SECTION("empty completion handler")
    {
        auto result = fixture.executor->start(start_request(), {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_start");
    }

    SECTION("invalid materialized attributes")
    {
        auto request = start_request();
        request.attributes.erase("job.timeout");
        auto result = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_snapshot");
        CHECK(result.error().detail == "attributes.invalid");
    }

    SECTION("invalid durable payload")
    {
        auto request    = start_request();
        request.payload = json_object({});
        auto result     = fixture.executor->start(std::move(request), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.invalid_snapshot");
        CHECK(result.error().detail == "missing_command");
    }

    SECTION("adapter rejection exposes only a stable process code")
    {
        fixture.adapter->set_start_error(Error{
            .category = ErrorCategory::Unavailable,
            .code     = "core.process.watch_failed",
            .message  = "private-marker-message",
            .detail   = "private-marker-detail",
        });
        auto result = fixture.executor->start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.start_failed");
        CHECK(result.error().detail == "core.process.watch_failed");
        CHECK(result.error().message.find("private-marker") == std::string::npos);
        CHECK(result.error().detail.find("private-marker") == std::string::npos);
    }

    SECTION("invalid adapter error code is replaced with a fixed safe detail")
    {
        fixture.adapter->set_start_error(Error{
            .category = ErrorCategory::Unavailable,
            .code     = "private-marker-code",
            .message  = "private-marker-message",
        });
        auto result = fixture.executor->start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().detail == "core.process.resource_setup_failed");
    }

    SECTION("zero operation identity is rejected and cleaned up")
    {
        fixture.adapter->set_next_operation_id(0);
        auto result = fixture.executor->start(start_request(), callback);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "jobu.cli.start_failed");
        CHECK(result.error().detail == "core.process.invalid_state");
        CHECK(fixture.observation->shutdown == std::vector<ProcessOperationId>{0});
    }

    CHECK(callback_count == 0U);
    CHECK(fixture.adapter->pending_operation_ids().empty());
}

TEST_CASE("CLI attempt executor rejects duplicate attempt and operation identities", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            callbacks = std::size_t{0};

    fixture.adapter->set_next_operation_id(9);
    REQUIRE(fixture.executor->start(start_request(1), [&](AttemptCompletion const&) { ++callbacks; }));

    auto duplicate_key = fixture.executor->start(start_request(1), [&](AttemptCompletion const&) { ++callbacks; });
    REQUIRE_FALSE(duplicate_key);
    CHECK(duplicate_key.error().code == "jobu.cli.duplicate_attempt");

    fixture.adapter->set_next_operation_id(9);
    auto duplicate_operation =
        fixture.executor->start(start_request(2), [&](AttemptCompletion const&) { ++callbacks; });
    REQUIRE_FALSE(duplicate_operation);
    CHECK(duplicate_operation.error().code == "jobu.cli.start_failed");
    CHECK(duplicate_operation.error().detail == "core.process.invalid_state");
    CHECK(callbacks == 0U);
    CHECK(fixture.adapter->pending_operation_ids() == std::vector<ProcessOperationId>{9});
}

TEST_CASE("CLI attempt executor repeats the root check immediately before adapter start", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    fixture.identity->set_sequence({1000U, 0U});

    CHECK(fixture.executor->is_available(JobType::Cli));
    auto result = fixture.executor->start(start_request(), [](AttemptCompletion const&) {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.cli.root_forbidden");
    CHECK(fixture.identity->call_count() == 2U);
    CHECK(fixture.observation->starts.empty());
    CHECK(fixture.adapter->pending_operation_ids().empty());
}

TEST_CASE("CLI attempt executor maps outcomes and capture modes through existing policy", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            completions = std::vector<AttemptCompletion>{};

    auto run = [&](AttemptNumber       attempt_number,
                   AttributeSet const& overrides,
                   ProcessExit         exit,
                   std::string_view    output = "stream-marker") -> AttemptCompletion const& {
        REQUIRE(fixture.executor->start(start_request(attempt_number, overrides), [&](AttemptCompletion completion) {
            completions.push_back(std::move(completion));
        }));
        auto const operation_id = fixture.observation->starts.back().id;
        REQUIRE(fixture.adapter->emit_standard_output(operation_id, as_bytes(output)));
        REQUIRE(fixture.adapter->finish(operation_id, std::move(exit)));
        return completions.back();
    };

    auto const& success = run(1, {}, exited(0));
    CHECK(success.outcome == AttemptOutcome::Succeeded);
    CHECK_FALSE(success.output);
    CHECK(result_string(success, "outcome") == "success");

    auto const& retryable = run(2, {}, exited(2));
    CHECK(retryable.outcome == AttemptOutcome::Failed);
    CHECK(retryable.failure_disposition == FailureDisposition::Retryable);
    REQUIRE(retryable.output);

    auto terminal_overrides = AttributeSet{
        {"cli.retry_exit_codes", {.data = string_list({})}},
    };
    auto const& terminal = run(3, terminal_overrides, exited(2));
    CHECK(terminal.outcome == AttemptOutcome::Failed);
    CHECK(terminal.failure_disposition == FailureDisposition::Terminal);

    auto const& signalled = run(4, {}, {.kind = ProcessExitKind::Signaled, .signal_number = 15});
    CHECK(signalled.outcome == AttemptOutcome::Failed);
    CHECK(signalled.failure_disposition == FailureDisposition::Retryable);

    auto const& timed_out = run(5, {}, {.kind = ProcessExitKind::TimedOut, .signal_number = 9});
    CHECK(timed_out.outcome == AttemptOutcome::Failed);
    CHECK(timed_out.failure_disposition == FailureDisposition::Retryable);

    auto const& cancelled = run(6, {}, {.kind = ProcessExitKind::Cancelled, .exit_code = 0});
    CHECK(cancelled.outcome == AttemptOutcome::Cancelled);
    CHECK_FALSE(cancelled.failure_disposition);

    auto const& start_failed = run(7,
                                   {
    },
                                   {
                                       .kind = ProcessExitKind::StartFailed,
                                       .start_error =
                                           Error{
                                               .category = ErrorCategory::NotFound,
                                               .code     = "core.process.exec_failed",
                                               .message  = "private-marker-message",
                                               .detail   = "private-marker-detail",
                                           },
                                   });
    CHECK(start_failed.outcome == AttemptOutcome::Failed);
    CHECK(start_failed.failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(start_failed, "error_code") == "core.process.exec_failed");
    auto serialized = serialize_json(start_failed.result);
    REQUIRE(serialized);
    CHECK(serialized->find("private-marker") == std::string::npos);
    CHECK(serialized->find("stream-marker") == std::string::npos);

    auto none_overrides = AttributeSet{
        {"output.capture", {.data = std::string{"none"}}},
    };
    auto const& discarded = run(8, none_overrides, exited(2), "discarded-output");
    CHECK_FALSE(discarded.output);
    auto const& stdout_result = discarded.result.as_object().at("stdout").as_object();
    CHECK(stdout_result.at("captured_bytes").as_uint() == 0U);
    CHECK(stdout_result.at("total_bytes").as_uint() == std::string_view{"discarded-output"}.size());
    CHECK(stdout_result.at("truncated").as_bool());

    auto const& lost = run(9, none_overrides, exited(2, true));
    CHECK(lost.result.as_object().at("capture_lost").as_bool());
    CHECK_FALSE(lost.output);

    // A malformed private-adapter observation is an internal defect, but it must still discharge the accepted
    // completion exactly once with a bounded safe result.
    auto const& internal = run(10, {}, ProcessExit{});
    CHECK(internal.outcome == AttemptOutcome::Failed);
    CHECK(internal.failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(internal, "outcome") == "internal_error");
    CHECK(result_string(internal, "error_code") == "jobu.cli.invalid_result");
}

TEST_CASE("CLI attempt executor discards disabled capture while active", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            overrides = AttributeSet{
        {"output.capture",      {.data = std::string{"none"}}},
        {"output.stdout_limit", {.data = std::int64_t{7}}    },
        {"output.stderr_limit", {.data = std::int64_t{5}}    },
    };
    auto       request     = start_request(1, overrides);
    auto const key         = request.key;
    auto       completions = std::vector<AttemptCompletion>{};
    REQUIRE(fixture.executor->start(std::move(request), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    }));
    auto const operation_id = fixture.observation->starts.back().id;

    auto check_discarded = [&] {
        auto sizes = CliAttemptExecutorTestAccess::retained_output_sizes(*fixture.executor, key);
        REQUIRE(sizes);
        CHECK(sizes->stdout_bytes == 0U);
        CHECK(sizes->stderr_bytes == 0U);
        CHECK(completions.empty());
    };
    check_discarded();

    SECTION("multiple binary chunks exceed both configured limits")
    {
        // Observe every delivery while the operation remains active; final JSON alone hides retention.
        for (auto chunk : {
                 std::string_view{"ab"},
                 std::string_view{"012\0"
                                  "456789", 10}
        }) {
            REQUIRE(fixture.adapter->emit_standard_output(operation_id, as_bytes(chunk)));
            check_discarded();
        }
        for (auto chunk : {
                 std::string_view{"xyz"},
                 std::string_view{"ab\0cdefg", 8}
        }) {
            REQUIRE(fixture.adapter->emit_standard_error(operation_id, as_bytes(chunk)));
            check_discarded();
        }

        REQUIRE(fixture.adapter->finish(operation_id, exited(0)));
        REQUIRE(completions.size() == 1U);
        auto const& result = completions.front().result.as_object();
        CHECK(result.at("stdout").as_object().at("total_bytes").as_uint() == 12U);
        CHECK(result.at("stderr").as_object().at("total_bytes").as_uint() == 11U);
        CHECK(result.at("stdout").as_object().at("truncated").as_bool());
        CHECK(result.at("stderr").as_object().at("truncated").as_bool());
    }

    SECTION("empty streams retain zero counts without truncation")
    {
        REQUIRE(fixture.adapter->finish(operation_id, exited(0)));
        REQUIRE(completions.size() == 1U);
        for (auto const* channel : {"stdout", "stderr"}) {
            auto const& result = completions.front().result.as_object().at(channel).as_object();
            CHECK(result.at("total_bytes").as_uint() == 0U);
            CHECK_FALSE(result.at("truncated").as_bool());
        }
    }

    REQUIRE(completions.size() == 1U);
    CHECK_FALSE(completions.front().output);
    CHECK_FALSE(completions.front().result.as_object().at("capture_lost").as_bool());
    for (auto const* channel : {"stdout", "stderr"}) {
        CHECK(completions.front().result.as_object().at(channel).as_object().at("captured_bytes").as_uint() == 0U);
    }
    CHECK_FALSE(CliAttemptExecutorTestAccess::retained_output_sizes(*fixture.executor, key));
}

TEST_CASE("CLI attempt executor observes bounded enabled capture while active", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            request     = start_request(1,
                                                {
                                                    {"output.capture",      {.data = std::string{"always"}}},
                                                    {"output.stdout_limit", {.data = std::int64_t{7}}      },
                                                    {"output.stderr_limit", {.data = std::int64_t{5}}      },
    });
    auto const      key         = request.key;
    auto            completions = std::vector<AttemptCompletion>{};
    REQUIRE(fixture.executor->start(std::move(request), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    }));
    auto const operation_id = fixture.observation->starts.back().id;
    REQUIRE(fixture.adapter->emit_standard_output(operation_id, as_bytes("0123456789")));
    REQUIRE(fixture.adapter->emit_standard_error(operation_id, as_bytes("abcdefgh")));

    // Positive retention proves the observer reads the live buffers and other modes still honor their limits.
    auto sizes = CliAttemptExecutorTestAccess::retained_output_sizes(*fixture.executor, key);
    REQUIRE(sizes);
    CHECK(sizes->stdout_bytes == 7U);
    CHECK(sizes->stderr_bytes == 5U);
    CHECK(completions.empty());

    REQUIRE(fixture.adapter->finish(operation_id, exited(0)));
    REQUIRE(completions.size() == 1U);
    REQUIRE(completions.front().output);
    CHECK_FALSE(CliAttemptExecutorTestAccess::retained_output_sizes(*fixture.executor, key));
}

TEST_CASE("CLI attempt executor retains completion through cancellation", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            completions = std::vector<AttemptCompletion>{};
    auto const      key         = start_request().key;

    auto missing = fixture.executor->cancel(key);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "jobu.cli.attempt_not_found");

    REQUIRE(fixture.executor->start(start_request(), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
    }));
    auto const operation_id = fixture.observation->starts.front().id;

    fixture.adapter->set_stop_error(Error{
        .category = ErrorCategory::Io,
        .code     = "core.process.signal_failed",
        .message  = "private-marker-message",
        .detail   = "private-marker-detail",
    });
    auto rejected = fixture.executor->cancel(key);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobu.cli.cancel_failed");
    CHECK(rejected.error().detail == "core.process.signal_failed");
    CHECK(completions.empty());

    fixture.adapter->set_stop_error(std::nullopt);
    REQUIRE(fixture.executor->cancel(key));
    REQUIRE(fixture.executor->cancel(key));
    CHECK(completions.empty());
    REQUIRE(fixture.observation->stops.size() == 3U);
    CHECK(std::ranges::all_of(fixture.observation->stops,
                              [](auto const& stop) { return stop.reason == ProcessStopReason::Cancelled; }));

    REQUIRE(fixture.adapter->finish(operation_id, {.kind = ProcessExitKind::Cancelled, .signal_number = 15}));
    REQUIRE(completions.size() == 1U);
    CHECK(completions[0].outcome == AttemptOutcome::Cancelled);
}

TEST_CASE("CLI attempt executor fences stale events and retires state before callbacks", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            overrides = AttributeSet{
        {"output.capture", {.data = std::string{"always"}}},
    };
    auto completions      = std::vector<AttemptCompletion>{};
    auto reentrant_cancel = std::optional<Result<void, Error>>{};
    auto reentrant_start  = std::optional<Result<void, Error>>{};

    REQUIRE(fixture.executor->start(start_request(1, overrides), [&](AttemptCompletion completion) {
        completions.push_back(std::move(completion));
        reentrant_cancel.emplace(fixture.executor->cancel(start_request().key));
        reentrant_start.emplace(fixture.executor->start(start_request(1, overrides), [&](AttemptCompletion second) {
            completions.push_back(std::move(second));
        }));
    }));
    auto const first_id = fixture.observation->starts.front().id;
    auto       stale    = fixture.adapter->snapshot_sink(first_id);
    REQUIRE(stale);

    REQUIRE(fixture.adapter->emit_standard_output(first_id, as_bytes("wrong"), first_id + 100U));
    REQUIRE(fixture.adapter->finish(first_id, exited(0), first_id + 100U));
    CHECK(completions.empty());

    REQUIRE(fixture.adapter->emit_standard_output(first_id, as_bytes("first")));
    REQUIRE(fixture.adapter->finish(first_id, exited(0)));
    REQUIRE(completions.size() == 1U);
    REQUIRE(reentrant_cancel);
    REQUIRE_FALSE(*reentrant_cancel);
    CHECK(reentrant_cancel->error().code == "jobu.cli.attempt_not_found");
    REQUIRE(reentrant_start);
    REQUIRE(*reentrant_start);
    REQUIRE(fixture.observation->starts.size() == 2U);
    auto const second_id = fixture.observation->starts.back().id;

    stale->standard_output(first_id, as_bytes("stale"));
    stale->finished(first_id, exited(0));
    CHECK(completions.size() == 1U);

    REQUIRE(fixture.adapter->emit_standard_output(second_id, as_bytes("second")));
    REQUIRE(fixture.adapter->finish(second_id, exited(0)));
    REQUIRE(completions.size() == 2U);
    REQUIRE(completions[1].output);
    REQUIRE(completions[1].output->primary);
    CHECK(byte_text(completions[1].output->primary->bytes) == "second");
}

TEST_CASE("CLI attempt executor destruction cleans active operations and suppresses callbacks", "[jobu][cli][executor]")
{
    ExecutorFixture fixture;
    auto            callback_count = std::size_t{0};
    REQUIRE(fixture.executor->start(start_request(1), [&](AttemptCompletion const&) { ++callback_count; }));
    REQUIRE(fixture.executor->start(start_request(2), [&](AttemptCompletion const&) { ++callback_count; }));

    auto const first_id  = fixture.observation->starts[0].id;
    auto const second_id = fixture.observation->starts[1].id;
    auto       stale     = fixture.adapter->snapshot_sink(first_id);
    REQUIRE(stale);

    fixture.executor.reset();
    CHECK(callback_count == 0U);
    auto shutdown = fixture.observation->shutdown;
    std::ranges::sort(shutdown);
    CHECK(shutdown == std::vector<ProcessOperationId>{first_id, second_id});

    stale->standard_output(first_id, as_bytes("late-output"));
    stale->finished(first_id, exited(0));
    CHECK(callback_count == 0U);
}

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE("CLI production adapter executes explicit Process requests", "[jobu][cli][executor][posix]")
{
    RealExecutorFixture fixture;
    TemporaryDirectory  directory;

    // getcwd() reports the physical path even when the requested temporary directory traverses a macOS symlink.
    auto const physical_directory = std::filesystem::canonical(directory.path());

    for (AttemptNumber attempt_number : {AttemptNumber{1}, AttemptNumber{2}}) {
        auto const attempt_text = std::to_string(attempt_number);
        auto       payload      = RealCliPayload{};
        payload.arguments       = {
            "inspect-jobu",
            "",
            "-option",
            physical_directory.string(),
            job_id().to_string(),
            run_id().to_string(),
            attempt_text,
        };
        payload.working_directory = directory.path();
        payload.environment.emplace("PROCESS_MARKER", json_string("literal $x = value"));

        auto completion = fixture.execute(real_start_request(attempt_number, std::move(payload)));
        CHECK(completion.outcome == AttemptOutcome::Succeeded);
        CHECK(completion.key == AttemptKey{.run_id = run_id(), .attempt_number = attempt_number});
    }

    auto const helper_path = std::filesystem::path{PROCESS_TEST_HELPER};
    auto       path_lookup = RealCliPayload{};
    path_lookup.command    = helper_path.filename().string();
    path_lookup.arguments  = {"exit", "0"};
    path_lookup.environment.emplace("PATH", json_string(helper_path.parent_path().string()));
    CHECK(fixture.execute(real_start_request(3, std::move(path_lookup))).outcome == AttemptOutcome::Succeeded);

#  if defined(__linux__)
    auto capture_output = AttributeSet{
        {"output.capture",      {.data = std::string{"always"}}},
        {"output.stdout_limit", {.data = std::int64_t{64}}     },
    };
    auto no_new_privileges = fixture.execute(helper_start_request(4, {"no-new-privileges"}, capture_output));
    REQUIRE(no_new_privileges.output);
    REQUIRE(no_new_privileges.output->primary);
    CHECK(byte_text(no_new_privileges.output->primary->bytes) == "NoNewPrivs: 1\n");
#  endif
}

TEST_CASE("CLI production adapter enforces the child-side non-root policy", "[jobu][cli][executor][posix]")
{
    TestApplication    application;
    TemporaryDirectory directory;
    auto               process_operations = std::make_shared<RootChildIdentityOperations>();
    auto               parent_identity    = std::make_unique<FakeEffectiveIdentityProbe>(1000U);
    auto executor = CliAttemptExecutorTestAccess::create_with_system_process_adapter({},
                                                                                     std::move(parent_identity),
                                                                                     std::move(process_operations));

    REQUIRE(executor->is_available(JobType::Cli));

    auto marker         = directory.path() / "target-executed";
    auto completion     = std::optional<AttemptCompletion>{};
    auto callback_count = std::size_t{0};
    auto request        = helper_start_request(1, {"marker", marker.string()}, {}, {37U});

    auto accepted = executor->start(std::move(request), [&](AttemptCompletion result) {
        ++callback_count;
        completion = std::move(result);
    });
    if (!accepted) {
        UNSCOPED_INFO("start error: " << accepted.error().code << " " << accepted.error().detail);
    }
    REQUIRE(accepted);
    CHECK_FALSE(completion);

    auto const deadline = Clock::now() + 5s;
    while (!completion && Clock::now() < deadline) {
        REQUIRE(application.application.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
    }

    REQUIRE(completion);
    CHECK(callback_count == 1U);
    CHECK(completion->outcome == AttemptOutcome::Failed);
    CHECK(completion->failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(*completion, "outcome") == "start_failure");
    CHECK(result_string(*completion, "error_code") == "core.process.security_failed");
    CHECK_FALSE(std::filesystem::exists(marker));

    auto const cleanup_deadline = Clock::now() + 5s;
    while (!executor->children().empty() && Clock::now() < cleanup_deadline) {
        REQUIRE(application.application.process_events(EventFlag::All, 10) != ProcessEventsResult::Failed);
    }
    REQUIRE(executor->children().empty());
    REQUIRE(application.application.process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
    CHECK(callback_count == 1U);
}

TEST_CASE("CLI production adapter maps real Process terminal outcomes safely", "[jobu][cli][executor][posix]")
{
    RealExecutorFixture fixture;

    auto success = fixture.execute(helper_start_request(1, {"exit", "0"}));
    CHECK(success.outcome == AttemptOutcome::Succeeded);

    auto expected_nonzero = fixture.execute(helper_start_request(2, {"exit", "7"}, AttributeSet{}, {7U}));
    CHECK(expected_nonzero.outcome == AttemptOutcome::Succeeded);
    CHECK(result_string(expected_nonzero, "outcome") == "success");

    auto retryable = fixture.execute(helper_start_request(3, {"exit", "2"}));
    CHECK(retryable.outcome == AttemptOutcome::Failed);
    CHECK(retryable.failure_disposition == FailureDisposition::Retryable);
    CHECK(result_string(retryable, "outcome") == "unexpected_exit");

    auto terminal_attributes = AttributeSet{
        {"cli.retry_exit_codes", {.data = string_list({})}},
    };
    auto terminal = fixture.execute(helper_start_request(4, {"exit", "2"}, terminal_attributes));
    CHECK(terminal.outcome == AttemptOutcome::Failed);
    CHECK(terminal.failure_disposition == FailureDisposition::Terminal);

    auto signalled = fixture.execute(helper_start_request(5, {"signal", std::to_string(SIGTERM)}));
    CHECK(signalled.outcome == AttemptOutcome::Failed);
    CHECK(signalled.failure_disposition == FailureDisposition::Retryable);
    CHECK(result_string(signalled, "outcome") == "signal");

    constexpr std::string_view sensitive_marker{"private-marker-command"};
    auto                       start_failure_payload = RealCliPayload{};
    start_failure_payload.command                    = "/private-marker-command-does-not-exist";
    start_failure_payload.environment.emplace("PRIVATE_MARKER_ENV", json_string("private-marker-value"));
    auto start_failure = fixture.execute(real_start_request(6, std::move(start_failure_payload)));
    CHECK(start_failure.outcome == AttemptOutcome::Failed);
    CHECK(start_failure.failure_disposition == FailureDisposition::Terminal);
    CHECK(result_string(start_failure, "outcome") == "start_failure");
    CHECK(result_string(start_failure, "error_code") == "core.process.exec_failed");
    auto serialized = serialize_json(start_failure.result);
    REQUIRE(serialized);
    CHECK(serialized->find(sensitive_marker) == std::string::npos);
    CHECK(serialized->find("private-marker-value") == std::string::npos);

    auto timeout_attributes = AttributeSet{
        {"cli.termination_grace", {.data = Duration::zero()}},
        {"job.timeout",           {.data = 100ms}           },
    };
    auto timed_out = fixture.execute(helper_start_request(7, {"term", "1"}, timeout_attributes));
    CHECK(timed_out.outcome == AttemptOutcome::Failed);
    CHECK(timed_out.failure_disposition == FailureDisposition::Retryable);
    CHECK(result_string(timed_out, "outcome") == "timeout");
}

TEST_CASE("CLI production adapter preserves real output capture policy", "[jobu][cli][executor][posix]")
{
    RealExecutorFixture fixture;

    auto always_attributes = AttributeSet{
        {"output.capture",      {.data = std::string{"always"}}},
        {"output.stderr_limit", {.data = std::int64_t{6}}      },
        {"output.stdout_limit", {.data = std::int64_t{7}}      },
    };
    auto captured = fixture.execute(helper_start_request(1, {"output", "12", "11"}, always_attributes, {37U}));
    REQUIRE(captured.output);
    REQUIRE(captured.output->primary);
    REQUIRE(captured.output->diagnostic);
    CHECK(captured.output->primary->bytes == captured_pattern(12U, 7U, 0U));
    CHECK(captured.output->primary->total_bytes == 12U);
    CHECK(captured.output->primary->truncated);
    CHECK(captured.output->diagnostic->bytes == captured_pattern(11U, 6U, 1U));
    CHECK(captured.output->diagnostic->total_bytes == 11U);
    CHECK(captured.output->diagnostic->truncated);

    auto none_attributes = AttributeSet{
        {"output.capture", {.data = std::string{"none"}}},
    };
    auto discarded = fixture.execute(helper_start_request(2, {"output", "5", "4"}, none_attributes));
    CHECK_FALSE(discarded.output);
    auto const& discarded_stdout = discarded.result.as_object().at("stdout").as_object();
    auto const& discarded_stderr = discarded.result.as_object().at("stderr").as_object();
    CHECK(discarded_stdout.at("captured_bytes").as_uint() == 0U);
    CHECK(discarded_stdout.at("total_bytes").as_uint() == 5U);
    CHECK(discarded_stdout.at("truncated").as_bool());
    CHECK(discarded_stderr.at("captured_bytes").as_uint() == 0U);
    CHECK(discarded_stderr.at("total_bytes").as_uint() == 4U);
    CHECK(discarded_stderr.at("truncated").as_bool());

    auto on_error_attributes = AttributeSet{
        {"output.capture",      {.data = std::string{"on_error"}}},
        {"output.stderr_limit", {.data = std::int64_t{8}}        },
        {"output.stdout_limit", {.data = std::int64_t{8}}        },
    };
    auto on_error_success = fixture.execute(helper_start_request(3, {"output", "5", "4"}, on_error_attributes, {37U}));
    CHECK(on_error_success.outcome == AttemptOutcome::Succeeded);
    CHECK_FALSE(on_error_success.output);

    auto on_error_failure = fixture.execute(helper_start_request(4, {"output", "5", "4"}, on_error_attributes));
    CHECK(on_error_failure.outcome == AttemptOutcome::Failed);
    REQUIRE(on_error_failure.output);
    REQUIRE(on_error_failure.output->primary);
    REQUIRE(on_error_failure.output->diagnostic);
    CHECK(on_error_failure.output->primary->bytes == captured_pattern(5U, 8U, 0U));
    CHECK(on_error_failure.output->diagnostic->bytes == captured_pattern(4U, 8U, 1U));
}

TEST_CASE("CLI production adapter cancels and destroys complete Process groups", "[jobu][cli][executor][posix]")
{
    SECTION("cancellation retains completion until leader and descendant terminate")
    {
        RealExecutorFixture fixture;
        PostExecReport      report;
        auto                attributes = AttributeSet{
            {"cli.termination_grace", {.data = Duration::zero()}},
            {"job.timeout",           {.data = 30s}             },
        };
        auto completion     = std::optional<AttemptCompletion>{};
        auto callback_count = std::size_t{0};
        auto request        = helper_start_request(1, {"group-wait", report.path()}, attributes);

        REQUIRE(fixture.executor.start(std::move(request), [&](AttemptCompletion result) {
            ++callback_count;
            completion = std::move(result);
        }));
        CHECK(callback_count == 0U);

        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch leader{identities[0]};
        ProcessTerminationWatch descendant{identities[1]};
        REQUIRE(fixture.executor.children().size() == 1U);

        REQUIRE(fixture.executor.cancel(start_request().key));
        CHECK(callback_count == 0U);
        fixture.until([&] { return completion.has_value(); });
        CHECK(callback_count == 1U);
        CHECK(completion->outcome == AttemptOutcome::Cancelled);
        CHECK(result_string(*completion, "outcome") == "cancelled");
        fixture.until([&] { return fixture.executor.children().empty(); });
        leader.check_terminated();
        descendant.check_terminated();
    }

    SECTION("executor destruction immediately kills descendants and suppresses completion")
    {
        TestApplication application;
        PostExecReport  report;
        auto            executor = std::make_unique<CliAttemptExecutor>(CliAttemptExecutorOptions{.allow_root = true});
        auto            attributes = AttributeSet{
            {"cli.termination_grace", {.data = 30s}},
            {"job.timeout",           {.data = 30s}},
        };
        auto callback_count = std::size_t{0};
        auto request        = helper_start_request(1, {"group-wait", report.path()}, attributes);

        REQUIRE(executor->start(std::move(request), [&](AttemptCompletion const&) { ++callback_count; }));
        std::array<pid_t, 2> identities{};
        report.read(identities.data(), sizeof(identities));
        ProcessTerminationWatch leader{identities[0]};
        ProcessTerminationWatch descendant{identities[1]};
        REQUIRE(executor->children().size() == 1U);

        executor.reset();
        CHECK(callback_count == 0U);
        leader.check_terminated();
        descendant.check_terminated();
        REQUIRE(application.application.process_events(EventFlag::All, 0) != ProcessEventsResult::Failed);
        CHECK(callback_count == 0U);
    }
}
#endif
