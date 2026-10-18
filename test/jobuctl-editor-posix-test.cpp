#include "editor_posix_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <span>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace jb::core;
using namespace jb::jobuctl::detail;

namespace {

void interrupt_handler([[maybe_unused]] int number)
{}

void check_mask(sigset_t const& actual, sigset_t const& expected)
{
    for (auto number = 1; number < NSIG; ++number) {
        CHECK(sigismember(&actual, number) == sigismember(&expected, number));
    }
}

void check_action(int number, struct sigaction const& expected)
{
    struct sigaction actual{};
    REQUIRE(::sigaction(number, nullptr, &actual) == 0);
    // Ignore libc/kernel bookkeeping flags such as Linux SA_RESTORER.
    auto const flags = SA_RESTART | SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND | SA_NOCLDWAIT | SA_NOCLDSTOP;
    CHECK((actual.sa_flags & flags) == (expected.sa_flags & flags));
    if ((expected.sa_flags & SA_SIGINFO) != 0) {
        CHECK(actual.sa_sigaction == expected.sa_sigaction);
    }
    else {
        CHECK(actual.sa_handler == expected.sa_handler);
    }
    check_mask(actual.sa_mask, expected.sa_mask);
}

class SavedSignals final {
public:
    SavedSignals()
    {
        REQUIRE(::sigaction(SIGINT, nullptr, &interrupt) == 0);
        REQUIRE(::sigaction(SIGQUIT, nullptr, &quit) == 0);
        REQUIRE(::sigaction(SIGCHLD, nullptr, &child) == 0);
        REQUIRE(::sigprocmask(SIG_BLOCK, nullptr, &mask) == 0);
    }

    ~SavedSignals()
    {
        static_cast<void>(::sigaction(SIGINT, &interrupt, nullptr));
        static_cast<void>(::sigaction(SIGQUIT, &quit, nullptr));
        static_cast<void>(::sigaction(SIGCHLD, &child, nullptr));
        static_cast<void>(::sigprocmask(SIG_SETMASK, &mask, nullptr));
    }

    SavedSignals(SavedSignals const&)                    = delete;
    auto operator=(SavedSignals const&) -> SavedSignals& = delete;

    void check_restored() const
    {
        check_action(SIGINT, interrupt);
        check_action(SIGQUIT, quit);
        check_action(SIGCHLD, child);
        sigset_t actual{};
        REQUIRE(::sigprocmask(SIG_BLOCK, nullptr, &actual) == 0);
        check_mask(actual, mask);
    }

    struct sigaction interrupt{};
    struct sigaction quit{};
    struct sigaction child{};
    sigset_t         mask{};
};

struct FakeEditor {
    SavedSignals             signals;
    int                      open_error{0};
    int                      spawn_error{0};
    int                      wait_error{0};
    int                      interrupted_waits{0};
    int                      status{0};
    pid_t                    child{12345};
    pid_t                    waited_child{12345};
    int                      open_calls{0};
    int                      spawn_calls{0};
    int                      wait_calls{0};
    int                      original_terminal{-1};
    std::vector<std::string> arguments;

    void check_parent_wait_policy() const
    {
        struct sigaction action{};
        REQUIRE(::sigaction(SIGINT, nullptr, &action) == 0);
        CHECK(action.sa_handler == SIG_IGN);
        REQUIRE(::sigaction(SIGQUIT, nullptr, &action) == 0);
        CHECK(action.sa_handler == SIG_IGN);
        sigset_t mask{};
        REQUIRE(::sigprocmask(SIG_BLOCK, nullptr, &mask) == 0);
        CHECK(sigismember(&mask, SIGCHLD) == 1);
    }

    auto io() -> EditorPosixIo
    {
        return {
            .open_terminal =
                [this] {
                    ++open_calls;
                    if (open_error != 0) {
                        errno = open_error;
                        return -1;
                    }
                    // A regular descriptor substitutes for /dev/tty. Native terminal admission is a manual gate.
                    original_terminal = ::open("/dev/null", O_RDWR);
                    CHECK(original_terminal >= 0);
                    return original_terminal;
                },
            .spawn =
                [this](pid_t*                            pid,
                       std::span<char* const>            argv,
                       posix_spawn_file_actions_t const* actions,
                       posix_spawnattr_t const*          attributes) {
                    ++spawn_calls;
                    check_parent_wait_policy();
                    CHECK(actions != nullptr);
                    CHECK(::fcntl(original_terminal, F_GETFD) == -1);
                    CHECK(errno == EBADF);
                    REQUIRE_FALSE(argv.empty());
                    CHECK(argv.back() == nullptr);
                    for (auto* argument : argv.first(argv.size() - 1)) {
                        REQUIRE(argument != nullptr);
                        arguments.emplace_back(argument);
                    }

                    short flags{};
                    REQUIRE(::posix_spawnattr_getflags(attributes, &flags) == 0);
                    CHECK(flags == (POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));
                    sigset_t defaults{};
                    REQUIRE(::posix_spawnattr_getsigdefault(attributes, &defaults) == 0);
                    CHECK(sigismember(&defaults, SIGINT) == 1);
                    CHECK(sigismember(&defaults, SIGQUIT) == 1);
                    sigset_t mask{};
                    REQUIRE(::posix_spawnattr_getsigmask(attributes, &mask) == 0);
                    auto expected = signals.mask;
                    REQUIRE(sigdelset(&expected, SIGINT) == 0);
                    REQUIRE(sigdelset(&expected, SIGQUIT) == 0);
                    check_mask(mask, expected);

                    *pid  = child;
                    errno = EBUSY; // Spawn failures must use the returned error number, never this unrelated errno.
                    return spawn_error;
                },
            .wait = [this](pid_t pid, int* value) -> pid_t {
                ++wait_calls;
                CHECK(pid == child);
                check_parent_wait_policy();
                if (interrupted_waits > 0) {
                    --interrupted_waits;
                    errno = EINTR;
                    return -1;
                }
                if (wait_error != 0) {
                    errno = wait_error;
                    return -1;
                }
                *value = status;
                return waited_child;
            },
        };
    }

