#include "application.hpp"
#include "local_socket.hpp"

#include <chrono>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <unistd.h>

auto main(int argc, char* argv[]) -> int
{
    if (argc != 3) {
        return 2;
    }
    // This executable runs only in provisioned privileged tests. Copy NSS fields before initgroups;
    // the parent test process never changes identity, and no worker exists during the transition.
    auto const* account = ::getpwnam(argv[1]);
    if (account == nullptr || account->pw_uid == 0) {
        return 2;
    }
    auto const user  = account->pw_uid;
    auto const group = account->pw_gid;
    if (::initgroups(argv[1], group) != 0 || ::setgid(group) != 0 || ::setuid(user) != 0) {
        return 2;
    }

    jb::core::Application app{0, nullptr};
    jb::net::LocalSocket  socket;
    std::string           response;
    socket.connected.connect(&app, [&] {
        // A system.info round trip establishes actual management access through the filesystem group.
        static_cast<void>(
            socket.write("Content-Length: 47\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"system.info\"}"));
    });
    socket.ready_read.connect(&app, [&] { response.append(socket.read_all()); });
    socket.connect_to_server(argv[2]);
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (response.find("capabilities") == std::string::npos && std::chrono::steady_clock::now() < deadline &&
           socket.is_open()) {
        if (app.process_events(jb::core::EventFlag::All, 20) == jb::core::ProcessEventsResult::Failed) {
            return 2;
        }
    }
    return response.find("capabilities") != std::string::npos ? 0 : 1;
}
