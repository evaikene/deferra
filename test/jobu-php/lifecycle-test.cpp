#include "attribute_binding.hpp"
#include "cli_payload_binding.hpp"
#include "client.hpp"
#include "history-test.hpp"
#include "http_payload_binding.hpp"
#include "lifecycle-bindings.hpp"
#include "list-test.hpp"
#include "operation.hpp"
#include "page-bindings.hpp"
#include "query_binding.hpp"
#include "queue-test.hpp"
#include "recovery-proxy.hpp"
#include "request.hpp"
#include "resource-bindings.hpp"
#include "result_binding.hpp"
#include "support/temporary_directory.hpp"
#include "value_binding.hpp"

#include <jb/core/byte_buffer.hpp>
#include <jb/core/event_loop.hpp>
#include <jb/core/event_loop_types.hpp>
#include <jb/core/json.hpp>
#include <jb/core/object.hpp>
#include <jb/core/process.hpp>
#include <jb/core/scoped_event_loop.hpp>
#include <jb/core/signal.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/history_json.hpp>
#include <jb/jobu/management_json.hpp>
#include <jb/jobu/secret_json.hpp>
#include <jb/jobu/statistics_json.hpp>
#include <jb/jobu/utc_timestamp.hpp>
#include <jb/net/local_server.hpp>
#include <jb/net/local_socket.hpp>
#include <jb/rpc/protocol.hpp>
#include <jb/rpc/server.hpp>

#include <Zend/zend_exceptions.h>
#include <phpcxx/conversion.hpp>
#include <phpcxx/error.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>
#include <sapi/embed/php_embed.h>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include <chrono> // IWYU pragma: keep duration literals
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

extern "C" auto get_module() -> zend_module_entry*;

namespace {
using namespace std::chrono_literals;
using jb::core::Clock;
using jb::core::EventLoop;

// Embed replaces additional_functions. As in the helper lifecycle fixture,
// register through SAPI startup and reconstruct Zend's mutable module entry
// for each runtime. Actual production RINIT/RSHUTDOWN callbacks stay unchanged.
auto start_module(int type, int number) noexcept -> zend_result
{
    if (get_module()->module_startup_func(type, number) != SUCCESS) {
        return FAILURE;
    }
    return jobu_php_test::register_classes();
}

auto start_sapi(sapi_module_struct* sapi) noexcept -> int
{
    static zend_module_entry module;
    module                     = *get_module();
    module.module_startup_func = start_module;
    return php_module_startup(sapi, &module);
}

class Runtime {
public:
    Runtime()
        : _saved(php_embed_module)
    {
        static char  program[]          = "jobu-php-lifecycle";
        static char* arguments[]        = {program, nullptr};
        php_embed_module.php_ini_ignore = 1;
        php_embed_module.startup        = start_sapi;
        if (php_embed_init(1, arguments) == FAILURE) {
            std::fputs("PHP Embed initialization failed\n", stderr);
            std::exit(EXIT_FAILURE);
        }
    }

    ~Runtime()
    {
        php_embed_shutdown();
        php_embed_module = _saved;
    }

    Runtime(Runtime const&)                    = delete;
    auto operator=(Runtime const&) -> Runtime& = delete;

    auto restart(EventLoop* previous) -> void
    {
        php_request_shutdown(nullptr);
        CHECK(jb::php::current_request() == nullptr);
        CHECK(EventLoop::current() == previous);
        CHECK(jobu_php_test::live_clients() == 0);
        if (php_request_startup() == FAILURE) {
            std::fputs("PHP request restart failed\n", stderr);
            std::exit(EXIT_FAILURE);
        }
        SG(headers_sent)            = 1;
        SG(request_info).no_headers = true;
    }

private:
    sapi_module_struct _saved;
};

auto evaluate(std::string_view expression) -> bool
{
    zval result{};
    ZVAL_UNDEF(&result);
    auto status    = zend_eval_stringl(expression.data(), expression.size(), &result, "jobu lifecycle test");
    auto owner     = php::Value::adopt(result);
    bool exception = php::has_exception();
    if (exception) {
        zend_exception_error(EG(exception), E_WARNING);
        zend_clear_exception();
    }
    return status == SUCCESS && !exception && owner.view().is_true();
}

// The daemon lives on a dormant host loop while PHP attaches each request loop.
// Its Process owner reaps the child even if a test assertion interrupts the case.
class Daemon {
public:
    auto start(EventLoop& loop, std::filesystem::path const& directory, bool execute_cli = false) -> void
    {
        _process.standard_error.connect(&_receiver, [this](jb::core::ByteBuffer const& bytes) {
            _log.append(jb::core::as_string_view(bytes));
        });
        _process.finished.connect(&_receiver, [this](jb::core::ProcessExit const& value) { _exit = value; });
        std::vector<std::string> arguments{"--no-config",
                                           "--database",
                                           (directory / "daemon.sqlite").string(),
                                           "--socket",
                                           (directory / "daemon.sock").string()};
        if (::geteuid() == 0) {
            arguments.emplace_back("--allow-root-daemon");
            if (execute_cli) {
                // Root daemon startup does not authorize child execution. Only
                // explicitly opted-in, disposable test environments may do both.
                auto const* enabled = std::getenv("JOBU_TEST_ALLOW_ROOT_CLI");
                INFO("Root CLI execution requires JOBU_TEST_ALLOW_ROOT_CLI=1 in an isolated test environment");
                REQUIRE((enabled && std::string_view{enabled} == "1"));
                arguments.emplace_back("--allow-root-cli");
            }
        }
        REQUIRE(_process.start({.executable = JOBUD_EXECUTABLE, .arguments = std::move(arguments)}));
        REQUIRE(loop.run_until(Clock::now() + 8s,
                               [this] { return _exit || _log.find("jobud.ready") != std::string::npos; }));
        INFO(_log);
        REQUIRE_FALSE(_exit);
    }

    auto stop(EventLoop& loop) -> void
    {
        REQUIRE(_process.process_id());
        REQUIRE(::kill(static_cast<pid_t>(*_process.process_id()), SIGTERM) == 0);
        REQUIRE(loop.run_until(Clock::now() + 8s, [this] { return _exit.has_value(); }));
        INFO(_log);
        CHECK(_exit->kind == jb::core::ProcessExitKind::Exited);
        CHECK(_exit->exit_code == 0);
    }

private:
    std::string                          _log;
    std::optional<jb::core::ProcessExit> _exit;
    jb::core::Object                     _receiver;
    jb::core::Process                    _process;
};

// Linux exposes descriptor accounting without reaching into socket internals.
// Other platforms still exercise native lifetime and thread-binding assertions.
auto descriptor_count() -> std::size_t
{
#ifdef __linux__
    std::size_t count = 0;
    for ([[maybe_unused]] auto const& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        ++count;
    }
    return count;
#else
    return 0;
#endif
}

} // namespace

TEST_CASE("PHP module isolates repeated requests and independent connected sessions", "[php][lifecycle]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto*                        previous = &host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(*previous, directory.path());
    auto const descriptors = descriptor_count();

    for (int runtime_index = 0; runtime_index < 3; ++runtime_index) {
        {
            Runtime runtime;
            for (int request_index = 0; request_index < 5; ++request_index) {
                INFO(runtime_index);
                INFO(request_index);
                REQUIRE(jb::php::current_request());
                CHECK(jb::php::current_request()->loop() == nullptr);
                CHECK(EventLoop::current() == previous);
                auto endpoint = (directory.path() / "daemon.sock").string();
                zend_register_string_constant("JOBU_TEST_SOCKET",
                                              sizeof("JOBU_TEST_SOCKET") - 1,
                                              endpoint.c_str(),
                                              CONST_CS,
                                              0);
                REQUIRE(evaluate("(function () { return include '" JOBU_PHP_LIFECYCLE_SCRIPT "'; })()"));
                CHECK(jb::php::current_request()->loop() != previous);
                CHECK(jobu_php_test::live_clients() == 1);
                runtime.restart(previous);
            }
        }
        CHECK(jb::php::current_request() == nullptr);
        CHECK(EventLoop::current() == previous);
        CHECK(jobu_php_test::live_clients() == 0);
        CHECK(descriptor_count() == descriptors);
    }
    daemon.stop(*previous);
}

TEST_CASE("Request shutdown detaches late native wrappers and discards pending work", "[php][lifecycle]")
{
    CHECK(EventLoop::current() == nullptr);
    std::unique_ptr<jb::php::Session> survivor;
    bool                              invoked = false;
    auto                              capture = std::make_shared<int>(42);
    std::weak_ptr<int>                pending = capture;
    {
        Runtime runtime;
        auto*   request = jb::php::current_request();
        REQUIRE(request);
        auto first  = request->create_session();
        auto second = request->create_session();
        REQUIRE(first);
        REQUIRE(second);
        CHECK(first.value()->socket().event_loop() == second.value()->socket().event_loop());
        CHECK(&first.value()->socket() != &second.value()->socket());
        CHECK_FALSE(first.value()->attach_clients());
        survivor = std::move(first).value();
        second.value()->close();
        second.value()->close();
        REQUIRE(request->loop()->post([capture, &invoked] { invoked = true; }));
        capture.reset();
        survivor->receiver().delete_later();
        runtime.restart(nullptr);
        CHECK(survivor->is_closed());
        CHECK_FALSE(survivor->attach_clients());
        CHECK_FALSE(invoked);
        CHECK(pending.expired());
        survivor->close();
        survivor.reset();
    }
    CHECK(EventLoop::current() == nullptr);
}

TEST_CASE("Request attachment and waits preserve the core reentrancy boundary", "[php][lifecycle]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto&   loop = host.value()->loop();
    Runtime runtime;
    bool    rejected = false;
    REQUIRE(loop.post([&] {
        auto session = jb::php::current_request()->create_session();
        rejected     = !session && session.error().code == "core.event_loop.reentrant_wait";
    }));
    REQUIRE(loop.run_until(Clock::now() + 1s, [&] { return rejected; }));
    CHECK(jb::php::current_request()->loop() == nullptr);
    CHECK(EventLoop::current() == &loop);

    auto session = jb::php::current_request()->create_session();
    REQUIRE(session);
    auto& request_loop = *jb::php::current_request()->loop();
    {
        auto nested_host = jb::core::ScopedEventLoop::create();
        REQUIRE(nested_host);
        auto misplaced = jb::php::current_request()->create_session();
        REQUIRE_FALSE(misplaced);
        CHECK(misplaced.error().code == "php.request.invalid_context");
    }
    CHECK(EventLoop::current() == &request_loop);

    rejected = false;
    REQUIRE(request_loop.post([&] {
        auto nested = request_loop.run_until(Clock::now() + 1s, [] { return true; });
        rejected    = !nested && nested.error().code == "core.event_loop.reentrant_wait";
    }));
    REQUIRE(request_loop.run_until(Clock::now() + 1s, [&] { return rejected; }));
}