    auto launch() -> Result<void, Error>
    {
        auto const command = std::vector<std::string>{"fake-editor", "--wait"};
        auto       result  = launch_editor(command, "/private draft/draft.json", io());
        signals.check_restored();
        if (original_terminal >= 0) {
            CHECK(::fcntl(original_terminal, F_GETFD) == -1);
            CHECK(errno == EBADF);
        }
        return result;
    }
};

} // namespace

TEST_CASE("editor launcher appends one literal absolute path and waits for the owned PID", "[jobuctl][editor]")
{
    auto       fake     = FakeEditor{};
    auto const command  = std::vector<std::string>{"/editor tools/editor", "--wait", "", "$HOME;$(id)"};
    auto const path     = std::filesystem::path{"/private draft/'$(id);*.json"};
    auto       launched = launch_editor(command, path, fake.io());
    REQUIRE(launched);
    CHECK(fake.arguments ==
          std::vector<std::string>{"/editor tools/editor", "--wait", "", "$HOME;$(id)", path.string()});
    CHECK(fake.open_calls == 1);
    CHECK(fake.spawn_calls == 1);
    CHECK(fake.wait_calls == 1);
    CHECK(command.size() == 4);
    fake.signals.check_restored();
}

TEST_CASE("editor wait retries interruption and restores custom handlers and blocked signals", "[jobuctl][editor]")
{
    auto             original = SavedSignals{};
    struct sigaction action{};
    action.sa_handler = interrupt_handler;
    action.sa_flags   = SA_RESTART;
    REQUIRE(sigemptyset(&action.sa_mask) == 0);
    REQUIRE(sigaddset(&action.sa_mask, SIGUSR1) == 0);
    REQUIRE(::sigaction(SIGINT, &action, nullptr) == 0);
    action.sa_handler = SIG_IGN;
    REQUIRE(::sigaction(SIGQUIT, &action, nullptr) == 0);
    sigset_t blocked{};
    REQUIRE(sigemptyset(&blocked) == 0);
    REQUIRE(sigaddset(&blocked, SIGINT) == 0);
    REQUIRE(sigaddset(&blocked, SIGQUIT) == 0);
    REQUIRE(sigaddset(&blocked, SIGUSR1) == 0);
    REQUIRE(::sigprocmask(SIG_BLOCK, &blocked, nullptr) == 0);

    auto fake              = FakeEditor{};
    fake.interrupted_waits = 2;
    CHECK(fake.launch());
    CHECK(fake.wait_calls == 3);
}

TEST_CASE("editor nonzero exit and signal termination are distinct cancellations", "[jobuctl][editor]")
{
    struct Example {
        int         status;
        std::string code;
        std::string number;
    };

    // Synthetic Linux/Darwin wait statuses; no child process or editor is started.
    auto const examples = std::vector<Example>{
        {.status = 7 << 8,   .code = "jobuctl.editor.cancelled",   .number = "7"                    },
        {.status = 127 << 8, .code = "jobuctl.editor.cancelled",   .number = "127"                  },
        {.status = SIGINT,   .code = "jobuctl.editor.interrupted", .number = std::to_string(SIGINT) },
        {.status = SIGQUIT,  .code = "jobuctl.editor.interrupted", .number = std::to_string(SIGQUIT)},
    };
    for (auto const& example : examples) {
        auto fake     = FakeEditor{};
        fake.status   = example.status;
        auto launched = fake.launch();
        REQUIRE_FALSE(launched);
        CHECK(launched.error().category == ErrorCategory::Cancelled);
        CHECK(launched.error().code == example.code);
        CHECK(launched.error().message.find(example.number) != std::string::npos);
        CHECK(launched.error().message.find("no update submitted") != std::string::npos);
    }
}

