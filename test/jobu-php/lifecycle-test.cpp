#include "client.hpp"
#include "lifecycle-bindings.hpp"
#include "operation.hpp"
#include "recovery-proxy.hpp"
#include "request.hpp"
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
    auto start(EventLoop& loop, std::filesystem::path const& directory) -> void
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
