#include "json.hpp"
#include "uuid.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio> // IWYU pragma: keep stderr for test diagnostics
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using jb::core::JsonValue;

// One helper invocation owns one daemon and its short-lived client children.
// Normal checks require graceful exit; destructor cleanup never counts as success.
class ChildProcess {
public:
    ChildProcess() = default;

    ~ChildProcess()
    {
        if (_pid < 0) {
            return;
        }
        static_cast<void>(::kill(_pid, SIGTERM));
        if (wait_for(2s)) {
            return;
        }

        if (_pid >= 0) {
            static_cast<void>(::kill(_pid, SIGKILL));
            auto status = 0;
            while (::waitpid(_pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    ChildProcess(ChildProcess const&)                    = delete;
    auto operator=(ChildProcess const&) -> ChildProcess& = delete;

    auto start(std::filesystem::path const&            executable,
               std::initializer_list<std::string_view> arguments,
               std::filesystem::path const&            output,
               std::filesystem::path const&            error) -> bool
    {
        // Prepare all owning argument storage before fork; the child only uses
        // POSIX descriptor/exec operations and never allocates or logs in C++.
        auto storage = std::vector<std::string>{executable.string()};
        for (auto argument : arguments) {
            storage.emplace_back(argument);
        }
        auto argv = std::vector<char*>{};
        for (auto& argument : storage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        auto const output_fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (output_fd < 0) {
            return false;
        }
        auto const error_fd = ::open(error.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (error_fd < 0) {
            static_cast<void>(::close(output_fd));
            return false;
        }

        _pid = ::fork();
        if (_pid == 0) {
            if (::dup2(output_fd, STDOUT_FILENO) < 0 || ::dup2(error_fd, STDERR_FILENO) < 0) {
                ::_exit(127);
            }
            static_cast<void>(::close(output_fd));
            static_cast<void>(::close(error_fd));
            ::execv(executable.c_str(), argv.data());
            ::_exit(127);
        }
        static_cast<void>(::close(output_fd));
        static_cast<void>(::close(error_fd));
        return _pid > 0;
    }

    auto poll() -> std::optional<int>
    {
        if (_status || _pid < 0) {
            return _status;
        }
        auto status = 0;
        auto result = pid_t{};
        do {
            result = ::waitpid(_pid, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);

        if (result == _pid) {
            _status = status;
            _pid    = -1;
        }
        else if (result < 0) {
            // A lost child cannot supply acceptance evidence. Do not later send
            // a signal to a PID whose ownership waitpid could not establish.
            _pid = -1;
        }
        return _status;
    }

    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<int>
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto status = poll()) {
                return status;
            }
            if (_pid < 0) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(10ms);
        }
        return std::nullopt;
    }

    auto request_stop() const -> bool { return _pid > 0 && ::kill(_pid, SIGTERM) == 0; }

private:
    pid_t              _pid{-1};
    std::optional<int> _status;
};

auto read_file(std::filesystem::path const& path) -> std::optional<std::string>
{
    auto input = std::ifstream{path};
    if (!input) {
        return std::nullopt;
    }
    auto text = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        return std::nullopt;
    }
    return text;
}

auto successful(std::optional<int> status) -> bool
{
    return status && WIFEXITED(*status) && WEXITSTATUS(*status) == EXIT_SUCCESS;
}

auto string_field(JsonValue const& value, std::string_view name) -> std::optional<std::string_view>
{
    if (!value.is_object()) {
        return std::nullopt;
    }
    auto const& object = value.as_object();
    auto const  found  = object.find(name);
    if (found == object.end() || !found->second.is_string()) {
        return std::nullopt;
    }
    return found->second.as_string();
}

auto read_json(std::filesystem::path const& path) -> std::optional<JsonValue>
{
    auto text = read_file(path);
    if (!text) {
        return std::nullopt;
    }
    auto parsed = jb::core::parse_json(*text);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(parsed).value();
}

auto expected_system_info(JsonValue const& info, std::string_view version) -> bool
{
    if (string_field(info, "daemon_version") != version) {
        return false;
    }
    auto const& object       = info.as_object();
    auto const  api          = object.find("api_version");
    auto const  expected_api = JsonValue{
        .data = JsonValue::Object{
                                  {"major", JsonValue{.data = std::uint64_t{1}}},
                                  {"minor", JsonValue{.data = std::uint64_t{5}}},
                                  }
    };
    return api != object.end() && api->second == expected_api;
}

auto run_client(std::filesystem::path const&            prefix,
                std::filesystem::path const&            logs,
                std::string_view                        label,
                std::initializer_list<std::string_view> arguments) -> std::optional<JsonValue>
{
    auto const   output = logs / fmt::format("{}.stdout", label);
    auto const   error  = logs / fmt::format("{}.stderr", label);
    ChildProcess client;
    if (!client.start(prefix / "bin/jobuctl", arguments, output, error) || !successful(client.wait_for(2s))) {
        return std::nullopt;
    }
    return read_json(output);
}

auto wait_until_ready(ChildProcess&                daemon,
                      std::filesystem::path const& prefix,
                      std::filesystem::path const& logs,
                      std::string_view             phase,
                      std::string_view             version) -> bool
{
    // A socket entry alone is not readiness. Each probe completes an installed
    // client's handshake and system.info using its compiled default endpoint.
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    auto       probe    = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (daemon.poll()) {
            return false;
        }
        auto info = run_client(prefix,
                               logs,
                               fmt::format("{}-ready-{}", phase, ++probe),
                               {"--timeout", "250", "--json", "system", "info"});
        if (info) {
            return expected_system_info(*info, version);
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

auto check_metadata(std::filesystem::path const& path, mode_t type, mode_t mode) -> bool
{
    struct stat metadata{};
    if (::lstat(path.c_str(), &metadata) != 0) {
        fmt::print(stderr, "Cannot inspect {}: errno={}\n", path.string(), errno);
        return false;
    }
    fmt::print("{}: mode={:04o}, uid={}, gid={}\n",
               path.string(),
               metadata.st_mode & 07777,
               metadata.st_uid,
               metadata.st_gid);
    return (metadata.st_mode & S_IFMT) == type && (metadata.st_mode & 07777) == mode &&
           metadata.st_uid == ::geteuid() && metadata.st_gid == ::getegid();
}

auto check_runtime_resources(std::filesystem::path const& prefix) -> bool
{
    // Inspect actual resources without following replacement symlinks. Optional
    // SQLite sidecars are checked when present, rather than manufactured by us.
    if (!check_metadata(prefix / "var/lib", S_IFDIR, 0700) || !check_metadata(prefix / "var/run", S_IFDIR, 0700) ||
        !check_metadata(prefix / "var/run/jobud.sock", S_IFSOCK, 0600) ||
        !check_metadata(prefix / "var/run/jobud.sock.lock", S_IFREG, 0600) ||
        !check_metadata(prefix / "var/lib/jobu.sqlite3", S_IFREG, 0600)) {
        return false;
    }
    for (auto const* suffix : {"-wal", "-shm", "-journal", ".lock"}) {
        auto const  path = prefix / "var/lib" / fmt::format("jobu.sqlite3{}", suffix);
        struct stat metadata{};
        if (::lstat(path.c_str(), &metadata) == 0) {
            if (!check_metadata(path, S_IFREG, 0600)) {
                return false;
            }
        }
        else if (errno != ENOENT) {
            return false;
        }
    }
    return true;
}

auto fail(std::string_view message) -> int
{
    fmt::print(stderr, "jobu-fresh-install-lifecycle: {}\n", message);
    return EXIT_FAILURE;
}

} // namespace

auto main(int argc, char* argv[]) -> int
{
    if (argc != 5) {
        return fail("expected prefix, logs, phase and project version");
    }
    if (::getuid() == 0 || ::geteuid() == 0) {
        return fail("ordinary user required; unsafe root flags are not supported");
    }
    auto const prefix = std::filesystem::path{argv[1]};
    auto const logs   = std::filesystem::path{argv[2]};
    auto const phase  = std::string_view{argv[3]};
    if (phase != "fresh" && phase != "restart" && phase != "configured") {
        return fail("unknown lifecycle phase");
    }

    // Both no-INI launches must exercise automatic absence handling, not
    // --no-config. The configured launch exercises the README's optional copy.
    struct stat config{};
    auto const  inspected = ::lstat((prefix / "etc/jobud.ini").c_str(), &config);
    if (phase == "configured") {
        if (!check_metadata(prefix / "etc/jobud.ini", S_IFREG, 0600)) {
            return fail("configured launch requires a private operator-owned INI");
        }
    }
    else if (inspected == 0 || errno != ENOENT) {
        return fail("no-INI acceptance requires the active configuration to be absent");
    }

    ChildProcess daemon;
    if (!daemon.start(prefix / "bin/jobud",
                      {},
                      logs / fmt::format("{}-daemon.stdout", phase),
                      logs / fmt::format("{}-daemon.stderr", phase))) {
        return fail("unable to launch installed daemon");
    }
    if (!wait_until_ready(daemon, prefix, logs, phase, argv[4]) || !check_runtime_resources(prefix)) {
        return fail("default installed daemon/client did not reach readiness with private resources");
    }

    auto expected = phase == "fresh" ? run_client(prefix,
                                                  logs,
                                                  "fresh-create",
                                                  {"--timeout",
                                                   "1000",
                                                   "--json",
                                                   "queue",
                                                   "create",
                                                   "fresh-install",
                                                   "--weight",
                                                   "2",
                                                   "--concurrency-limit",
                                                   "3"})
                                     : read_json(logs / "fresh-create.stdout");
    if (!expected) {
        return fail("unable to obtain the durable queue expectation");
    }
    auto const id = string_field(*expected, "id");
    if (!id || !jb::core::Uuid::parse(*id) || string_field(*expected, "name") != "fresh-install") {
        return fail("queue creation did not return its canonical identity");
    }
    auto observed = run_client(prefix,
                               logs,
                               fmt::format("{}-queue", phase),
                               {"--timeout", "1000", "--json", "queue", "get", "--id", *id});
    if (!observed || *observed != *expected) {
        return fail("queue identity or settings changed across the installed lifecycle");
    }

    // SIGKILL is reserved for failed-test cleanup in the owner above. Acceptance
    // requires a normal zero exit and the daemon's removal of its own socket.
    if (!daemon.request_stop() || !successful(daemon.wait_for(5s))) {
        return fail("daemon did not stop normally before the deadline");
    }
    struct stat socket{};
    if (::lstat((prefix / "var/run/jobud.sock").c_str(), &socket) == 0 || errno != ENOENT) {
        return fail("daemon did not remove its socket on normal shutdown");
    }
    fmt::print("{} lifecycle passed; durable queue={}\n", phase, *id);
    return EXIT_SUCCESS;
}
