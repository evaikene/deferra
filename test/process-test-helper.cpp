#include "event_loop.hpp"
#include "json.hpp"
#include "process.hpp"
#include "support/fake_event_loop_backend.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal> // IWYU pragma: keep Provides POSIX signal sets and dispositions.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <crt_externs.h>
#endif

namespace {
auto number(char const* value) noexcept -> int
{
    int        result{-1};
    auto const parsed = std::from_chars(value, value + std::strlen(value), result);
    return parsed.ec == std::errc{} && *parsed.ptr == '\0' ? result : -1;
}

#if defined(__APPLE__)
int atfork_report{-1};
#endif

void atfork_child() noexcept
{
#if defined(__APPLE__)
    // This synthetic host handler deliberately runs inside public fork(), before Process regains control.
    // Report using only an async-signal-safe write; the child cleanup must close this inherited endpoint later.
    if (::write(atfork_report, "x", 1) != 1) {
        ::_exit(88);
    }
#else
    // Linux _Fork() must bypass host handlers entirely.
    ::_exit(88);
#endif
}

auto target_environment() noexcept -> char**
{
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return ::environ;
#endif
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
        if (sigismember(&mask, signal) == 1) {
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

auto inspect_jobu(int argc, char** argv) noexcept -> int
{
    if (argc != 8 || std::strcmp(argv[2], "") != 0 || std::strcmp(argv[3], "-option") != 0) {
        return 49;
    }

    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr || std::strcmp(cwd, argv[4]) != 0) {
        return 48;
    }

    auto const* marker  = ::getenv("PROCESS_MARKER");
    auto const* job_id  = ::getenv("JOBU_JOB_ID");
    auto const* run_id  = ::getenv("JOBU_RUN_ID");
    auto const* attempt = ::getenv("JOBU_ATTEMPT");
    if (marker == nullptr || std::strcmp(marker, "literal $x = value") != 0 || job_id == nullptr ||
        std::strcmp(job_id, argv[5]) != 0 || run_id == nullptr || std::strcmp(run_id, argv[6]) != 0 ||
        attempt == nullptr || std::strcmp(attempt, argv[7]) != 0) {
        return 47;
    }

    std::size_t environment_size{0};
    for (auto const* const* entry = target_environment(); *entry != nullptr; ++entry) {
        ++environment_size;
    }
    return environment_size == 4U ? 0 : 46;
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

auto open_coordination_channel(char const* path, int flags) noexcept -> int
{
    int fd;
    do {
        fd = ::open(path, flags | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

auto wait_for_permission_path(char const* path) noexcept -> bool
{
    auto const fd = open_coordination_channel(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    auto const permitted = wait_permission(fd);
    ::close(fd);
    return permitted;
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

volatile sig_atomic_t term_received{0};

void record_term(int /*signal*/) noexcept
{
    term_received = 1;
}

auto install_term_handler(bool ignore) noexcept -> bool
{
    struct sigaction action{};
    action.sa_handler = ignore ? SIG_IGN : record_term;
    return sigemptyset(&action.sa_mask) == 0 && ::sigaction(SIGTERM, &action, nullptr) == 0;
}

auto write_all(int fd, void const* data, std::size_t size) noexcept -> bool
{
    auto const* bytes   = static_cast<char const*>(data);
    std::size_t written = 0;
    while (written < size) {
        auto const count = ::write(fd, bytes + written, size - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        }
        else if (count < 0 && errno == EINTR) {
            continue;
        }
        else {
            return false;
        }
    }
    return true;
}

// Report the actual target context so daemon tests can compare it with the durable job/run identities.
auto inspect_daemon(int argc, char** argv) -> int
{
    using jb::core::JsonValue;
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
        return 48;
    }
    JsonValue::Array arguments;
    for (int index = 2; index < argc; ++index) {
        arguments.push_back({.data = std::string{argv[index]}});
    }
    JsonValue::Object environment;
    for (auto const* const* entry = target_environment(); *entry != nullptr; ++entry) {
        auto const value     = std::string_view{*entry};
        auto const separator = value.find('=');
        if (separator == std::string_view::npos) {
            return 47;
        }
        environment.emplace(value.substr(0, separator), JsonValue{.data = std::string{value.substr(separator + 1)}});
    }
    char           input{};
    auto const     report = jb::core::serialize_json({
        .data = JsonValue::Object{
                                  {"arguments", {.data = std::move(arguments)}},
                                  {"cwd", {.data = std::string{cwd}}},
                                  {"environment", {.data = std::move(environment)}},
                                  {"stdin_eof", {.data = ::read(STDIN_FILENO, &input, 1) == 0}},
                                  }
    });
    constexpr char diagnostic[]{'e', '\0', 'r', '\n'};
    return report && write_all(STDOUT_FILENO, report->data(), report->size()) &&
                   write_all(STDERR_FILENO, diagnostic, sizeof(diagnostic))
             ? 37
             : 46;
}

// Separate report and release FIFOs prevent the helper from consuming its own readiness acknowledgement.
auto daemon_wait(char const* report_path, char const* release_path) noexcept -> int
{
    // A failed daemon test may lose its runner before it can enforce job.timeout. Bound this helper independently.
    ::alarm(15);
    auto const fd       = open_coordination_channel(report_path, O_WRONLY);
    auto const pid      = ::getpid();
    auto const reported = fd >= 0 && write_all(fd, &pid, sizeof(pid));
    if (fd >= 0) {
        ::close(fd);
    }
    return reported && wait_for_permission_path(release_path) ? 0 : 45;
}

#if defined(__linux__)
auto report_no_new_privileges() noexcept -> int
{
    auto const fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 55;
    }

    std::array<char, std::size_t{16} * 1024> status{};
    std::size_t                              size{0};
    while (size < status.size()) {
        auto const count = ::read(fd, status.data() + size, status.size() - size);
        if (count > 0) {
            size += static_cast<std::size_t>(count);
        }
        else if (count < 0 && errno == EINTR) {
            continue;
        }
        else {
            break;
        }
    }
    ::close(fd);

    auto const contents = std::string_view{status.data(), size};
    auto const label    = contents.find("NoNewPrivs:");
    if (label == std::string_view::npos) {
        return 54;
    }
    auto value = label + std::string_view{"NoNewPrivs:"}.size();
    while (value < contents.size() && (contents[value] == ' ' || contents[value] == '\t')) {
        ++value;
    }
    if (value == contents.size() || contents[value] != '1') {
        return 53;
    }

    constexpr std::string_view observation{"NoNewPrivs: 1\n"};
    return write_all(STDOUT_FILENO, observation.data(), observation.size()) ? 0 : 52;
}
#endif

auto confirm_descriptors_closed(int argc, char** argv) noexcept -> int
{
    for (int index = 2; index < argc; ++index) {
        auto const fd = number(argv[index]);
        errno         = 0;
        if (fd < 0 || ::fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            return 51;
        }
    }
    return argc > 2 ? 0 : 50;
}

auto wait_for_term() noexcept -> bool
{
    while (term_received == 0) {
        if (::pause() < 0 && errno != EINTR) {
            return false;
        }
    }
    return true;
}

auto term_target(bool ignore) noexcept -> int
{
    if (!install_term_handler(ignore) || !write_all(STDOUT_FILENO, "R", 1)) {
        return 69;
    }
    if (ignore) {
        for (;;) {
            ::pause();
        }
    }
    if (!wait_for_term() || !write_all(STDOUT_FILENO, "T", 1)) {
        return 68;
    }
    return 42;
}

enum class DescendantBehavior : std::uint8_t {
    IgnoreTerm,
    HandleTerm,
};

auto descendant_group(char const*        report_path,
                      bool               leader_handles_term,
                      DescendantBehavior descendant_behavior,
                      bool               leader_exits_naturally,
                      unsigned int       watchdog_seconds = 0) noexcept -> int
{
    if (watchdog_seconds != 0) {
        ::alarm(watchdog_seconds);
    }
    auto const report_fd = open_coordination_channel(report_path, O_WRONLY);
    if (report_fd < 0) {
        return 55;
    }
    if (!install_term_handler(!leader_handles_term)) {
        return 67;
    }

    int ready[2];
    if (::pipe(ready) != 0) {
        return 66;
    }
    auto const child = ::fork();
    if (child < 0) {
        return 65;
    }
    if (child == 0) {
        // Alarms are not inherited across fork. Both members must expire even if the daemon disappears.
        if (watchdog_seconds != 0) {
            ::alarm(watchdog_seconds);
        }
        ::close(ready[0]);
        auto const ignore = descendant_behavior == DescendantBehavior::IgnoreTerm;
        if (!install_term_handler(ignore) || !write_all(ready[1], "R", 1)) {
            ::_exit(64);
        }
        ::close(ready[1]);
        if (ignore) {
            for (;;) {
                ::pause();
            }
        }
        if (!wait_for_term() || !write_all(report_fd, "C", 1)) {
            ::_exit(63);
        }
        ::_exit(0);
    }

    ::close(ready[1]);
    auto const child_ready = wait_permission(ready[0]);
    ::close(ready[0]);
    std::array<pid_t, 2> const identities{::getpid(), child};
    if (!child_ready || !write_all(report_fd, identities.data(), sizeof(identities))) {
        return 62;
    }
    if (leader_exits_naturally) {
        return 37;
    }
    if (leader_handles_term) {
        return wait_for_term() ? 42 : 61;
    }
    for (;;) {
        ::pause();
    }
}

auto escaped_writer(char const* report_path, int channel, bool continuous) noexcept -> int
{
    auto const report_fd = open_coordination_channel(report_path, O_WRONLY);
    if (report_fd < 0) {
        return 55;
    }
    int ready[2];
    if (::pipe(ready) != 0) {
        return 60;
    }
    auto const child = ::fork();
    if (child < 0) {
        return 59;
    }
    if (child == 0) {
        ::close(ready[0]);
        if (::setsid() < 0) {
            ::_exit(58);
        }
        ::close(channel == 0 ? STDERR_FILENO : STDOUT_FILENO);
        if (!write_all(ready[1], "R", 1)) {
            ::_exit(57);
        }
        ::close(ready[1]);
        if (continuous) {
            ::_exit(continuous_output(1 << channel));
        }
        for (;;) {
            ::pause();
        }
    }

    ::close(ready[1]);
    auto const child_ready = wait_permission(ready[0]);
    ::close(ready[0]);
    if (!child_ready || !write_all(report_fd, &child, sizeof(child))) {
        return 56;
    }
    return 37;
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
#if defined(__APPLE__)
        int reports[2];
        if (::pipe(reports) != 0) {
            return 97;
        }
        // isolated_launch reserves descriptor 3 itself; keep both report endpoints out of that range.
        for (auto& fd : reports) {
            if (fd <= 3) {
                auto const normalized = ::fcntl(fd, F_DUPFD_CLOEXEC, 4);
                if (normalized < 0) {
                    return 97;
                }
                ::close(fd);
                fd = normalized;
            }
        }
        atfork_report = reports[1];
#endif
        if (::pthread_atfork(nullptr, nullptr, atfork_child) != 0) {
            return 97;
        }
        auto const result = isolated_launch(argv[0], 0, false);
#if defined(__APPLE__)
        ::close(reports[1]);
        char       observation{};
        auto const count = ::read(reports[0], &observation, 1);
        ::close(reports[0]);
        if (count != 1 || observation != 'x') {
            return 97;
        }
#endif
        return result;
    }
    if (mode == "inspect") {
        return inspect(argc, argv);
    }
    if (mode == "inspect-daemon") {
        return inspect_daemon(argc, argv);
    }
    if (mode == "daemon-wait" && argc == 4) {
        return daemon_wait(argv[2], argv[3]);
    }
    if (mode == "inspect-jobu") {
        return inspect_jobu(argc, argv);
    }
    if (mode == "wait" && argc == 3) {
        return wait_for_permission_path(argv[2]) ? 0 : 96;
    }
    if (mode == "descriptors-closed") {
        return confirm_descriptors_closed(argc, argv);
    }
#if defined(__linux__)
    if (mode == "no-new-privileges" && argc == 2) {
        return report_no_new_privileges();
    }
#endif
    if (mode == "output" && argc == 4 && number(argv[2]) >= 0 && number(argv[3]) >= 0) {
        return output({static_cast<std::size_t>(number(argv[2])), static_cast<std::size_t>(number(argv[3]))});
    }
    if (mode == "continuous" && argc == 3 && number(argv[2]) >= 1 && number(argv[2]) <= 3) {
        return continuous_output(number(argv[2]));
    }
    if (mode == "term" && argc == 3 && (number(argv[2]) == 0 || number(argv[2]) == 1)) {
        return term_target(number(argv[2]) != 0);
    }
    if (mode == "group" && argc == 5) {
        auto const behavior = number(argv[3]) == 0 ? DescendantBehavior::HandleTerm : DescendantBehavior::IgnoreTerm;
        return descendant_group(argv[2], true, behavior, number(argv[4]) != 0);
    }
    if (mode == "group-wait" && argc == 3) {
        return descendant_group(argv[2], false, DescendantBehavior::IgnoreTerm, false);
    }
    if (mode == "daemon-group-wait" && argc == 3) {
        // The daemon's five-second timeout must win; SIGALRM is only failed-fixture cleanup, never success evidence.
        return descendant_group(argv[2], false, DescendantBehavior::IgnoreTerm, false, 15);
    }
    if (mode == "group-exit" && argc == 3) {
        return descendant_group(argv[2], false, DescendantBehavior::IgnoreTerm, true);
    }
    if (mode == "escape" && argc == 5 && (number(argv[3]) == 0 || number(argv[3]) == 1)) {
        return escaped_writer(argv[2], number(argv[3]), number(argv[4]) != 0);
    }
    if (mode == "early" && argc == 4 && (number(argv[2]) == 0 || number(argv[2]) == 1)) {
        auto const closed_channel = number(argv[2]);
        auto const open_channel   = 1 - closed_channel;
        ::close(closed_channel + 1);
        if (write_pattern(open_channel + 1, 0, 1, static_cast<std::size_t>(open_channel)) != 1 ||
            !wait_for_permission_path(argv[3])) {
            return 71;
        }
        return write_pattern(open_channel + 1, 1, 4096, static_cast<std::size_t>(open_channel)) == 4096 ? 37 : 70;
    }
    return 95;
}