TEST_CASE("Production constructor translates terminal and timed-out handshakes", "[php][bridge]")
{
    for (auto const* scenario : {"timeout", "malformed", "remote", "incompatible"}) {
        INFO(scenario);
        Runtime runtime;
        auto*   request = jb::php::current_request();
        auto    anchor  = request->create_session();
        REQUIRE(anchor);
        jb::test::TemporaryDirectory                       directory{std::filesystem::perms::owner_all};
        jb::net::LocalServer                               listener;
        jb::rpc::Server                                    server;
        jb::core::Object                                   receiver;
        std::vector<std::unique_ptr<jb::net::LocalSocket>> silent;
        REQUIRE(server.register_method("system.info", [&](auto const&, auto const&) {
            if (std::string_view{scenario} == "remote") {
                return jb::rpc::MethodResult::failure({.code = -32000, .message = "PRIVATE PEER DETAIL"});
            }
            if (std::string_view{scenario} == "incompatible") {
                auto json = jb::core::parse_json(
                    R"({"daemon_version":"test","api_version":{"major":2,"minor":7},"capabilities":[]})");
                REQUIRE(json);
                return jb::rpc::MethodResult::success(std::move(json).value());
            }
            return jb::rpc::MethodResult::success(jb::core::JsonValue{});
        }));
        listener.new_connection.connect(&receiver, [&] {
            while (auto socket = listener.take_next_connection()) {
                if (std::string_view{scenario} == "timeout") {
                    silent.push_back(std::move(socket));
                }
                else {
                    REQUIRE(server.add_connection(std::move(socket)));
                }
            }
        });
        auto endpoint = (directory.path() / "peer.sock").string();
        REQUIRE(listener.listen(endpoint));
        zend_register_string_constant("JOBU_TEST_SOCKET",
                                      sizeof("JOBU_TEST_SOCKET") - 1,
                                      endpoint.c_str(),
                                      CONST_CS,
                                      0);
        auto const* expected = "ConnectionException";
        if (std::string_view{scenario} == "timeout") {
            expected = "TimeoutException";
        }
        else if (std::string_view{scenario} == "incompatible") {
            expected = "UnsupportedFeatureException";
        }
        auto const milliseconds = std::string_view{scenario} == "timeout" ? 20 : 1000;
        auto       expression =
            fmt::format("(function () {{ try {{ new JobU\\Client(JOBU_TEST_SOCKET, JobU\\Duration::milliseconds({})); "
                        "return false; }} catch (JobU\\{} $e) {{ return $e->getErrorCode() !== null && "
                        "$e->getCategory() !== null && !str_contains($e->getMessage(), 'PRIVATE'); }} }})()",
                        milliseconds,
                        expected);
        REQUIRE(evaluate(expression));
    }
}

TEST_CASE("Production CLI module connects to the same-build daemon", "[php][bridge]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto&                        loop = host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(loop, directory.path());

    jb::core::Process                    process;
    jb::core::Object                     receiver;
    std::optional<jb::core::ProcessExit> exit;
    std::string                          errors;
    process.standard_error.connect(&receiver, [&](jb::core::ByteBuffer const& bytes) {
        errors.append(jb::core::as_string_view(bytes));
    });
    process.finished.connect(&receiver, [&](jb::core::ProcessExit const& value) { exit = value; });
    REQUIRE(process.start({
        .executable = JOBU_PHP_BINARY,
        .arguments  = {"-n",
                       "-dextension=" JOBU_PHP_MODULE,
                       JOBU_PHP_CLIENT_SCRIPT, (directory.path() / "daemon.sock").string()}
    }));
    REQUIRE(loop.run_until(Clock::now() + 10s, [&] { return exit.has_value(); }));
    INFO(errors);
    CHECK(errors.empty());
    CHECK(exit->kind == jb::core::ProcessExitKind::Exited);
    CHECK(exit->exit_code == 0);
    daemon.stop(loop);
}

TEST_CASE("Production native Client safely outlives request shutdown", "[php][lifecycle]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto&                        loop = host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(loop, directory.path());
    auto client = std::make_unique<jb::php::Client>();
    {
        Runtime runtime;
        REQUIRE(client->initialize(*jb::php::current_request(),
                                   (directory.path() / "daemon.sock").string(),
                                   Clock::now() + 1s,
                                   10s));
        // Leave an accepted call pending. RSHUTDOWN must retire it and detach
        // the bundle without pumping callbacks into this surviving wrapper.
        REQUIRE(client->typed()->get_system_info());
    }
    CHECK(EventLoop::current() == &loop);
    CHECK(client->typed() == nullptr);
    client->close();
    client.reset();
    daemon.stop(loop);
}