TEST_CASE("editor setup spawn and wait failures are safe and do not become successful edits", "[jobuctl][editor]")
{
    auto fake          = FakeEditor{};
    auto expected_code = std::string{};
    SECTION("unavailable controlling terminal")
    {
        fake.open_error = ENXIO;
        expected_code   = "jobuctl.editor.terminal_unavailable";
    }
    SECTION("spawn error number is independent of errno")
    {
        fake.spawn_error = ENOENT;
        expected_code    = "jobuctl.editor.launch_failed";
    }
    SECTION("owned child no longer waitable")
    {
        fake.wait_error = ECHILD;
        expected_code   = "jobuctl.editor.wait_failed";
    }
    SECTION("other wait failure")
    {
        fake.wait_error = EIO;
        expected_code   = "jobuctl.editor.wait_failed";
    }
    auto launched = fake.launch();
    REQUIRE_FALSE(launched);
    CHECK(launched.error().category == ErrorCategory::Io);
    CHECK(launched.error().code == expected_code);
    CHECK(launched.error().message.find("fake-editor") == std::string::npos);
    CHECK(launched.error().message.find("private draft") == std::string::npos);
    if (fake.open_error != 0) {
        CHECK(fake.spawn_calls == 0);
    }
    if (fake.open_error != 0 || fake.spawn_error != 0) {
        CHECK(fake.wait_calls == 0);
    }
    if (fake.spawn_error != 0) {
        CHECK(launched.error().message.find("system error " + std::to_string(ENOENT)) != std::string::npos);
    }
}

TEST_CASE("editor launcher rejects nonwaitable child signal policies before spawn", "[jobuctl][editor]")
{
    auto             original = SavedSignals{};
    struct sigaction action{};
    REQUIRE(sigemptyset(&action.sa_mask) == 0);
    SECTION("ignored child notifications")
    {
        action.sa_handler = SIG_IGN;
    }
    SECTION("automatic child reaping")
    {
        action.sa_handler = SIG_DFL;
        action.sa_flags   = SA_NOCLDWAIT;
    }
    REQUIRE(::sigaction(SIGCHLD, &action, nullptr) == 0);
    auto fake     = FakeEditor{};
    auto launched = fake.launch();
    REQUIRE_FALSE(launched);
    CHECK(launched.error().code == "jobuctl.editor.wait_unavailable");
    CHECK(fake.spawn_calls == 0);
    CHECK(fake.wait_calls == 0);
}

TEST_CASE("editor launcher never broadens owned waiting after invalid fake outcomes", "[jobuctl][editor]")
{
    auto fake = FakeEditor{};
    SECTION("invalid spawn PID")
    {
        fake.child = 0;
    }
    SECTION("wrong wait PID")
    {
        fake.waited_child = 54321;
    }
    SECTION("nonterminal status")
    {
        fake.status = (SIGSTOP << 8) | 0x7f;
    }
    auto launched = fake.launch();
    REQUIRE_FALSE(launched);
    CHECK(launched.error().category == ErrorCategory::Internal);
    if (fake.child == 0) {
        CHECK(launched.error().code == "jobuctl.editor.invalid_child");
        CHECK(fake.wait_calls == 0);
    }
    else {
        CHECK(launched.error().code == "jobuctl.editor.invalid_wait");
        CHECK(fake.wait_calls == 1);
    }
}

TEST_CASE("editor launcher rejects invalid arguments before any terminal or process effects", "[jobuctl][editor]")
{
    struct Example {
        std::vector<std::string> command;
        std::filesystem::path    path;
    };

    auto examples = std::vector<Example>{
        {.command = {},                                                    .path = "/draft.json"                             },
        {.command = {""},                                                  .path = "/draft.json"                             },
        {.command = {"editor"},                                            .path = "draft.json"                              },
        {.command = {"editor"},                                            .path = ""                                        },
        {.command = {std::string{"private"} + '\0' + "literal"},           .path = "/draft.json"                             },
        {.command = {"editor", std::string{"private"} + '\0' + "literal"}, .path = "/draft.json"                             },
        {.command = {"editor"},                                            .path = std::string{"/private"} + '\0' + "literal"},
    };
    for (auto const& example : examples) {
        auto fake     = FakeEditor{};
        auto launched = launch_editor(example.command, example.path, fake.io());
        REQUIRE_FALSE(launched);
        CHECK(launched.error().category == ErrorCategory::InvalidArgument);
        CHECK(launched.error().message.find("private") == std::string::npos);
        CHECK(fake.open_calls == 0);
        CHECK(fake.spawn_calls == 0);
        CHECK(fake.wait_calls == 0);
        fake.signals.check_restored();
    }
}

TEST_CASE("editor workflow seam accepts an in-process replacement", "[jobuctl][editor]")
{
    auto           called   = false;
    EditorLauncher launcher = [&called](std::span<std::string const> command, std::filesystem::path const& path) {
        called = true;
        CHECK(command.front() == "fake-editor");
        CHECK(path == "/draft.json");
        return Result<void, Error>::success();
    };
    auto const command = std::vector<std::string>{"fake-editor"};
    CHECK(launcher(command, "/draft.json"));
    CHECK(called);
}
