#pragma once

#include "command_line_priv.hpp"
#include "error.hpp"
#include "object.hpp"
#include "signal.hpp"

#include <string_view>

namespace jb::jobuctl::detail {

/// One owner-thread command session. The borrowed registry must outlive the session.
/// Owns socket, raw RPC client and deadline; construction performs no I/O.
/// Destroy only after callback stacks unwind. No typed-client or retry policy is added here.
class Session final : public jb::core::Object {
public:
    Session(Command command, jb::jobu::StandardAttributeRegistry const& registry);
    ~Session() override;

    /// Starts connection and the existing five-second overall deadline once.
    /// Completion can occur synchronously; connect finished before calling start().
    void start();

    /// Emitted once after terminal state is latched and the deadline is stopped.
    jb::core::Signal<int> finished;

private:
    struct Private;

    void connected();
    void receive_result(jb::core::JsonValue const& value);
    void finish(int code);
    void finish_operator_error(std::string_view message);
    void finish_call_error(std::string_view method, jb::core::Error const& error);
};

} // namespace jb::jobuctl::detail