TEST_CASE("Recovery preserves committed effects and frozen requests through lost replies", "[php][recovery]")
{
    using jb::jobu::ControlClient;
    using jb::php::Operation;
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(host.value()->loop(), directory.path());
    {
        jb::php::Request request;
        auto             anchor = request.create_session();
        REQUIRE(anchor);
        auto                         endpoint = (directory.path() / "proxy.sock").string();
        jobu_php_test::RecoveryProxy proxy(endpoint, (directory.path() / "daemon.sock").string());
        jb::php::Client              client;
        REQUIRE(client.initialize(request, endpoint, Clock::now() + 2s, 2s));
        auto call       = [&](Operation const& operation) { return client.call(Clock::now() + 2s, operation); };
        auto make_queue = Operation::bind("queue.create",
                                          &ControlClient::create_queue,
                                          jb::jobu::CreateQueueRequest{.name = "recovery"});
        REQUIRE(make_queue);
        proxy.drop_method = "queue.create";
        proxy.drops       = 1;
        auto created      = call(make_queue.value());
        REQUIRE(created);
        auto queue = std::get<jb::jobu::Queue>(created.value());
        CHECK(proxy.requests["queue.create"].size() == 2);
        CHECK(proxy.requests["queue.create"][0] == proxy.requests["queue.create"][1]);
        CHECK(proxy.dropped_results[0].as_object().at("id").as_string() == queue.id.to_string());
        CHECK(make_queue.value().key()->size() == 72);

        SECTION("lost save reply never repeats the update and next read reconnects")
        {
            jb::jobu::UpdateQueueRequest edits{.queue = queue.id, .expected_revision = queue.revision, .weight = 7};
            auto                         save = Operation::bind("queue.update", &ControlClient::update_queue, edits);
            REQUIRE(save);
            proxy.drop_method = "queue.update";
            proxy.drops       = 1;
            auto saved        = call(save.value());
            REQUIRE_FALSE(saved);
            CHECK(saved.error().outcome_unknown);
            CHECK(proxy.requests["queue.update"].size() == 1);
            CHECK(edits.weight == 7);
            CHECK(edits.expected_revision == queue.revision);
            CHECK(client.state() == jb::php::ConnectionState::Disconnected);

            auto get =
                Operation::bind("queue.get",
                                static_cast<Operation::Accepted (ControlClient::*)(jb::jobu::GetQueueRequest const&,
                                                                                   jb::jobu::ControlCallOptions)>(
                                    &ControlClient::get_queue),
                                jb::jobu::GetQueueRequest{.queue = queue.id});
            REQUIRE(get);
            auto loaded = call(get.value());
            REQUIRE(loaded);
            CHECK(std::get<jb::jobu::Queue>(loaded.value()).weight == 7);
            CHECK(std::get<jb::jobu::Queue>(loaded.value()).revision == queue.revision + 1);
            CHECK(proxy.requests["queue.update"].size() == 1);
        }
        SECTION("post-commit response error is unknown and is not replayed")
        {
            auto error = jb::core::parse_json(
                R"({"code":-32000,"message":"PRIVATE","data":{"code":"jobu.response.too_large","category":"resource_exhausted"}})");
            REQUIRE(error);
            proxy.error_method   = "queue.update";
            proxy.response_error = error.value();
            auto save            = Operation::bind(
                "queue.update",
                &ControlClient::update_queue,
                jb::jobu::UpdateQueueRequest{.queue = queue.id, .expected_revision = queue.revision, .weight = 5});
            REQUIRE(save);
            auto result = call(save.value());
            REQUIRE_FALSE(result);
            CHECK(result.error().outcome_unknown);
            CHECK(result.error().kind == jb::jobu::ControlFailureKind::Remote);
            CHECK(jb::php::failure_metadata(result.error()).code == "jobu.response.too_large");
            CHECK(proxy.requests["queue.update"].size() == 1);
        }
        SECTION("observed remote errors and invalid local requests never retry")
        {
            auto error = jb::core::parse_json(
                R"({"code":-32000,"message":"PRIVATE","data":{"code":"jobu.history.invalid_cursor","category":"invalid_argument"}})");
            REQUIRE(error);
            proxy.error_method   = "system.info";
            proxy.response_error = error.value();
            auto before          = proxy.connections;
            auto result          = call(Operation::bind("system.info", &ControlClient::get_system_info));
            REQUIRE_FALSE(result);
            CHECK_FALSE(result.error().outcome_unknown);
            CHECK(jb::php::failure_metadata(result.error()).code == "jobu.history.invalid_cursor");
            CHECK(proxy.connections == before);
            auto invalid = Operation::bind("queue.create",
                                           &ControlClient::create_queue,
                                           jb::jobu::CreateQueueRequest{.name = "invalid", .idempotency_key = ""});
            REQUIRE_FALSE(invalid);
            CHECK(invalid.error().code == "php.client.invalid_idempotency_key");
            CHECK(proxy.connections == before);
        }
        SECTION("remote revision conflict followed by EOF never replays an unsafe update")
        {
            // The daemon legitimately rejects this mismatched revision. The proxy forwards
            // the original error and closes before the client can dispatch deferred tasks.
            proxy.close_after_response = true;
            auto save                  = Operation::bind(
                "queue.update",
                &ControlClient::update_queue,
                jb::jobu::UpdateQueueRequest{.queue = queue.id, .expected_revision = queue.revision + 1, .weight = 5});
            REQUIRE(save);
            auto const connections = proxy.connections;
            auto       result      = call(save.value());
            REQUIRE_FALSE(result);
            REQUIRE(result.error().kind == jb::jobu::ControlFailureKind::Remote);
            CHECK_FALSE(result.error().outcome_unknown);
            auto const metadata = jb::php::failure_metadata(result.error());
            CHECK(metadata.code == "jobu.queue.revision_conflict");
            auto const& error = std::get<jb::rpc::RpcError>(result.error().error);
            CHECK(error.code == -32000);
            CHECK_FALSE(error.message.empty());
            REQUIRE(error.data);
            CHECK(error.data->as_object().at("code").as_string() == "jobu.queue.revision_conflict");
            CHECK(proxy.requests["queue.update"].size() == 1);
            CHECK(proxy.connections == connections);
            CHECK(client.state() == jb::php::ConnectionState::Disconnected);
        }
        SECTION("desired state actions replay safely")
        {
            for (bool suspend : {true, false}) {
                auto const* name = suspend ? "queue.suspend" : "queue.resume";
                auto action = Operation::bind(name,
                                              suspend ? &ControlClient::suspend_queue : &ControlClient::resume_queue,
                                              jb::jobu::QueueSelector{queue.id});
                REQUIRE(action);
                proxy.drop_method = name;
                proxy.drops       = 1;
                auto result       = call(action.value());
                REQUIRE(result);
                CHECK(proxy.requests[name].size() == 2);
                CHECK(proxy.requests[name][0] == proxy.requests[name][1]);
                CHECK(std::get<jb::jobu::Queue>(result.value()).state ==
                      (suspend ? jb::jobu::QueueState::Suspended : jb::jobu::QueueState::Active));
            }
        }
        SECTION("job creation and Run Now retain explicit and generated keys")
        {
            auto payload = jb::core::parse_json(R"({"command":"/bin/true"})");
            REQUIRE(payload);
            jb::jobu::CreateJobRequest input{.queue = queue.id,
                                             .schedule =
                                                 jb::jobu::OnceSchedule{.planned_at = jb::core::UtcClock::now() + 1h},
                                             .payload         = payload.value(),
                                             .idempotency_key = "explicit-job-key"};
            auto                       create = Operation::bind("job.create", &ControlClient::create_job, input);
            REQUIRE(create);
            // Mutating the source after binding must not alter the frozen request.
            input.name        = "changed-after-binding";
            proxy.drop_method = "job.create";
            proxy.drops       = 1;
            auto result       = call(create.value());
            REQUIRE(result);
            auto job = std::get<jb::jobu::JobDefinition>(result.value());
            CHECK_FALSE(job.name);
            CHECK(proxy.requests["job.create"].size() == 2);
            CHECK(proxy.requests["job.create"][0] == proxy.requests["job.create"][1]);
            CHECK(create.value().key() == "explicit-job-key");

            auto run =
                Operation::bind("job.run_now", &ControlClient::run_now, jb::jobu::RunNowRequest{.job_id = job.id});
            REQUIRE(run);
            proxy.drop_method = "job.run_now";
            proxy.drops       = 1;
            auto ran          = call(run.value());
            REQUIRE(ran);
            CHECK(proxy.requests["job.run_now"].size() == 2);
            CHECK(proxy.requests["job.run_now"][0] == proxy.requests["job.run_now"][1]);
            CHECK(run.value().key()->size() == 72);
        }
        SECTION("unsent unsafe mutation can reconnect")
        {
            client.typed()->close(); // Deterministic pre-write terminal state.
            auto save = Operation::bind(
                "queue.update",
                &ControlClient::update_queue,
                jb::jobu::UpdateQueueRequest{.queue = queue.id, .expected_revision = queue.revision, .weight = 9});
            REQUIRE(save);
            auto result = call(save.value());
            REQUIRE(result);
            CHECK(proxy.requests["queue.update"].size() == 1);
            CHECK(std::get<jb::jobu::Queue>(result.value()).weight == 9);
        }
        SECTION("unresolved keyed commit survives incompatible reconnect")
        {
            auto create = Operation::bind("queue.create",
                                          &ControlClient::create_queue,
                                          jb::jobu::CreateQueueRequest{.name = "uncertain"});
            REQUIRE(create);
            proxy.drop_method = "queue.create";
            proxy.drops       = 1;
            proxy.after_drop  = [&] { proxy.incompatible = true; };
            auto result       = call(create.value());
            REQUIRE_FALSE(result);
            CHECK(result.error().outcome_unknown);
            CHECK(jb::php::failure_metadata(result.error()).code == "jobu.client.unsupported_api");
            CHECK(client.state() == jb::php::ConnectionState::Unusable);
            CHECK(proxy.requests["queue.create"].size() == 3); // Baseline pair, then one uncertain commit.
        }
        SECTION("reconnect exhaustion uses one budget and makes the client unusable")
        {
            proxy.drop_method = "system.info";
            proxy.drops       = 1;
            proxy.after_drop  = [&] { proxy.stop_accepting(); };
            auto read         = Operation::bind("system.info", &ControlClient::get_system_info);
            auto deadline     = client.deadline(Clock::now(), 30ms);
            REQUIRE(deadline);
            auto result = client.call(deadline.value(), read);
            REQUIRE_FALSE(result);
            CHECK_FALSE(result.error().outcome_unknown);
            CHECK(client.state() == jb::php::ConnectionState::Unusable);
            auto count = proxy.connections;
            CHECK_FALSE(call(read));
            CHECK(proxy.connections == count);
            CHECK(client.timeout() == 2s);
        }
        SECTION("read retries and explicit close never reconnects")
        {
            auto read         = Operation::bind("system.info", &ControlClient::get_system_info);
            proxy.drop_method = "system.info";
            proxy.drops       = 1;
            REQUIRE(call(read));
            auto count = proxy.connections;
            client.close();
            CHECK_FALSE(call(read));
            CHECK(proxy.connections == count);
        }
    }
    daemon.stop(host.value()->loop());
}

TEST_CASE("PHP operation errors preserve identity without exposing submitted data", "[php][recovery]")
{
    Runtime runtime;
    REQUIRE(evaluate(R"PHP((function () {
        $cases = [
            ['jobu.queue.not_found', 'not_found', 'NotFoundException'],
            ['jobu.queue.revision_conflict', 'conflict', 'RevisionConflictException'],
            ['jobu.job.revision_conflict', 'conflict', 'RevisionConflictException'],
            ['jobu.history.invalid_cursor', 'invalid_argument', 'InvalidCursorException'],
            ['jobu.statistics.invalid_cursor', 'invalid_argument', 'InvalidCursorException'],
            ['jobu.test.timeout', 'timeout', 'TimeoutException'],
            ['jobu.test.unsupported', 'unsupported', 'UnsupportedFeatureException'],
            ['jobu.response.too_large', 'resource_exhausted', 'OutcomeUnknownException'],
            ['jobu.other', 'internal', 'Exception'],
        ];
        foreach ($cases as [$code, $category, $class]) {
            try { JobUTestBridge::failure($code, $category, true); return false; }
            catch (JobU\Exception $e) {
                if (get_class($e) !== 'JobU\\' . $class || $e->getErrorCode() !== $code ||
                    $e->getCategory() !== $category || $e->getCode() !== -32000 ||
                    str_contains($e->getMessage(), 'PRIVATE')) return false;
            }
        }
        foreach ([false, true] as $remote) {
            try { JobUTestBridge::failure('test.lost', 'unavailable', $remote, true, 'original-key'); return false; }
            catch (JobU\OutcomeUnknownException $e) {
                if ($e->getOperation() !== 'queue.create' || $e->getIdempotencyKey() !== 'original-key' ||
                    $e->getErrorCode() !== 'test.lost') return false;
            }
        }
        try { JobUTestBridge::failure('rpc.connection_closed', 'unavailable', false); return false; }
        catch (JobU\ConnectionException $e) {}
        try { JobUTestBridge::failure('test', 'internal', false, false, null, true); return false; }
        catch (ValueError $e) { if ($e->getMessage() !== 'original') return false; }
        return true;
    })())PHP"));
}

TEST_CASE("PHP per-call Duration overrides and unknown keys use the production recovery path", "[php][recovery]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(host.value()->loop(), directory.path());
    {
        Runtime runtime;
        auto    anchor = jb::php::current_request()->create_session();
        REQUIRE(anchor);
        auto                         endpoint = (directory.path() / "php-proxy.sock").string();
        jobu_php_test::RecoveryProxy proxy(endpoint, (directory.path() / "daemon.sock").string());
        zend_register_string_constant("JOBU_TEST_SOCKET",
                                      sizeof("JOBU_TEST_SOCKET") - 1,
                                      endpoint.c_str(),
                                      CONST_CS,
                                      0);
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['recoveryClient'] = new JobU\Client(JOBU_TEST_SOCKET, JobU\Duration::seconds(1));
            $client = $GLOBALS['recoveryClient'];
            foreach ([0, -1, PHP_INT_MAX] as $ms) {
                try { JobUTestBridge::ping($client, JobU\Duration::milliseconds($ms)); return false; }
                catch (ValueError $e) {}
            }
            return JobUTestBridge::ping($client, JobU\Duration::seconds(2)) && JobUTestBridge::timeout($client) === 1000;
        })())PHP"));
        proxy.drop_method = "queue.create";
        proxy.drops       = 1;
        proxy.after_drop  = [&] { proxy.incompatible = true; };
        REQUIRE(evaluate(R"PHP((function () {
            try { JobUTestBridge::createQueue($GLOBALS['recoveryClient'], 'php-unknown', JobU\Duration::seconds(1)); return false; }
            catch (JobU\OutcomeUnknownException $e) {
                return $e->getOperation() === 'queue.create' && strlen($e->getIdempotencyKey()) === 72 &&
                       $e->getErrorCode() === 'jobu.client.unsupported_api' && !str_contains($e->getMessage(), 'php-unknown');
            }
        })())PHP"));
        CHECK(proxy.requests["queue.create"].size() == 1);
    }
    daemon.stop(host.value()->loop());
}

