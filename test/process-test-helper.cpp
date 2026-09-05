#include "event_loop.hpp"
#include "process.hpp"
#include "support/fake_event_loop_backend.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal> // IWYU pragma: keep Provides POSIX signal sets and dispositions.
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
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
    jb::core::Process  process;
    int                result{90};
    bool               finished{false};
    int                started{0};
    std::array<int, 2> output_bytes{};
    auto               output_connection   = process.standard_output.connect([&](jb::core::ByteBuffer const& chunk) {
        output_bytes[0] += chunk == jb::core::ByteBuffer{std::byte{'x'}} ? 1 : 100;
    });
    auto               error_connection    = process.standard_error.connect([&](jb::core::ByteBuffer const& chunk) {
        output_bytes[1] += chunk == jb::core::ByteBuffer{std::byte{'x'}} ? 1 : 100;
    });
    auto               started_connection  = process.started.connect([&] { ++started; });
    auto               finished_connection = process.finished.connect([&](jb::core::ProcessExit const& exit) {
        finished = true;
        if (missing) {
            result = exit.start_error && started == 0 ? 0 : 91;
        }
        else {
            result = exit.exit_code == 37 && started == 1 && output_bytes == std::array{1, 1} ? 0 : 92;
        }
    });
    auto               accepted =
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
    output_connection.disconnect();
    error_connection.disconnect();
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

auto wait_permission(int fd) noexcept -> bool
{
    char    permission{};
    ssize_t count;
    do {
        count = ::read(fd, &permission, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

// Each stream has its own position-dependent pattern, including NUL. Partial writes retain that position.
auto write_pattern(int fd, std::size_t offset, std::size_t size, std::size_t channel) noexcept -> ssize_t
{
    std::array<unsigned char, 4096> bytes{};
    size = std::min(size, bytes.size());
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>((offset + i + (channel * 73)) % 251);
    }
    ssize_t count;
    do {
        count = ::write(fd, bytes.data(), size);
    } while (count < 0 && errno == EINTR);
    return count;
}

auto output(std::array<std::size_t, 2> sizes) noexcept -> int
{
    std::array<std::size_t, 2> positions{};
    while (positions != sizes) {
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (positions[i] == sizes[i]) {
                continue;
            }
            auto const count = write_pattern(static_cast<int>(i) + 1, positions[i], sizes[i] - positions[i], i);
            if (count <= 0) {
                return 75;
            }
            positions[i] += static_cast<std::size_t>(count);
        }
    }
    return 37;
}

auto continuous_output(int mask) noexcept -> int
{
    // A single helper can keep either/both streams supplied without blocking one stream behind the other's writer.
    // These target-local nonblocking flags do not alter the parent's reader file descriptions.
    std::array<pollfd, 2>      polls{};
    std::array<std::size_t, 2> positions{};
    for (std::size_t i = 0; i < polls.size(); ++i) {
        auto const fd = static_cast<int>(i) + 1;
        polls[i]      = {.fd = (mask & (1 << i)) != 0 ? fd : -1, .events = POLLOUT, .revents = 0};
        if (polls[i].fd >= 0 && ::fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
            return 74;
        }
    }
    for (;;) {
        auto const ready = ::poll(polls.data(), polls.size(), -1);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return 73;
        }
        for (std::size_t i = 0; i < polls.size(); ++i) {
            if ((polls[i].revents & POLLOUT) == 0) {
                continue;
            }
            auto const count = write_pattern(polls[i].fd, positions[i], 4096, i);
            if (count > 0) {
                positions[i] += static_cast<std::size_t>(count);
            }
            else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return 72;
            }
        }
    }
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
            (::fcntl(2, F_GETFL) & O_ACCMODE) != O_WRONLY || (::fcntl(1, F_GETFL) & O_NONBLOCK) != 0 ||
            (::fcntl(2, F_GETFL) & O_NONBLOCK) != 0 || ::read(0, &byte, 1) != 0 || ::write(1, "x", 1) != 1 ||
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
        return wait_permission(number(argv[2])) ? 0 : 96;
    }
    if (mode == "output" && argc == 4 && number(argv[2]) >= 0 && number(argv[3]) >= 0) {
        return output({static_cast<std::size_t>(number(argv[2])), static_cast<std::size_t>(number(argv[3]))});
    }
    if (mode == "continuous" && argc == 3 && number(argv[2]) >= 1 && number(argv[2]) <= 3) {
        return continuous_output(number(argv[2]));
    }
    if (mode == "early" && argc == 4 && (number(argv[2]) == 0 || number(argv[2]) == 1)) {
        auto const closed_channel = number(argv[2]);
        auto const open_channel   = 1 - closed_channel;
        ::close(closed_channel + 1);
        if (write_pattern(open_channel + 1, 0, 1, static_cast<std::size_t>(open_channel)) != 1 ||
            !wait_permission(number(argv[3]))) {
            return 71;
        }
        return write_pattern(open_channel + 1, 1, 4096, static_cast<std::size_t>(open_channel)) == 4096 ? 37 : 70;
    }
    return 95;
}
