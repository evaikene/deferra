#pragma once

#include "command_line_priv.hpp"
#include "control_client.hpp"
#include "object.hpp"
#include "output_priv.hpp"
#include "signal.hpp"

#include <cstdint>
#include <optional>

namespace jb::jobuctl::detail {

/// Render delivers normal CLI output; Capture returns owning data without result/error or byte/file delivery.
enum class SessionMode : std::uint8_t {
    Render,
    Capture,
};

/// Owning terminal outcome. Capture success includes a reply; failures include the existing CLI error projection.
/// Render returns status/error after performing normal delivery, without retaining a reply.
struct SessionResult {
    int                                   exit_code;
    std::optional<jb::jobu::ControlReply> reply;
    std::optional<CliError>               error;
};

/// One owner-thread command session. The borrowed registry must outlive the session.
/// Owns the socket, raw RPC client, typed client, and one overall deadline; construction performs no I/O.
/// Human Render-mode run.get composes attempt queries and preserves partial output on failure.
/// JSON and Capture run.get retain the single requested reply without enrichment.
/// Destroy after signal callback stacks unwind.
class Session final : public jb::core::Object {
public:
    Session(Command                                    command,
            jb::jobu::StandardAttributeRegistry const& registry,
            SessionMode                                mode = SessionMode::Render);
    ~Session() override;

    /// Starts connection once. Connect finished before calling start(); connection may complete synchronously.
    void start();

    /// Moves out the terminal outcome once, returning nullopt before completion or after extraction.
    /// Call only after finished and its callback stacks have unwound, normally after Application::exec().
    [[nodiscard]] auto take_result() -> std::optional<SessionResult>;

    /// Emitted once after terminal state is latched and observation has stopped.
    jb::core::Signal<int> finished;

private:
    struct Private;

    void connected();
    void ready(jb::jobu::SystemInfo const& info);
    void receive_reply(jb::jobu::ControlCallId id, jb::jobu::ControlReply const& reply);
    void receive_failure(jb::jobu::ControlCallId id, jb::jobu::ControlFailure const& failure);
    void receive_run_inspection(jb::jobu::ControlReply const& reply);
    void request_run_inspection();
    void poll_wait();
    void schedule_poll();
    void deadline_expired();
    void deliver_reply(jb::jobu::ControlReply const& reply);
    void finish(int                                   code,
                std::optional<CliError>               error = std::nullopt,
                std::optional<jb::jobu::ControlReply> reply = std::nullopt);
};

} // namespace jb::jobuctl::detail