TEST_CASE("Independent clients survive daemon restart and preserve explicit closure", "[php][recovery]")
{
    jb::php::Request request;
    auto             anchor = request.create_session();
    REQUIRE(anchor);
    auto&                        loop = *request.loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       original;
    original.start(loop, directory.path());
    auto            endpoint = (directory.path() / "daemon.sock").string();
    jb::php::Client closed;
    jb::php::Client surviving;
    REQUIRE(closed.initialize(request, endpoint, Clock::now() + 2s, 2s));
    REQUIRE(surviving.initialize(request, endpoint, Clock::now() + 2s, 2s));
    closed.close();
    original.stop(loop);
    Daemon replacement;
    replacement.start(loop, directory.path());
    auto read = jb::php::Operation::bind("system.info", &jb::jobu::ControlClient::get_system_info);
    CHECK_FALSE(closed.call(Clock::now() + 2s, read));
    CHECK(surviving.call(Clock::now() + 2s, read));
    replacement.stop(loop);
}

TEST_CASE("Value adapters preserve native schedule and identifier contracts", "[php][lifecycle]")
{
    Runtime runtime;
    auto    value = [](std::string_view expression) {
        zval slot{};
        auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "value adapter test");
        auto owner  = php::Value::adopt(slot);
        REQUIRE(status == SUCCESS);
        REQUIRE_FALSE(php::has_exception());
        return owner;
    };

    SECTION("UUID spelling is stricter than the shared native parser")
    {
        for (auto const* text : {"00000000-0000-0000-0000-000000000000", "019a1234-abcd-7abc-8def-0123456789ab"}) {
            auto input  = value(fmt::format("'{}'", text));
            auto parsed = jb::php::uuid_from_php(input.view());
            REQUIRE(parsed);
            CHECK(parsed.value().to_string() == text);
        }
        for (auto const* text : {"019A1234-ABCD-7ABC-8DEF-0123456789AB",
                                 "bad",
                                 " 019a1234-abcd-7abc-8def-0123456789ab",
                                 "{019a1234-abcd-7abc-8def-0123456789ab}",
                                 ""}) {
            auto input  = value(fmt::format("'{}'", text));
            auto parsed = jb::php::uuid_from_php(input.view());
            REQUIRE_FALSE(parsed);
            CHECK(parsed.error().kind == php::ErrorKind::Value);
        }
        auto wrong = value("123");
        CHECK(jb::php::uuid_from_php(wrong.view()).error().kind == php::ErrorKind::Type);
        auto binary = value(R"("019a1234-abcd-7abc-8def-0123456789ab\0")");
        CHECK_FALSE(jb::php::uuid_from_php(binary.view()));
        CHECK(jb::core::Uuid::parse("019A1234-ABCD-7ABC-8DEF-0123456789AB"));
    }

    SECTION("Creation now cannot become an update or an absolute timestamp")
    {
        auto now      = value("JobU\\OnceSchedule::now()");
        auto creation = jb::php::creation_schedule_from_php(now.view());
        REQUIRE(creation);
        CHECK(std::holds_alternative<jb::jobu::ImmediateSchedule>(creation.value()));
        auto update = jb::php::update_schedule_from_php(now.view());
        REQUIRE_FALSE(update);
        CHECK(update.error().kind == php::ErrorKind::Value);
        auto fake = value("new class extends JobU\\Schedule {}");
        CHECK_FALSE(jb::php::creation_schedule_from_php(fake.view()));
    }

    SECTION("Cron omission survives input conversion and loaded zones remain explicit")
    {
        auto input    = value("new JobU\\CronSchedule('@daily')");
        auto creation = jb::php::creation_schedule_from_php(input.view());
        auto update   = jb::php::update_schedule_from_php(input.view());
        REQUIRE(creation);
        REQUIRE(update);
        CHECK_FALSE(std::get<jb::jobu::CronScheduleInput>(creation.value()).timezone);
        CHECK_FALSE(std::get<jb::jobu::CronScheduleInput>(update.value()).timezone);
        for (auto const* zone : {"UTC", "Europe/Tallinn"}) {
            auto loaded = jb::php::schedule_to_php(jb::jobu::CronSchedule{.expression = "@daily", .timezone = zone});
            REQUIRE(loaded);
            auto roundtrip = jb::php::update_schedule_from_php(loaded.value().view());
            REQUIRE(roundtrip);
            CHECK(std::get<jb::jobu::CronScheduleInput>(roundtrip.value()).timezone == zone);
        }
    }

    SECTION("Native clock bounds and negative microseconds roundtrip exactly")
    {
        using Clock  = jb::core::UtcClock;
        using Micros = std::chrono::microseconds;
        auto lower   = std::chrono::duration_cast<Micros>(Clock::duration::min()).count();
        auto upper   = std::chrono::duration_cast<Micros>(Clock::duration::max()).count();
        for (std::int64_t us :
             {lower, std::int64_t{-1000001}, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, upper}) {
            auto date = php::datetime_immutable(php::timestamp_from_microseconds(us));
            REQUIRE(date);
            auto native = jb::php::time_from_php(date.value().view());
            REQUIRE(native);
            CHECK(native.value().time_since_epoch() == Micros{us});
            auto loaded = jb::php::schedule_to_php(jb::jobu::OnceSchedule{native.value()});
            REQUIRE(loaded);
            auto roundtrip = jb::php::update_schedule_from_php(loaded.value().view());
            REQUIRE(roundtrip);
            CHECK(std::get<jb::jobu::OnceSchedule>(roundtrip.value()).planned_at == native.value());
        }
        if (lower > std::numeric_limits<std::int64_t>::min() && upper < std::numeric_limits<std::int64_t>::max()) {
            for (auto us : {lower - 1,
                            upper + 1,
                            std::numeric_limits<std::int64_t>::min(),
                            std::numeric_limits<std::int64_t>::max()}) {
                auto date = php::datetime_immutable(php::timestamp_from_microseconds(us));
                REQUIRE(date);
                CHECK_FALSE(jb::php::time_from_php(date.value().view()));
            }
        }
        if constexpr (Clock::period::den > Micros::period::den) {
            CHECK_FALSE(jb::php::time_to_php(Clock::time_point{Clock::duration{1}}));
        }
    }

    SECTION("Pending PHP exception identity survives date and schedule conversions")
    {
        auto date = value("new DateTimeImmutable('2000-01-01T00:00:00Z')");
        // Install an owned exception directly, as in the helper lifecycle fixture;
        // raising one without an active PHP call frame would be a fatal error.
        zval exception{};
        object_init_ex(&exception, zend_ce_exception);
        EG(exception)  = Z_OBJ(exception);
        auto* original = EG(exception);
        CHECK_FALSE(jb::php::time_from_php(date.view()));
        CHECK_FALSE(jb::php::time_to_php(jb::core::UtcTimePoint{}));
        CHECK_FALSE(jb::php::schedule_to_php(jb::jobu::CronSchedule{.expression = "@daily", .timezone = "UTC"}));
        CHECK(EG(exception) == original);
        zend_clear_exception();
    }
}

TEST_CASE("Configuration copies preserve attachment and submission boundaries", "[php][lifecycle][attributes]")
{
    Runtime runtime;
    auto    value = [](std::string_view expression) {
        zval slot{};
        auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "attribute adapter test");
        auto owner  = php::Value::adopt(slot);
        REQUIRE(status == SUCCESS);
        REQUIRE_FALSE(php::has_exception());
        return owner;
    };

    SECTION("Merge includes unchanged source keys and rejects incompatible candidates atomically")
    {
        auto target = value("$GLOBALS['target'] = new JobU\\CliJobAttributes");
        auto source = value("$GLOBALS['source'] = new JobU\\JobAttributes");
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['target']->setStdoutLimit(11);
            $GLOBALS['target']->setRetryMaxAttempts(1);
            $GLOBALS['source']->setRetryMaxAttempts(2);
            return true;
        })())PHP"));
        REQUIRE(jb::php::merge_attributes(Z_OBJ_P(target.get()), source.view()));
        REQUIRE(jb::php::merge_attributes(Z_OBJ_P(target.get()), source.view()));
        CHECK(
            evaluate("$GLOBALS['target']->getStdoutLimit() === 11 && $GLOBALS['target']->getRetryMaxAttempts() === 2"));

        REQUIRE(evaluate("(function () { $GLOBALS['source']->set('http.tls_verify', false); return true; })()"));
        CHECK_FALSE(jb::php::merge_attributes(Z_OBJ_P(target.get()), source.view()));
        CHECK(evaluate("$GLOBALS['target']->getRetryMaxAttempts() === 2"));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['target']->setRetryInitialDelay(JobU\Duration::seconds(10));
            $GLOBALS['target']->setRetryMaxDelay(JobU\Duration::seconds(1));
            return true;
        })())PHP"));
        CHECK_FALSE(jb::php::attributes_from_php(target.view()));
        REQUIRE(evaluate(
            "(function () { $GLOBALS['target']->setRetryMaxDelay(JobU\\Duration::seconds(20)); return true; })()"));
        CHECK(jb::php::attributes_from_php(target.view()));
    }

    SECTION("Replacement preserves views and attached clones drop owners")
    {
        auto changes = std::make_shared<int>(0);
        auto target  = value("$GLOBALS['defaults'] = new JobU\\QueueDefaults");
        auto owner   = value("$GLOBALS['owner'] = new stdClass");
        REQUIRE(jb::php::attach_attributes(Z_OBJ_P(target.get()), owner.view(), [changes] { ++*changes; }));
        CHECK_FALSE(jb::php::attach_attributes(Z_OBJ_P(target.get()), owner.view(), {}));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['owner']->defaults = $GLOBALS['defaults'];
            $GLOBALS['weakOwner'] = WeakReference::create($GLOBALS['owner']);
            $GLOBALS['cliView'] = $GLOBALS['defaults']->cli();
            $GLOBALS['httpView'] = $GLOBALS['defaults']->http();
            $GLOBALS['cliView']->setStdoutLimit(10);
            $GLOBALS['cliView']->setStdoutLimit(10);
            return true;
        })())PHP"));
        CHECK(*changes == 1);
        auto source = value("$GLOBALS['replacement'] = new JobU\\QueueDefaults");
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['replacement']->http()->setHttpBodyLimit(20);
            $GLOBALS['replacement']->cli()->setTimeout(JobU\Duration::seconds(3));
            return true;
        })())PHP"));
        REQUIRE(jb::php::replace_defaults(Z_OBJ_P(target.get()), source.view()));
        REQUIRE(jb::php::replace_defaults(Z_OBJ_P(target.get()), source.view()));
        CHECK(*changes == 2);
        CHECK(evaluate(R"PHP($GLOBALS['defaults']->cli() === $GLOBALS['cliView'] &&
            $GLOBALS['defaults']->http() === $GLOBALS['httpView'] &&
            $GLOBALS['cliView']->getStdoutLimit() === null &&
            $GLOBALS['httpView']->getTimeout()->getMilliseconds() === 3000 &&
            $GLOBALS['httpView']->getHttpBodyLimit() === 20)PHP"));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['detached'] = clone $GLOBALS['defaults'];
            $GLOBALS['viewCopy'] = clone $GLOBALS['cliView'];
            $GLOBALS['detached']->clear('job.timeout');
            $GLOBALS['viewCopy']->setStdoutLimit(99);
            return true;
        })())PHP"));
        CHECK(*changes == 2);

        // A failed full-layer validation leaves the old layer and notifications intact.
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['replacement']->http()->setFollowRedirects(true);
            $GLOBALS['replacement']->http()->setMaxRedirects(0);
            return true;
        })())PHP"));
        CHECK_FALSE(jb::php::replace_defaults(Z_OBJ_P(target.get()), source.view()));
        CHECK(*changes == 2);
        CHECK(evaluate("$GLOBALS['httpView']->getMaxRedirects() === null"));

        target = {};
        owner  = {};
        REQUIRE(evaluate(R"PHP((function () {
            unset($GLOBALS['owner'], $GLOBALS['defaults']);
            gc_collect_cycles();
            return $GLOBALS['weakOwner']->get() !== null;
        })())PHP"));
        REQUIRE(evaluate(R"PHP((function () {
            unset($GLOBALS['cliView'], $GLOBALS['httpView']);
            gc_collect_cycles();
            return $GLOBALS['weakOwner']->get() === null &&
                $GLOBALS['viewCopy']->getStdoutLimit() === 99;
        })())PHP"));
    }

    SECTION("Configuration cycles are released at repeated request shutdown")
    {
        for (int iteration = 0; iteration != 3; ++iteration) {
            REQUIRE(evaluate(R"PHP((function () {
                $GLOBALS['defaults'] = new JobU\QueueDefaults;
                $GLOBALS['defaults']->cli()->setStdoutLimit(5);
                $GLOBALS['defaults']->http()->setHttpBodyLimit(6);
                return true;
            })())PHP"));
            runtime.restart(nullptr);
        }
    }
}

