#include "lifecycle-bindings.hpp"
#include "request.hpp"
#include "support/temporary_directory.hpp"

#include <jb/core/byte_buffer.hpp>
#include <jb/core/event_loop.hpp>
#include <jb/core/event_loop_types.hpp>
#include <jb/core/object.hpp>
#include <jb/core/process.hpp>
#include <jb/core/scoped_event_loop.hpp>
#include <jb/core/signal.hpp>
#include <jb/net/local_socket.hpp>

#include <Zend/zend_exceptions.h>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>
#include <sapi/embed/php_embed.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono> // IWYU pragma: keep duration literals
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
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
