#pragma once

#include "command_line_priv.hpp"
#include "control_client.hpp"
#include "object.hpp"
#include "output_priv.hpp"
#include "signal.hpp"

#include <optional>

namespace jb::jobuctl::detail {

/// One owner-thread command session. The borrowed registry must outlive the session.
/// Owns the socket, raw RPC client, typed client, and one overall deadline; construction performs no I/O.
/// Destroy after signal callback stacks unwind.
class Session final : public jb::core::Object {
public:
    Session(Command command, jb::jobu::StandardAttributeRegistry const& registry);
    ~Session() override;

    /// Starts connection once. Connect finished before calling start(); connection may complete synchronously.
    void start();

    /// Emitted once after terminal state is latched and observation has stopped.
    jb::core::Signal<int> finished;

private:
    struct Private;

    void connected();
    void ready(jb::jobu::SystemInfo const& info);
    void receive_reply(jb::jobu::ControlCallId id, jb::jobu::ControlReply const& reply);
    void receive_failure(jb::jobu::ControlCallId id, jb::jobu::ControlFailure const& failure);
    void poll_wait();
    void schedule_poll();
    void deadline_expired();
    void finish(int code, std::optional<CliError> error = std::nullopt);
};

} // namespace jb::jobuctl::detail