TEST_CASE("CLI templates preserve wire values and request-local ownership", "[php][lifecycle][cli-payload]")
{
    Runtime runtime;
    auto    value = [](std::string_view expression) {
        zval slot{};
        auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "CLI payload adapter test");
        auto owner  = php::Value::adopt(slot);
        REQUIRE(status == SUCCESS);
        REQUIRE_FALSE(php::has_exception());
        return owner;
    };

    SECTION("Export emits the separated template without resolving secrets")
    {
        auto payload = value(R"PHP($GLOBALS['payload'] = new JobU\CliPayload('"/opt/My Tools/worker" --verbose',
            ['two words', new JobU\SecretReference('service.token')]))PHP");
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->setEnvironment('TOKEN', new JobU\SecretReference('service.token'));
            $p->unsetEnvironment('HOME');
            $p->setWorkingDirectory('/tmp');
            $p->setExpectedExitCodes([7, 0]);
            return true;
        })())PHP"));
        auto exported = jb::php::cli_payload_from_php(payload.view());
        REQUIRE(exported);
        auto expected = jb::core::parse_json(R"JSON({"command":"/opt/My Tools/worker",
            "arguments":["--verbose","two words",{"secret":"service.token"}],
            "environment":{"HOME":null,"TOKEN":{"secret":"service.token"}},
            "working_directory":"/tmp","expected_exit_codes":[0,7]})JSON");
        REQUIRE(expected);
        CHECK(exported.value() == expected.value());

        // The existing typed request codec preserves this template unchanged.
        jb::jobu::CreateJobRequest request;
        request.queue    = std::string("payload-test");
        request.type     = jb::jobu::JobType::Cli;
        request.schedule = jb::jobu::ImmediateSchedule{};
        request.payload  = exported.value();
        auto wire        = jb::jobu::create_job_request_to_json(request, jb::jobu::StandardAttributeRegistry{});
        REQUIRE(wire);
        CHECK(wire->as_object().at("payload") == expected.value());
    }

    SECTION("Complete validation rejects missing PATH and bounded routing or payload overflow")
    {
        auto payload = value("$GLOBALS['payload'] = new JobU\\CliPayload('worker')");
        CHECK_FALSE(jb::php::cli_payload_from_php(payload.view()));
        REQUIRE(
            evaluate("(function () { $GLOBALS['payload']->setEnvironment('PATH', '/bin:/usr/bin'); return true; })()"));
        REQUIRE(jb::php::cli_payload_from_php(payload.view()));
        for (auto const* path : {"", ":/bin", "/bin:", ".:/bin", "relative"}) {
            INFO(path);
            REQUIRE(evaluate(
                fmt::format("(function () {{ $GLOBALS['payload']->setEnvironment('PATH', '{}'); return true; }})()",
                            path)));
            CHECK_FALSE(jb::php::cli_payload_from_php(payload.view()));
        }
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['payload']->setEnvironment('PATH', implode(':', array_fill(0, 257, '/bin')));
            return true;
        })())PHP"));
        CHECK_FALSE(jb::php::cli_payload_from_php(payload.view()));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['payload']->setEnvironment('PATH', '/' . str_repeat('x', 262144));
            return true;
        })())PHP"));
        CHECK_FALSE(jb::php::cli_payload_from_php(payload.view()));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['payload']->setExecutable('/bin/echo');
            $GLOBALS['payload']->clearEnvironment('PATH');
            $GLOBALS['payload']->setArguments([str_repeat('x', 262144)]);
            return true;
        })())PHP"));
        CHECK_FALSE(jb::php::cli_payload_from_php(payload.view()));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['payload']->setArguments([str_repeat("\n", 131072)]);
            return true;
        })())PHP"));
        CHECK_FALSE(jb::php::cli_payload_from_php(payload.view()));
    }

    SECTION("Replacement keeps identity, notification tracks changes, and GC sees the retained owner")
    {
        auto changes = std::make_shared<int>(0);
        auto target  = value("$GLOBALS['payload'] = new JobU\\CliPayload('/bin/echo old')");
        auto owner   = value("$GLOBALS['owner'] = new stdClass");
        REQUIRE(jb::php::attach_cli_payload(Z_OBJ_P(target.get()), owner.view(), [changes] { ++*changes; }));
        CHECK_FALSE(jb::php::attach_cli_payload(Z_OBJ_P(target.get()), owner.view(), {}));
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['alias'] = $GLOBALS['payload'];
            $GLOBALS['owner']->payload = $GLOBALS['payload'];
            $GLOBALS['weakOwner'] = WeakReference::create($GLOBALS['owner']);
            $GLOBALS['payload']->setWorkingDirectory('/tmp');
            $GLOBALS['payload']->setWorkingDirectory('/tmp');
            try { $GLOBALS['payload']->setCommand('/bin/echo "'); } catch (ValueError $e) {}
            return true;
        })())PHP"));
        CHECK(*changes == 1);
        auto source = value("new JobU\\CliPayload('/bin/printf new')");
        REQUIRE(jb::php::replace_cli_payload(Z_OBJ_P(target.get()), source.view()));
        REQUIRE(jb::php::replace_cli_payload(Z_OBJ_P(target.get()), source.view()));
        CHECK(*changes == 2);
        CHECK(evaluate(R"PHP($GLOBALS['alias'] === $GLOBALS['payload'] &&
            $GLOBALS['alias']->getExecutable() === '/bin/printf' &&
            $GLOBALS['alias']->getArguments() === ['new'] &&
            $GLOBALS['alias']->getWorkingDirectory() === '/')PHP"));
        auto invalid = value("new JobU\\CliPayload('missing-path')");
        CHECK_FALSE(jb::php::replace_cli_payload(Z_OBJ_P(target.get()), invalid.view()));
        CHECK(*changes == 2);
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['clone'] = clone $GLOBALS['payload'];
            $GLOBALS['clone']->addArgument('detached');
            return true;
        })())PHP"));
        CHECK(*changes == 2);

        target = {};
        owner  = {};
        REQUIRE(evaluate(R"PHP((function () {
            unset($GLOBALS['owner'], $GLOBALS['payload']);
            gc_collect_cycles();
            return $GLOBALS['weakOwner']->get() !== null;
        })())PHP"));
        REQUIRE(evaluate(R"PHP((function () {
            unset($GLOBALS['alias']);
            gc_collect_cycles();
            return $GLOBALS['weakOwner']->get() === null &&
                $GLOBALS['clone']->getArguments() === ['new', 'detached'];
        })())PHP"));
    }

    SECTION("Setters notify once for their own changes after nested coercion edits")
    {
        auto                             target = value("$GLOBALS['payload'] = new JobU\\CliPayload('/bin/echo old')");
        auto                             owner  = value("new stdClass");
        std::vector<jb::core::JsonValue> notifications;
        auto*                            object = Z_OBJ_P(target.get());
        REQUIRE(jb::php::attach_cli_payload(object, owner.view(), [&] {
            // Native export executes no PHP. Snapshotting here verifies that
            // observers never see only half of setCommand's executable/argv pair.
            auto snapshot = jb::php::cli_payload_from_php(target.view());
            REQUIRE(snapshot);
            notifications.push_back(std::move(snapshot).value());
        }));
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->setCommand(new class($p) {
                public function __construct(private $p) {}
                public function __toString(): string {
                    $this->p->setEnvironment('KEEP', 'nested');
                    return '/bin/printf new';
                }
            });
            return true;
        })())PHP"));
        REQUIRE(notifications.size() == 2);
        CHECK(notifications[0].as_object().at("command").as_string() == "/bin/echo");
        CHECK(notifications[0].as_object().at("arguments").as_array().front().as_string() == "old");
        CHECK(notifications[1].as_object().at("command").as_string() == "/bin/printf");
        CHECK(notifications[1].as_object().at("arguments").as_array().front().as_string() == "new");
        CHECK(notifications[1].as_object().at("environment").as_object().at("KEEP").as_string() == "nested");

        // The nested setter already installs the outer command. Compare against
        // current fields so the outer call neither erases KEEP nor notifies twice.
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->setCommand(new class($p) {
                public function __construct(private $p) {}
                public function __toString(): string {
                    $this->p->setCommand('/bin/echo latest');
                    return '/bin/echo latest';
                }
            });
            return $p->getEnvironment() === ['KEEP' => 'nested'];
        })())PHP"));
        CHECK(notifications.size() == 3);

        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            try {
                $p->setCommand(new class($p) {
                    public function __construct(private $p) {}
                    public function __toString(): string {
                        $this->p->setWorkingDirectory('/tmp');
                        return '/bin/printf rejected';
                    }
                }, [123]);
                return false;
            } catch (TypeError $e) {}
            return $p->getExecutable() === '/bin/echo' && $p->getArguments() === ['latest'] &&
                $p->getWorkingDirectory() === '/tmp';
        })())PHP"));
        CHECK(notifications.size() == 4);

        // Exercise every setter's no-op rule. An append always changes argv;
        // assigning an existing explicit null is a no-op, unlike adding that null.
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->setCommand('/bin/echo latest');
            $p->setExecutable('/bin/echo');
            $p->setArguments(['latest']);
            $p->setWorkingDirectory('/tmp');
            $p->setExpectedExitCodes([0]);
            $p->setEnvironment('KEEP', 'nested');
            $p->clearEnvironment('ABSENT');
            return true;
        })())PHP"));
        CHECK(notifications.size() == 4);
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->addArgument('appended');
            $p->setExpectedExitCodes([7, 0]);
            $p->setExpectedExitCodes([0, 7]);
            $p->unsetEnvironment('HOME');
            $p->unsetEnvironment('HOME');
            $p->clearEnvironment('HOME');
            $p->clearEnvironment('HOME');
            return true;
        })())PHP"));
        CHECK(notifications.size() == 8);
    }

    SECTION("Attached cycles die at repeated request shutdown")
    {
        for (int iteration = 0; iteration != 3; ++iteration) {
            {
                auto payload = value("$GLOBALS['payload'] = new JobU\\CliPayload('/bin/echo')");
                auto owner   = value("$GLOBALS['owner'] = new stdClass");
                REQUIRE(jb::php::attach_cli_payload(Z_OBJ_P(payload.get()), owner.view(), {}));
                REQUIRE(evaluate("(function () { $GLOBALS['owner']->payload = $GLOBALS['payload']; return true; })()"));
            }
            runtime.restart(nullptr);
        }
    }
}

