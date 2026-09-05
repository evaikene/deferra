#include "event_loop.hpp"
#include "process.hpp"
#include "support/fake_event_loop_backend.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal> // IWYU pragma: keep Provides POSIX signal sets and dispositions.
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

namespace {
auto number(char const* value) noexcept -> int
{
    int        result{-1};
    auto const parsed = std::from_chars(value, value + std::strlen(value), result);
    return parsed.ec == std::errc{} && *parsed.ptr == '\0' ? result : -1;
}

void atfork_child() noexcept
{
    ::_exit(88);
}

auto isolated_launch(char const* executable, int closed, bool missing) -> int
{
    // Reserve 3 so the loop's private descriptors stay above 3. Close the selected descriptors
    // only after loop construction, immediately before Process creates its own resources.
    auto const reserved = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (reserved != 3) {
        if (reserved >= 0) {
            ::close(reserved);
        }
        return 94;
    }
    jb::core::EventLoop                    loop;
    jb::core::priv::ScopedCurrentEventLoop current{&loop};
    for (int fd = 0; fd <= 3; ++fd) {
        if ((closed & (1 << fd)) != 0) {
            ::close(fd);
        }
    }
    jb::core::Process process;
    int               result{90};
    bool              finished{false};
    int               started{0};
    auto              started_connection  = process.started.connect([&] { ++started; });
    auto              finished_connection = process.finished.connect([&](jb::core::ProcessExit const& exit) {
        finished = true;
        if (missing) {
            result = exit.start_error && started == 0 ? 0 : 91;
        }
        else {
            result = exit.exit_code == 37 && started == 1 ? 0 : 92;
        }
    });
    auto              accepted =
        process.start({.executable = missing ? "/no-such-process-helper" : executable, .arguments = {"stdio"}});
    if (!accepted) {
        return 93;
    }
    auto const deadline = jb::core::Clock::now() + std::chrono::seconds{3};
    while (!finished && jb::core::Clock::now() < deadline) {
        static_cast<void>(loop.process_events(jb::core::EventFlag::All, 10));
    }
    started_connection.disconnect();
    finished_connection.disconnect();
    if ((closed & 8) == 0) {
        ::close(reserved);
    }
    return result;
}

auto inspect(int argc, char** argv) noexcept -> int
{
    if (argc != 5 || std::strcmp(argv[2], "") != 0 || std::strcmp(argv[3], "-option") != 0) {
        return 80;
    }
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr || std::strcmp(cwd, argv[4]) != 0) {
        return 81;
    }
    auto const* marker = ::getenv("PROCESS_MARKER");
    if (!marker || std::strcmp(marker, "literal $x = value") != 0 || ::getenv("HOME") || ::getenv("PATH")) {
        return 82;
    }
    sigset_t mask{};
    if (::sigprocmask(SIG_SETMASK, nullptr, &mask) != 0) {
        return 83;
    }
    for (int signal = 1; signal < NSIG; ++signal) {
        if (::sigismember(&mask, signal) == 1) {
            return 84;
        }
        if (signal != SIGKILL && signal != SIGSTOP) {
            struct sigaction action{};
            if (::sigaction(signal, nullptr, &action) == 0 && action.sa_handler != SIG_DFL) {
                return 85;
            }
        }
    }
    return 0;
}
} // namespace

auto main(int argc, char** argv) -> int
{
    if (argc < 2) {
        return 99;
    }
    std::string_view const mode{argv[1]};
    if (mode == "exit" && argc == 3) {
        return number(argv[2]);
    }
    if (mode == "signal" && argc == 3) {
        ::raise(number(argv[2]));
        return 98;
    }
    if (mode == "closed" && argc == 4) {
        return isolated_launch(argv[0], number(argv[2]), number(argv[3]) != 0);
    }
    if (mode == "stdio") {
        char byte{};
        if ((::fcntl(0, F_GETFL) & O_ACCMODE) != O_RDONLY || (::fcntl(1, F_GETFL) & O_ACCMODE) != O_WRONLY ||
            (::fcntl(2, F_GETFL) & O_ACCMODE) != O_WRONLY || ::read(0, &byte, 1) != 0 || ::write(1, "x", 1) != 1 ||
            ::write(2, "x", 1) != 1 || ::fcntl(3, F_GETFD) != -1 || errno != EBADF) {
            return 89;
        }
        return 37;
    }
    if (mode == "marker" && argc == 3) {
        auto const fd = ::open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            return 87;
        }
        auto const written = ::write(fd, "executed", 8);
        ::close(fd);
        return written == 8 ? 37 : 86;
    }
    if (mode == "atfork") {
        if (::pthread_atfork(nullptr, nullptr, atfork_child) != 0) {
            return 97;
        }
        return isolated_launch(argv[0], 0, false);
    }
    if (mode == "inspect") {
        return inspect(argc, argv);
    }
    if (mode == "wait" && argc == 3) {
        char    permission{};
        ssize_t count;
        do {
            count = ::read(number(argv[2]), &permission, 1);
        } while (count < 0 && errno == EINTR);
        return count == 1 ? 0 : 96;
    }
    return 95;
}
