#include "lifecycle-bindings.hpp"

#include "request.hpp"

#include <jb/core/event_loop.hpp>
#include <jb/core/event_loop_types.hpp>
#include <jb/core/io_device.hpp>
#include <jb/core/object.hpp>
#include <jb/core/signal.hpp>
#include <jb/jobu/client/control_client.hpp>
#include <jb/jobu/system_info.hpp>
#include <jb/net/local_socket.hpp>

#include <phpcxx/arguments.hpp>
#include <phpcxx/error.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/value.hpp>

#include <chrono> // IWYU pragma: keep duration literals
#include <memory>
#include <string>
#include <utility>

#include "lifecycle_arginfo.h"

namespace jobu_php_test {
namespace {

using namespace std::chrono_literals;
int live_count = 0;

// Fixture-only synchronous driving. Production Session handles ownership only;
// this does not define the future public constructor, deadlines or RPC bridge.
class Client {
public:
    Client() noexcept { ++live_count; }

    ~Client() noexcept
    {
        _session.reset();
        --live_count;
    }

    Client(Client const&)                    = delete;
    auto operator=(Client const&) -> Client& = delete;

    auto trace([[maybe_unused]] php::GcVisitor& visitor) const noexcept -> void {}

    auto connect(std::string const& endpoint) -> bool
    {
        auto* request = jb::php::current_request();
        if (!request) {
            return false;
        }
        auto created = request->create_session();
        if (!created) {
            return false;
        }
        _session = std::move(created).value();

        auto& socket   = _session->socket();
        auto* receiver = &_session->receiver();
        socket.connected.connect(receiver, [this] { _connected = true; });
        socket.error_occurred.connect(receiver, [this](jb::core::IOError, std::string const&) { _failed = true; });
        socket.connect_to_server(endpoint);
        auto& loop = *request->loop();
        if (!loop.run_until(jb::core::Clock::now() + 3s, [this] { return _connected || _failed; }) || _failed ||
            !_session->attach_clients()) {
            return false;
        }

        auto& typed = _session->typed();
        typed.ready.connect(receiver, [this](jb::jobu::SystemInfo const&) { _ready = true; });
        typed.failed.connect(receiver, [this](jb::core::Error const&) { _failed = true; });
        typed.reply_received.connect(receiver,
                                     [this](jb::jobu::ControlCallId, jb::jobu::ControlReply const&) { ++_replies; });
        typed.call_failed.connect(receiver,
                                  [this](jb::jobu::ControlCallId, jb::jobu::ControlFailure const&) { _failed = true; });
        return typed.initialize({.timeout = 3s}) &&
               loop.run_until(jb::core::Clock::now() + 3s, [this] { return _ready || _failed; }) && !_failed;
    }

    auto close() noexcept -> void
    {
        if (_session) {
            _session->close();
        }
    }

    auto ping() -> bool
    {
        if (!_session || _session->is_closed()) {
            return false;
        }
        auto const before = _replies;
        return _session->typed().get_system_info({.timeout = 3s}) &&
               jb::php::current_request()->loop()->run_until(
                   jb::core::Clock::now() + 3s,
                   [this, before] { return _replies != before || _failed; }) &&
               !_failed;
    }

    auto pending() -> bool
    {
        if (!_session || _session->is_closed() || !_session->typed().get_system_info({.timeout = 3s})) {
            return false;
        }
        // Leave both an accepted call/timer and a deferred delete outstanding.
        // Session cleanup must destroy the receiver synchronously and invalidate
        // the queued deletion; it must not pump callbacks into this native value.
        _session->receiver().delete_later();
        return true;
    }

private:
    bool                              _connected = false;
    bool                              _ready     = false;
    bool                              _failed    = false;
    int                               _replies   = 0;
    std::unique_ptr<jb::php::Session> _session;
};

class Holder {
public:
    explicit Holder(php::Value client) noexcept
        : _client(std::move(client))
    {}

    auto trace(php::GcVisitor& visitor) const noexcept -> void { visitor.add(_client.view()); }

    auto client() const noexcept -> php::ValueView { return _client.view(); }

private:
    php::Value _client;
};

using ClientBinding = php::ObjectBinding<Client>;
using HolderBinding = php::ObjectBinding<Holder>;

auto client_this(zend_execute_data* execute_data) -> Client*
{
    auto native = ClientBinding::get(Z_OBJ(execute_data->This));
    if (!native) {
        php::raise(native.error());
        return nullptr;
    }
    return native.value();
}

auto construct_client(zend_execute_data* execute_data) noexcept -> void
{
    php::Arguments arguments(execute_data);
    auto           endpoint = arguments.string(1);
    auto           fail     = arguments.get(2).get() ? arguments.boolean(2) : php::Result<bool>(false);
    if (!endpoint || !fail) {
        php::raise(!endpoint ? endpoint.error() : fail.error());
        return;
    }
    auto native = ClientBinding::construct(Z_OBJ(execute_data->This));
    if (!native) {
        php::raise(native.error());
        return;
    }
    if (!native.value()->connect(std::string(endpoint.value().bytes())) || fail.value()) {
        php::raise({.kind = php::ErrorKind::Runtime, .message = "Fixture connection or partial initialization failed"});
    }
}

} // namespace

auto register_classes() noexcept -> zend_result
{
    auto* client = register_class_JobUTestClient();
    auto* holder = register_class_JobUTestHolder();
    return ClientBinding::bind(client) && HolderBinding::bind(holder) ? SUCCESS : FAILURE;
}

auto live_clients() noexcept -> int
{
    return live_count;
}
} // namespace jobu_php_test

ZEND_METHOD(JobUTestClient, __construct)
{
    (void)return_value;
    jobu_php_test::construct_client(execute_data);
}

ZEND_METHOD(JobUTestClient, close)
{
    (void)return_value;
    [&]() noexcept {
        if (auto* native = jobu_php_test::client_this(execute_data)) {
            native->close();
        }
    }();
}

ZEND_METHOD(JobUTestClient, ping)
{
    [&]() noexcept {
        if (auto* native = jobu_php_test::client_this(execute_data)) {
            ZVAL_BOOL(return_value, native->ping());
        }
    }();
}

ZEND_METHOD(JobUTestClient, pending)
{
    (void)return_value;
    [&]() noexcept {
        if (auto* native = jobu_php_test::client_this(execute_data); native && !native->pending()) {
            php::raise({.kind = php::ErrorKind::Runtime, .message = "Fixture session is closed"});
        }
    }();
}

ZEND_METHOD(JobUTestClient, live)
{
    (void)execute_data;
    ZVAL_LONG(return_value, jobu_php_test::live_clients());
}

ZEND_METHOD(JobUTestHolder, __construct)
{
    (void)return_value;
    [&]() noexcept {
        auto value = php::Arguments(execute_data).get(1).dereference();
        if (!value.is_object() || !jobu_php_test::ClientBinding::get(Z_OBJ_P(value.get()))) {
            php::raise({.kind = php::ErrorKind::Type, .message = "Expected an initialized fixture client"});
            return;
        }
        auto native = jobu_php_test::HolderBinding::construct(Z_OBJ(execute_data->This), php::Value::retain(value));
        if (!native) {
            php::raise(native.error());
        }
    }();
}

ZEND_METHOD(JobUTestHolder, client)
{
    [&]() noexcept {
        auto native = jobu_php_test::HolderBinding::get(Z_OBJ(execute_data->This));
        if (!native) {
            php::raise(native.error());
            return;
        }
        ZVAL_COPY(return_value, native.value()->client().get());
    }();
}