TEST_CASE("HTTP templates preserve wire values ownership and callback atomicity", "[php][lifecycle][http-payload]")
{
    Runtime runtime;
    auto    value = [](std::string_view expression) {
        zval slot{};
        auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "HTTP payload adapter test");
        auto owner  = php::Value::adopt(slot);
        REQUIRE(status == SUCCESS);
        REQUIRE_FALSE(php::has_exception());
        return owner;
    };

    SECTION("Wire conversion preserves duplicates binary and symbolic references")
    {
        auto payload = value("$GLOBALS['payload'] = new JobU\\HttpPayload('https://example.test/', 'post')");
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->addHeader('X-Test', 'literal');
            $p->addHeader('x-test', new JobU\SecretReference('service.token'));
            $p->setBody("\x00\xff\xfe");
            return true;
        })())PHP"));
        auto exported = jb::php::http_payload_from_php(payload.view());
        REQUIRE(exported);
        auto expected = jb::core::parse_json(R"JSON({"url":"https://example.test/","method":"POST",
            "headers":[{"name":"X-Test","value":"literal","sensitive":false},
                       {"name":"x-test","value":{"secret":"service.token"},"sensitive":true}],
            "body":{"encoding":"base64","data":"AP/+"},"expected_statuses":["200-299"]})JSON");
        REQUIRE(expected);
        CHECK(exported.value() == expected.value());
        jb::jobu::CreateJobRequest request;
        request.queue    = std::string("payload-test");
        request.type     = jb::jobu::JobType::Http;
        request.schedule = jb::jobu::ImmediateSchedule{};
        request.payload  = exported.value();
        auto wire        = jb::jobu::create_job_request_to_json(request, jb::jobu::StandardAttributeRegistry{});
        REQUIRE(wire);
        CHECK(wire->as_object().at("payload") == expected.value());

        REQUIRE(evaluate(
            "(function () { $GLOBALS['payload']->setBody(new JobU\\SecretReference('body.bytes')); return true; })()"));
        exported = jb::php::http_payload_from_php(payload.view());
        REQUIRE(exported);
        CHECK(exported.value().as_object().at("body").as_object().at("secret").as_string() == "body.bytes");
        REQUIRE(evaluate("(function () { $GLOBALS['payload']->setBody(\"nul\\0text\"); return true; })()"));
        exported = jb::php::http_payload_from_php(payload.view());
        REQUIRE(exported);
        auto const& body = exported.value().as_object().at("body").as_object();
        CHECK(body.at("encoding").as_string() == "utf8");
        CHECK(body.at("data").as_string() == std::string("nul\0text", 8));
    }

    SECTION("Notifications observe complete edits and clones drop attachments")
    {
        auto target = value("$GLOBALS['payload'] = new JobU\\HttpPayload('https://example.test/')");
        auto owner  = value("$GLOBALS['owner'] = new stdClass");
        std::vector<jb::core::JsonValue> notifications;
        REQUIRE(jb::php::attach_http_payload(Z_OBJ_P(target.get()), owner.view(), [&] {
            auto snapshot = jb::php::http_payload_from_php(target.view());
            REQUIRE(snapshot);
            notifications.push_back(std::move(snapshot).value());
        }));
        CHECK_FALSE(jb::php::attach_http_payload(Z_OBJ_P(target.get()), owner.view(), {}));
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $GLOBALS['alias'] = $p;
            $GLOBALS['weakOwner'] = WeakReference::create($GLOBALS['owner']);
            $GLOBALS['owner']->payload = $p;
            $p->setJsonBody(['x' => 1]);
            $p->setJsonBody(['x' => 1]);
            $p->clearHeader('missing');
            try { $p->setMethod('HEAD'); } catch (ValueError $error) {}
            return true;
        })())PHP"));
        REQUIRE(notifications.size() == 1);
        CHECK(notifications[0].as_object().at("headers").as_array().size() == 1);
        CHECK(notifications[0].as_object().at("body").as_object().at("data").as_string() == "{\"x\":1}");
        REQUIRE(evaluate(R"PHP((function () {
            $p = $GLOBALS['payload'];
            $p->setJsonBody(new class($p) implements JsonSerializable {
                public function __construct(private JobU\HttpPayload $p) {}
                public function jsonSerialize(): mixed {
                    $this->p->setUrl('https://nested.test/');
                    return ['y' => 2];
                }
            });
            try {
                $p->setJsonBody(new class($p) implements JsonSerializable {
                    public function __construct(private JobU\HttpPayload $p) {}
                    public function jsonSerialize(): mixed {
                        $this->p->setUrl('https://failed.test/');
                        throw new LogicException('nested');
                    }
                });
            } catch (LogicException $error) {}
            return true;
        })())PHP"));
        CHECK(notifications.size() == 4); // nested URL, outer JSON, then failed callback's URL only.

        auto source = value("new JobU\\HttpPayload('https://replacement.test/', 'HEAD')");
        REQUIRE(jb::php::replace_http_payload(Z_OBJ_P(target.get()), source.view()));
        REQUIRE(jb::php::replace_http_payload(Z_OBJ_P(target.get()), source.view()));
        CHECK(notifications.size() == 5);
        REQUIRE(evaluate(R"PHP((function () {
            $GLOBALS['clone'] = clone $GLOBALS['payload'];
            $GLOBALS['clone']->setUrl('https://detached.test/');
            return $GLOBALS['alias'] === $GLOBALS['payload'] && $GLOBALS['payload']->getBody() === null &&
                $GLOBALS['payload']->getHeaders() === [] && $GLOBALS['payload']->getMethod() === 'HEAD';
        })())PHP"));
        CHECK(notifications.size() == 5);

        target = {};
        owner  = {};
        REQUIRE(evaluate(R"PHP((function () {
            unset($GLOBALS['owner'], $GLOBALS['payload']);
            gc_collect_cycles();
            return $GLOBALS['weakOwner']->get() !== null;
        })())PHP"));
        REQUIRE(evaluate(R"PHP((function () {
            unset($GLOBALS['alias']);
            gc_collect_cycles();
            return $GLOBALS['weakOwner']->get() === null &&
                $GLOBALS['clone']->getUrl() === 'https://detached.test/';
        })())PHP"));
    }

    SECTION("Oversized complete templates reject export and replacement atomically")
    {
        auto source = value("$GLOBALS['source'] = new JobU\\HttpPayload('https://example.test/')");
        auto target = value("new JobU\\HttpPayload('https://target.test/')");
        auto before = jb::php::http_payload_from_php(target.view());
        REQUIRE(before);
        REQUIRE(evaluate("(function () { $GLOBALS['source']->setBody(str_repeat(\"\\0\", 100000)); return true; })()"));
        CHECK_FALSE(jb::php::http_payload_from_php(source.view()));
        CHECK_FALSE(jb::php::replace_http_payload(Z_OBJ_P(target.get()), source.view()));
        auto after = jb::php::http_payload_from_php(target.view());
        REQUIRE(after);
        CHECK(after.value() == before.value());
    }

    SECTION("Attached cycles release at repeated request shutdown")
    {
        for (int i = 0; i != 3; ++i) {
            {
                auto target = value("$GLOBALS['payload'] = new JobU\\HttpPayload('https://example.test/')");
                auto owner  = value("$GLOBALS['owner'] = new stdClass");
                REQUIRE(jb::php::attach_http_payload(Z_OBJ_P(target.get()), owner.view(), {}));
                REQUIRE(evaluate("(function () { $GLOBALS['owner']->payload = $GLOBALS['payload']; return true; })()"));
            }
            runtime.restart(nullptr);
        }
    }
}

