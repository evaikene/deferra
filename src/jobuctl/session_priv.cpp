#include "session_priv.hpp"

#include "client.hpp"
#include "commands/commands_priv.hpp"
#include "local_socket.hpp"
#include "logging.hpp"
#include "object_priv.hpp"
#include "output_priv.hpp"
#include "protocol.hpp"
#include "system_info.hpp"
#include "timer.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::net;
using namespace jb::rpc;

namespace {

enum class SessionPhase : std::uint8_t {
    Idle,
    Connecting,
    SystemInfo,
    Command,
    Finished
};

} // namespace

struct Session::Private : jb::core::priv::ObjectPrivate {
    Private(Command value, StandardAttributeRegistry const& attributes)
        : command{std::move(value)}
        , registry{attributes}
    {}

    Command                          command;
    StandardAttributeRegistry const& registry;

    // Reverse destruction stops the timer and releases the client before its borrowed socket.
    // These value/unique-owned Objects have no parent, avoiding a second ownership path.
    LocalSocket             socket;
    std::unique_ptr<Client> client;
    Timer                   timeout;
    SessionPhase            phase{SessionPhase::Idle};
};

Session::Session(Command command, StandardAttributeRegistry const& registry)
    : Object{
          *new Private{std::move(command), registry}
}
{
    // Install receiver-aware connections only after Object owns the complete private block.
    auto* data = d_ptr<Private>();
    data->socket.set_read_buffer_limit(std::size_t{2} * 1024U * 1024U);
    data->socket.error_occurred.connect(this, [this](IOError, std::string const& message) {
        finish_operator_error(message);
    });
    data->socket.connected.connect(this, [this] { connected(); });
    data->timeout.timeout.connect(this, [this] {
        auto*      state = d_ptr<Private>();
        auto const method =
            state->phase == SessionPhase::Command ? state->command.method : std::string_view{"system.info"};
        finish_operator_error(fmt::format("{} request timed out", method));
    });
}

Session::~Session()
{
    // Quiesce event sources while the derived object is intact. No completion is emitted during teardown.
    auto* data  = d_ptr<Private>();
    data->phase = SessionPhase::Finished;
    data->timeout.stop();
    data->client.reset();
}

void Session::start()
{
    auto* data = d_ptr<Private>();
    if (data->phase != SessionPhase::Idle) {
        return;
    }

    data->phase = SessionPhase::Connecting;
    data->timeout.start(std::chrono::seconds{5});
    data->socket.connect_to_server(data->command.socket_path);
}

void Session::finish(int code)
{
    auto* data = d_ptr<Private>();
    if (data->phase == SessionPhase::Finished) {
        return;
    }

    // Latch completion before notifying the application; retained callbacks become harmless.
    data->phase = SessionPhase::Finished;
    data->timeout.stop();
    emit(finished, code);
}

void Session::finish_operator_error(std::string_view message)
{
    if (d_ptr<Private>()->phase == SessionPhase::Finished) {
        return;
    }
    print_operator_error(message);
    finish(EXIT_FAILURE);
}

void Session::finish_call_error(std::string_view method, Error const& error)
{
    if (d_ptr<Private>()->phase == SessionPhase::Finished) {
        return;
    }
    log_error("Unable to send the {} request: {} ({})", method, error.message, error.code);
    finish(EXIT_FAILURE);
}

void Session::connected()
{
    auto* data = d_ptr<Private>();
    if (data->phase != SessionPhase::Connecting) {
        return;
    }

    // Establish every observer before call(): the raw client may deliver a response synchronously.
    data->client = std::make_unique<Client>(data->socket);
    data->client->result_received.connect(this,
                                          [this](RequestId const&, JsonValue const& value) { receive_result(value); });
    data->client->error_received.connect(this, [this](RequestId const&, RpcError const& error) {
        if (d_ptr<Private>()->phase == SessionPhase::Finished) {
            return;
        }
        print_remote_error(error);
        finish(EXIT_FAILURE);
    });
    data->client->request_failed.connect(this, [this](RequestId const&, Error const& error) {
        finish_operator_error(error.message);
    });
    data->client->protocol_error.connect(this, [this](Error const& error) {
        if (d_ptr<Private>()->phase == SessionPhase::Finished) {
            return;
        }
        log_error("RPC protocol error: {} ({})", error.message, error.code);
        finish(EXIT_FAILURE);
    });

    data->phase = SessionPhase::SystemInfo;
    auto call   = data->client->call("system.info");
    if (!call) {
        finish_call_error("system.info", call.error());
    }
}

void Session::receive_result(JsonValue const& value)
{
    auto* data = d_ptr<Private>();
    if (data->phase == SessionPhase::Finished) {
        return;
    }

    // The information command displays the handshake itself. Other commands require compatible capabilities.
    if (data->phase == SessionPhase::SystemInfo) {
        auto info = system_info_from_json(value);
        if (!info) {
            log_error("Invalid system.info response: {} ({})", info.error().message, info.error().code);
            finish(EXIT_FAILURE);
            return;
        }
        if (data->command.kind == CommandKind::SystemInfo) {
            print_system_info(info.value());
            finish(EXIT_SUCCESS);
            return;
        }
        if (info->api_version.major != 1U) {
            finish_operator_error("the daemon uses an incompatible API major version");
            return;
        }
        if (std::find(info->capabilities.begin(), info->capabilities.end(), data->command.method) ==
            info->capabilities.end()) {
            finish_operator_error(fmt::format("the daemon does not advertise {}", data->command.method));
            return;
        }

        // Keep the original two-call protocol and set the phase before possible synchronous delivery.
        data->phase = SessionPhase::Command;
        auto call   = data->client->call(data->command.method, data->command.params);
        if (!call) {
            finish_call_error(data->command.method, call.error());
        }
        return;
    }

    if (data->phase != SessionPhase::Command) {
        log_error("Received an RPC result in an invalid command-session phase");
        finish(EXIT_FAILURE);
        return;
    }
    if (!print_command_result(data->command, value, data->registry)) {
        finish(EXIT_FAILURE);
        return;
    }
    finish(EXIT_SUCCESS);
}

} // namespace jb::jobuctl::detail