TEST_CASE("HTTP PHP templates are admitted unchanged by the real daemon", "[php][lifecycle][http-payload]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(host.value()->loop(), directory.path());
    {
        Runtime runtime;
        REQUIRE(evaluate(R"PHP((function () {
            $p = new JobU\HttpPayload('https://example.test/', 'POST');
            $p->addHeader('X-Test', 'one');
            $p->addHeader('x-test', 'two');
            $p->setBody("\xff\0");
            $GLOBALS['payload'] = $p;
            return true;
        })())PHP"));
        auto* slot = zend_hash_str_find(&EG(symbol_table), "payload", 7);
        REQUIRE(slot != nullptr);
        auto exported = jb::php::http_payload_from_php(php::ValueView(slot));
        REQUIRE(exported);
        jb::php::Client client;
        REQUIRE(client.initialize(*jb::php::current_request(),
                                  (directory.path() / "daemon.sock").string(),
                                  Clock::now() + 2s,
                                  2s));
        auto queue = jb::php::Operation::bind("queue.create",
                                              &jb::jobu::ControlClient::create_queue,
                                              jb::jobu::CreateQueueRequest{.name = "http-payload"});
        REQUIRE(queue);
        REQUIRE(client.call(Clock::now() + 2s, queue.value()));
        jb::jobu::CreateJobRequest input{.queue = std::string("http-payload"),
                                         .type  = jb::jobu::JobType::Http,
                                         .schedule =
                                             jb::jobu::OnceSchedule{.planned_at = jb::core::UtcClock::now() + 1h},
                                         .payload = exported.value()};
        auto create = jb::php::Operation::bind("job.create", &jb::jobu::ControlClient::create_job, input);
        REQUIRE(create);
        auto result = client.call(Clock::now() + 2s, create.value());
        REQUIRE(result);
        CHECK(std::get<jb::jobu::JobDefinition>(result.value()).payload == exported.value());
    }
    daemon.stop(host.value()->loop());
}

TEST_CASE("PHP query adapters preserve initial wire filters and unresolved bounds", "[php][lifecycle][queries]")
{
    Runtime runtime;
    REQUIRE(evaluate(R"PHP((function () {
        $GLOBALS['runs'] = new JobU\RunQuery(
            queueId: '019c0000-0000-7000-8000-000000000001',
            states: [JobU\RunState::Failed, JobU\RunState::Running],
            origin: JobU\RunOrigin::Manual, type: JobU\JobType::Http,
            planned: new JobU\TimeRange(new DateTimeImmutable('2030-01-01T00:00:00.123456Z')), limit: 17);
        $GLOBALS['statistics'] = new JobU\StatisticsQuery(groupBy: JobU\StatisticsGroupBy::Queue);
        return true;
    })())PHP"));
    auto* runs  = zend_hash_str_find(&EG(symbol_table), "runs", 4);
    auto  query = jb::php::run_query_from_php(php::ValueView(runs));
    REQUIRE(query);
    CHECK(query.value().limit == 17);
    REQUIRE(query.value().filters.states);
    CHECK(query.value().filters.states->size() == 2);
    CHECK(query.value().filters.states->front() == jb::jobu::RunState::Failed);
    auto wire = jb::jobu::run_list_request_to_json(query.value());
    REQUIRE(wire);
    auto decoded = jb::jobu::run_list_request_from_json(wire.value());
    REQUIRE(decoded);
    CHECK(std::get<jb::jobu::RunQuery>(decoded.value()).filters.planned.from == query.value().filters.planned.from);
    CHECK_FALSE(query.value().filters.planned.to);

    auto* stats      = zend_hash_str_find(&EG(symbol_table), "statistics", 10);
    auto  statistics = jb::php::statistics_query_from_php(php::ValueView(stats));
    REQUIRE(statistics);
    CHECK_FALSE(statistics.value().planned.from);
    CHECK_FALSE(statistics.value().planned.to);
    CHECK(statistics.value().group_by == jb::jobu::StatisticsGroupBy::Queue);
    auto stats_wire = jb::jobu::system_statistics_request_to_json(statistics.value());
    REQUIRE(stats_wire);
    CHECK(jb::jobu::system_statistics_request_from_json(stats_wire.value()));
    CHECK_FALSE(jb::php::run_query_from_php(php::ValueView(stats)));
}

TEST_CASE("PHP metadata and result snapshots preserve codec data without client ownership", "[php][lifecycle][results]")
{
    Runtime runtime;
    auto    publish = [](std::string_view name, php::Result<php::Value> result) {
        REQUIRE(result);
        auto slot = result.value().release();
        zend_hash_str_update(&EG(symbol_table), name.data(), name.size(), &slot);
    };
    auto parse = [](std::string_view json) {
        auto parsed = jb::core::parse_json(json);
        REQUIRE(parsed);
        return std::move(parsed).value();
    };

    SECTION("Metadata uses existing codecs and returns independent values")
    {
        auto info = jb::jobu::system_info_from_json(parse(
            R"({"daemon_version":"test","api_version":{"major":1,"minor":7},"capabilities":["z","a","a"],"future":true})"));
        REQUIRE(info);
        publish("info", jb::php::system_info_to_php(info.value()));
        auto metadata = jb::jobu::secret_metadata_from_json(parse(
            R"({"name":"test.token","created_at":"2030-01-01T00:00:00.123456Z","updated_at":"2030-01-02T00:00:00Z","value":"PRIVATE"})"));
        REQUIRE(metadata);
        publish("secret", jb::php::secret_metadata_to_php(metadata.value()));
        REQUIRE(evaluate(R"PHP((function () {
            $i = $GLOBALS['info']; $s = $GLOBALS['secret'];
            $caps = $i->getCapabilities(); $caps[] = 'changed';
            return $i->getDaemonVersion() === 'test' && $i->getApiMajor() === 1 && $i->getApiMinor() === 7 &&
                $i->getCapabilities() === ['a','z'] && $i->hasCapability('a') && !$i->hasCapability('changed') &&
                $s->getName() === 'test.token' && $s->getCreatedAt()->format('u') === '123456' &&
                $s->getUpdatedAt()->format('Y-m-d e') === '2030-01-02 UTC' && !method_exists($s, 'getValue');
        })())PHP"));
        metadata->created_at += jb::core::UtcClock::duration(1);
        if (std::chrono::duration_cast<std::chrono::microseconds>(jb::core::UtcClock::duration(1)) !=
            jb::core::UtcClock::duration(1)) {
            CHECK_FALSE(jb::php::secret_metadata_to_php(metadata.value()));
        }
    }

    SECTION("Safe terminal JSON survives the history codec and PHP copy boundaries")
    {
        auto wire = parse(
            R"({"run_id":"019c0000-0000-7000-8000-000000000001","attempt_number":1,"due_at":"2030-01-01T00:00:00Z","started_at":null,"completed_at":null,"state":"pending","outcome":null,"result":{"type":"http","outcome":"success","exit_code":0,"signal":15,"capture_lost":false,"stdout":{"captured_bytes":0,"total_bytes":2,"truncated":true},"future":{"empty":{},"list":[],"null":null,"uint":18446744073709551615,"zero":0,"false":false,"text":"a\u0000b"},"null":null}})");
        auto details = jb::jobu::attempt_details_from_json(wire);
        REQUIRE(details);
        auto round_trip = jb::jobu::attempt_details_to_json(details.value());
        REQUIRE(round_trip);
        CHECK(round_trip->as_object().at("result") == wire.as_object().at("result"));
        publish("cli", jb::php::execution_result_to_php(jb::jobu::JobType::Cli, details->result));
        publish("equal", jb::php::execution_result_to_php(jb::jobu::JobType::Cli, details->result));
        REQUIRE(evaluate(R"PHP((function () {
            $r = $GLOBALS['cli']; $future = $r->get('future'); $future->empty->changed = true;
            try { clone $r; return false; } catch (Error) {}
            try { serialize($r); return false; } catch (Exception) {}
            try { $r->extra = 1; return false; } catch (Error) {}
            return $r instanceof JobU\CliExecutionResult && $r->getType() === JobU\JobType::Cli &&
                $r->get('type') === 'http' && $r->getExitCode() === 0 && $r->getSignal() === 15 &&
                $r->getOutcome() === 'success' && $r->getCaptureLost() === false &&
                $r->getCapturedBytes(JobU\OutputChannel::Stdout) === 0 &&
                $r->getTotalBytes(JobU\OutputChannel::Stdout) === 2 && $r->isTruncated(JobU\OutputChannel::Stdout) === true &&
                $r->getCapturedBytes(JobU\OutputChannel::Stderr) === null &&
                $r->has('null') && $r->get('null') === null && !$r->has('absent') && $r->get('absent') === null &&
                $r->get('future')->empty == new stdClass && $future->list === [] && $future->null === null &&
                $future->uint === '18446744073709551615' && $future->zero === 0 && $future->false === false &&
                $future->text === "a\0b" && $r == $GLOBALS['equal'] && $r !== $GLOBALS['equal'];
        })())PHP"));
        auto absent = jb::php::execution_result_to_php(jb::jobu::JobType::Cli, std::nullopt);
        REQUIRE(absent);
        CHECK(absent.value().view().is_null());
        publish("empty", jb::php::execution_result_to_php(jb::jobu::JobType::Cli, parse("{}")));
        publish("recovery",
                jb::php::execution_result_to_php(
                    jb::jobu::JobType::Cli,
                    parse(R"({"reason":"daemon_interrupted","outcome_unknown":true,"exit_code":999,"signal":-1})")));
        REQUIRE(
            evaluate("$GLOBALS['empty'] instanceof JobU\\ExecutionResult && $GLOBALS['recovery']->getReason() === "
                     "'daemon_interrupted' && $GLOBALS['recovery']->getOutcomeUnknown() === true && "
                     "$GLOBALS['recovery']->getExitCode() === null && $GLOBALS['recovery']->get('exit_code') === 999 "
                     "&& $GLOBALS['recovery']->getSignal() === null && $GLOBALS['empty'] != $GLOBALS['recovery']"));
    }

    SECTION("HTTP optional observations retain false and zero")
    {
        publish(
            "http",
            jb::php::execution_result_to_php(
                jb::jobu::JobType::Http,
                parse(
                    R"({"status":503,"duration_ms":0,"redirects":0,"tls_verified":false,"error_code":"http.failure","error_category":"connect","outcome":"transport_error","retry_after":{"accepted":true,"clamped":false},"body":{"captured_bytes":0,"total_bytes":0,"truncated":false}})")));
        REQUIRE(evaluate(R"PHP((function () {
            $r = $GLOBALS['http'];
            try { $r->getCapturedBytes(JobU\OutputChannel::Stdout); return false; } catch (ValueError) {}
            return $r->getStatus() === 503 && $r->getDuration()->getMilliseconds() === 0 && $r->getRedirects() === 0 &&
                $r->getTlsVerified() === false && $r->getErrorCode() === 'http.failure' && $r->getErrorCategory() === 'connect' &&
                $r->getOutcome() === 'transport_error' && $r->getRetryAfterAccepted() === true && $r->getRetryAfterClamped() === false &&
                $r->getCapturedBytes(JobU\OutputChannel::Body) === 0 && $r->getTotalBytes(JobU\OutputChannel::Body) === 0 &&
                $r->isTruncated(JobU\OutputChannel::Body) === false;
        })())PHP"));
        publish("http",
                jb::php::execution_result_to_php(
                    jb::jobu::JobType::Http,
                    parse(R"({"duration_ms":-1,"status":"200","redirects":4294967296,"tls_verified":1})")));
        REQUIRE(evaluate("$GLOBALS['http']->getStatus() === null && $GLOBALS['http']->getDuration() === null && "
                         "$GLOBALS['http']->getRedirects() === null && $GLOBALS['http']->getTlsVerified() === null"));
    }

    SECTION("Bounds reject malformed native trees without echoing their contents")
    {
        CHECK_FALSE(jb::php::execution_result_to_php(jb::jobu::JobType::Cli, parse("[]")));
        auto large = parse("{}");
        std::get<jb::core::JsonValue::Object>(large.data)
            .emplace("PRIVATE", jb::core::JsonValue{.data = std::string(std::size_t{256} * 1024U, 'x')});
        auto rejected = jb::php::execution_result_to_php(jb::jobu::JobType::Cli, large);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().message.find("PRIVATE") == std::string::npos);
        auto nested = parse("{}");
        for (int i = 0; i < 64; ++i) {
            nested = {.data = jb::core::JsonValue::Object{{"nested", std::move(nested)}}};
        }
        CHECK_FALSE(jb::php::execution_result_to_php(jb::jobu::JobType::Cli, nested));
    }
}

TEST_CASE("PHP output snapshots preserve raw bytes and every availability distinction", "[php][lifecycle][results]")
{
    Runtime runtime;
    auto    publish = [](jb::jobu::AttemptOutputChunk const& chunk) {
        auto result = jb::php::output_chunk_to_php(chunk);
        REQUIRE(result);
        auto slot = result.value().release();
        zend_hash_str_update(&EG(symbol_table), "chunk", 5, &slot);
    };
    auto id = jb::core::Uuid::parse("019c0000-0000-7000-8000-000000000001");
    REQUIRE(id);
    jb::jobu::AttemptOutputChunk chunk{
        .attempt        = {.run_id = id.value(), .attempt_number = 1},
        .channel        = jb::jobu::OutputChannel::Stdout,
        .status         = jb::jobu::OutputStatus::Lost,
        .bytes_returned = 3,
        .retained_bytes = 3,
        .truncated      = true,
        .capture_lost   = true,
        .encoding       = jb::jobu::OutputEncoding::Base64,
        .data           = {std::byte{0xff}, std::byte{0}, std::byte{0xc3}}
    };
    auto wire = jb::jobu::attempt_output_chunk_to_json(chunk);
    REQUIRE(wire);
    auto decoded = jb::jobu::attempt_output_chunk_from_json(wire.value());
    REQUIRE(decoded);
    publish(decoded.value());
    REQUIRE(evaluate(R"PHP((function () {
        $c = $GLOBALS['chunk'];
        try { clone $c; return false; } catch (Error) {}
        try { serialize($c); return false; } catch (Exception) {}
        return $c->getData() === "\xff\0\xc3" && $c->getRunId() === '019c0000-0000-7000-8000-000000000001' &&
            $c->getAttemptNumber() === 1 && $c->getChannel() === JobU\OutputChannel::Stdout &&
            $c->getStatus() === JobU\OutputStatus::Lost && $c->getOffset() === 0 && $c->getBytesReturned() === 3 &&
            $c->getNextOffset() === null && $c->getRetainedBytes() === 3 && $c->getTotalBytes() === null &&
            $c->getOmittedBytes() === null && $c->isTruncated() && $c->isCaptureLost();
    })())PHP"));
    chunk.status         = jb::jobu::OutputStatus::Available;
    chunk.retained_bytes = 10;
    chunk.next_offset    = 3;
    chunk.total_bytes    = 20;
    chunk.omitted_bytes  = 10;
    chunk.capture_lost   = false;
    publish(chunk);
    REQUIRE(evaluate("$GLOBALS['chunk']->getNextOffset() === 3 && $GLOBALS['chunk']->getTotalBytes() === 20 && "
                     "$GLOBALS['chunk']->getOmittedBytes() === 10 && $GLOBALS['chunk']->getRetainedBytes() === 10 && "
                     "$GLOBALS['chunk']->getStatus() === JobU\\OutputStatus::Available"));
    chunk.next_offset.reset();
    chunk.total_bytes.reset();
    chunk.omitted_bytes.reset();

    chunk.data.clear();
    chunk.bytes_returned = 0;
    chunk.retained_bytes = 0;
    chunk.truncated      = false;
    chunk.capture_lost   = false;
    chunk.encoding       = jb::jobu::OutputEncoding::Utf8;
    for (auto status :
         {jb::jobu::OutputStatus::Available, jb::jobu::OutputStatus::Pending, jb::jobu::OutputStatus::NotCaptured}) {
        chunk.status = status;
        publish(chunk);
        REQUIRE(evaluate("$GLOBALS['chunk']->getData() === '' && !$GLOBALS['chunk']->isCaptureLost()"));
    }
    chunk.total_bytes = std::numeric_limits<std::uint64_t>::max();
    CHECK_FALSE(jb::php::output_chunk_to_php(chunk));

    // Zend globals own these native-only snapshots through repeated request teardown.
    for (int i = 0; i < 3; ++i) {
        chunk.total_bytes.reset();
        publish(chunk);
        runtime.restart(nullptr);
    }
}

TEST_CASE("Resource caches preserve edits, attachments and client ownership", "[php][lifecycle][resource]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto*                        previous = &host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(*previous, directory.path());
    {
        Runtime runtime;
        for (int request = 0; request != 2; ++request) {
            jobu_php_test::test_resource_state((directory.path() / "daemon.sock").string(),
                                               (directory.path() / "resource-proxy.sock").string());
            runtime.restart(previous);
        }
    }
    daemon.stop(*previous);
}

TEST_CASE("Production queues preserve snapshots, identity and lifecycle", "[php][lifecycle][queue]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto*                        previous = &host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(*previous, directory.path());
    {
        Runtime runtime;
        for (int request = 0; request != 2; ++request) {
            jobu_php_test::test_queues((directory.path() / "daemon.sock").string(),
                                       (directory.path() / "queue-proxy.sock").string());
            runtime.restart(previous);
        }
    }
    daemon.stop(*previous);
}

TEST_CASE("Prefetched pages preserve position, ownership and continuations", "[php][page]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Runtime                      runtime;
    {
        // Both daemon Process owners belong to the active request loop, so the
        // restart can reap and launch children while the PHP Page remains alive.
        auto anchor = jb::php::current_request()->create_session();
        REQUIRE(anchor);
        auto&  loop = *jb::php::current_request()->loop();
        Daemon original;
        Daemon replacement;
        original.start(loop, directory.path());
        jobu_php_test::test_pages((directory.path() / "daemon.sock").string(),
                                  (directory.path() / "page-proxy.sock").string(),
                                  [&] {
                                      original.stop(loop);
                                      replacement.start(loop, directory.path());
                                  });
        replacement.stop(loop);
    }
    runtime.restart(&host.value()->loop());
}

TEST_CASE("Public definition lists preserve filters, resources and ownership", "[php][list]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto*                        previous = &host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(*previous, directory.path());
    {
        Runtime runtime;
        for (int request = 0; request != 2; ++request) {
            jobu_php_test::test_lists((directory.path() / "daemon.sock").string(),
                                      (directory.path() / "list-proxy.sock").string());
            runtime.restart(previous);
        }
    }
    daemon.stop(*previous);
}

TEST_CASE("Production CLI module lists typed definitions", "[php][list-cli]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto&                        loop = host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(loop, directory.path());

    jb::core::Process                    process;
    jb::core::Object                     receiver;
    std::optional<jb::core::ProcessExit> exit;
    std::string                          errors;
    process.standard_error.connect(&receiver, [&](jb::core::ByteBuffer const& bytes) {
        errors.append(jb::core::as_string_view(bytes));
    });
    process.finished.connect(&receiver, [&](jb::core::ProcessExit const& value) { exit = value; });
    REQUIRE(process.start({
        .executable = JOBU_PHP_BINARY,
        .arguments  = {"-n",
                       "-dextension=" JOBU_PHP_MODULE,
                       JOBU_PHP_LIST_SCRIPT, (directory.path() / "daemon.sock").string()}
    }));
    REQUIRE(loop.run_until(Clock::now() + 10s, [&] { return exit.has_value(); }));
    INFO(errors);
    CHECK(errors.empty());
    CHECK(exit->kind == jb::core::ProcessExitKind::Exited);
    CHECK(exit->exit_code == 0);
    daemon.stop(loop);
}

TEST_CASE("Public history preserves snapshots, typed results and ownership", "[php][history]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto*                        previous = &host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(*previous, directory.path(), /*execute_cli=*/true);
    {
        Runtime runtime;
        for (int request = 0; request != 2; ++request) {
            jobu_php_test::test_history((directory.path() / "daemon.sock").string(),
                                        (directory.path() / "history-proxy.sock").string());
            runtime.restart(previous);
        }
    }
    daemon.stop(*previous);
}

TEST_CASE("Production CLI module reads typed history", "[php][history-cli]")
{
    auto host = jb::core::ScopedEventLoop::create();
    REQUIRE(host);
    auto&                        loop = host.value()->loop();
    jb::test::TemporaryDirectory directory{std::filesystem::perms::owner_all};
    Daemon                       daemon;
    daemon.start(loop, directory.path(), /*execute_cli=*/true);

    jb::core::Process                    process;
    jb::core::Object                     receiver;
    std::optional<jb::core::ProcessExit> exit;
    std::string                          errors;
    process.standard_error.connect(&receiver, [&](jb::core::ByteBuffer const& bytes) {
        errors.append(jb::core::as_string_view(bytes));
    });
    process.finished.connect(&receiver, [&](jb::core::ProcessExit const& value) { exit = value; });
    REQUIRE(process.start({
        .executable = JOBU_PHP_BINARY,
        .arguments  = {"-n",
                       "-dextension=" JOBU_PHP_MODULE,
                       JOBU_PHP_HISTORY_SCRIPT, (directory.path() / "daemon.sock").string()}
    }));
    REQUIRE(loop.run_until(Clock::now() + 10s, [&] { return exit.has_value(); }));
    INFO(errors);
    CHECK(errors.empty());
    CHECK(exit->kind == jb::core::ProcessExitKind::Exited);
    CHECK(exit->exit_code == 0);
    daemon.stop(loop);
}
