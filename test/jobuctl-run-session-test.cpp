#include "run_session_priv.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "control_json.hpp"
#include "event_loop.hpp"
#include "event_thread.hpp"
#include "framing.hpp"
#include "history_json.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "management_json.hpp"
#include "session_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/temporary_directory.hpp"
#include "system_info.hpp"
#include "thread_context.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <exception>
#include <filesystem>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;
using namespace jb::net;
using namespace jb::rpc;

namespace {

constexpr auto fixture_id = "00112233-4455-6677-8899-aabbccddeeff";

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
};

/// Capture actual descriptor writes, including raw output. Restore before Catch reports assertions.
class StreamCapture {
public:
    explicit StreamCapture(std::FILE* stream)
        : _stream{stream}
        , _file{std::tmpfile()}
    {
        REQUIRE(_file);
        std::fflush(_stream);
        _saved = ::dup(::fileno(_stream));
        REQUIRE(_saved >= 0);
        REQUIRE(::dup2(::fileno(_file.get()), ::fileno(_stream)) >= 0);
    }

    ~StreamCapture() { restore(); }

    StreamCapture(StreamCapture const&)                    = delete;
    auto operator=(StreamCapture const&) -> StreamCapture& = delete;

    auto text() -> std::string
    {
        restore();
        REQUIRE(std::fseek(_file.get(), 0, SEEK_SET) == 0);
        std::string            result;
        std::array<char, 4096> block{};
        while (auto count = std::fread(block.data(), 1, block.size(), _file.get())) {
            result.append(block.data(), count);
        }
        REQUIRE_FALSE(std::ferror(_file.get()));
        return result;
    }

private:
    void restore()
    {
        if (_saved < 0) {
            return;
        }
        std::fflush(_stream);
        if (::dup2(_saved, ::fileno(_stream)) < 0) {
            std::terminate();
        }
        ::close(_saved);
        _saved = -1;
        std::clearerr(_stream);
    }

    std::FILE*                             _stream;
    std::unique_ptr<std::FILE, FileCloser> _file;
    int                                    _saved{-1};
};

enum class PeerAction : std::uint8_t {
    Reply,
    RemoteError,
    Silence,
    Disconnect,
    Duplicate
};

struct PeerStep {
    std::string method;
    JsonValue   value;
    PeerAction  action{PeerAction::Reply};
};

auto handshake() -> PeerStep
{
    auto info = SystemInfo{
        .daemon_version = "scoped-fixture",
        .api_version    = {.major = 1, .minor = 5},
        .capabilities   = {"system.info",
                           "job.get", "queue.list",
                           "queue.delete", "queue.suspend",
                           "queue.get", "run.cancel",
                           "run.get", "attempt.output"}
    };
    return {.method = "system.info", .value = system_info_to_json(info)};
}

/// A worker owns only an EventThread and socket peer; it never creates a second Application.
/// Readiness and join synchronize all access to worker-owned state. No timing sleeps or worker assertions.
class ScriptedPeer {
public:
    explicit ScriptedPeer(std::vector<PeerStep> steps)
        : _steps{std::move(steps)}
    {
        REQUIRE(_thread.start());
        // about_to_start precedes EventLoop::run() adopting this thread; perform I/O from a loop task instead.
        REQUIRE(_thread.as_event_loop()->post([this] {
            _listener = std::make_unique<LocalServer>();
            _listener->new_connection.connect(_listener.get(), [this] {
                _socket = _listener->take_next_connection();
                if (!_socket) {
                    _error = "missing peer connection";
                    return;
                }
                _socket->ready_read.connect(_socket.get(), [this] { read_requests(); });
                read_requests();
            });
            if (!_listener->listen(path())) {
                _error = "unable to listen";
            }
            _ready.count_down();
        }));
        _ready.wait();
        if (!_error.empty()) {
            stop();
        }
        REQUIRE(_error.empty());
    }

    ~ScriptedPeer() { stop(); }

    ScriptedPeer(ScriptedPeer const&)                    = delete;
    auto operator=(ScriptedPeer const&) -> ScriptedPeer& = delete;

    auto path() const -> std::filesystem::path { return _directory.path() / "peer.sock"; }

    void verify()
    {
        stop();
        INFO(_error);
        CHECK(_error.empty());
        CHECK(_next_step == _steps.size());
    }

private:
    void stop()
    {
        if (_stopped) {
            return;
        }
        _stopped = true;

        // Remove worker socket watches before run() restores the loop's original thread context.
        if (!_thread.as_event_loop()->post([this] {
                _socket.reset();
                _listener.reset();
                static_cast<void>(_thread.quit());
            })) {
            std::terminate();
        }
        _thread.wait();
    }

    void read_requests()
    {
        auto bodies = _framer.append(_socket->read_all());
        if (!bodies) {
            _error = "invalid request framing";
            return;
        }
        for (auto const& body : *bodies) {
            auto request = parse_json(body);
            if (!request || !request->is_object() || _next_step == _steps.size()) {
                _error = "unexpected request";
                return;
            }
            auto const& fields = request->as_object();
            auto const& step   = _steps[_next_step++];
            if (fields.at("method").as_string() != step.method) {
                _error = "unexpected method";
                return;
            }
            if (step.action == PeerAction::Silence) {
                continue;
            }
            if (step.action == PeerAction::Disconnect) {
                _socket->abort();
                return;
            }

            auto response = JsonValue{
                .data = JsonValue::Object{{"jsonrpc", JsonValue{.data = std::string{"2.0"}}},
                                          {"id", fields.at("id")},
                                          {step.action == PeerAction::RemoteError ? "error" : "result", step.value}}
            };
            auto encoded = serialize_json(response);
            if (!encoded) {
                _error = "unserializable response";
                return;
            }
            auto frame = frame_message(*encoded);
            if (!frame) {
                _error = "unframeable response";
                return;
            }
            auto bytes = *frame;
            if (step.action == PeerAction::Duplicate) {
                bytes.insert(bytes.end(), frame->begin(), frame->end());
            }
            if (_socket->write(bytes) != bytes.size()) {
                _error = "response write failed";
                return;
            }
        }
    }

    jb::test::TemporaryDirectory _directory;
    std::vector<PeerStep>        _steps;
    std::size_t                  _next_step{0};
    std::string                  _error;
    StreamFramer                 _framer;
    std::unique_ptr<LocalServer> _listener;
    std::unique_ptr<LocalSocket> _socket;
    std::latch                   _ready{1};
    bool                         _stopped{false};
    EventThread                  _thread;
};

auto command_for(std::filesystem::path const& path, std::vector<std::string> arguments) -> Command
{
    arguments.insert(arguments.begin(), {"jobuctl", "--socket", path.string()});
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    StandardAttributeRegistry registry;
    auto                      parsed = parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
    INFO(parsed.error);
    REQUIRE(parsed.action);
    REQUIRE(std::holds_alternative<Command>(*parsed.action));
    return std::move(std::get<Command>(*parsed.action));
}

struct CapturedSession {
    SessionResult result;
    std::string   output;
    std::string   error;
};

auto run_captured(Command command, SessionMode mode = SessionMode::Capture) -> CapturedSession
{
    StandardAttributeRegistry registry;
    StreamCapture             output{stdout};
    StreamCapture             error{stderr};
    auto                      result      = run_session(std::move(command), registry, mode);
    auto                      error_text  = error.text();
    auto                      output_text = output.text();
    return {.result = std::move(result), .output = std::move(output_text), .error = std::move(error_text)};
}

void check_silent(CapturedSession const& captured)
{
    CHECK(captured.output.empty());
    CHECK(captured.error.empty());
    CHECK(Application::instance() == nullptr);
    CHECK(ThreadCtx::current()->event_loop() == nullptr);
}

auto fixture_job() -> JobDefinition
{
    StandardAttributeRegistry registry;
    auto                      id         = Uuid::parse(fixture_id);
    auto                      at         = parse_utc_timestamp("2030-01-01T00:00:00Z");
    auto                      attributes = materialize_attributes(registry, {}, {}, {});
    auto                      payload    = parse_json(R"({"command":"/bin/true"})");
    REQUIRE(id);
    REQUIRE(at);
    REQUIRE(attributes);
    REQUIRE(payload);
    return {.id         = *id,
            .queue_id   = *id,
            .revision   = 7,
            .name       = "original",
            .schedule   = OnceSchedule{.planned_at = *at},
            .attributes = *attributes,
            .payload    = *payload,
            .created_at = *at,
            .updated_at = *at};
}

} // namespace

TEST_CASE("scoped capture owns a job beyond every network and registry lifetime", "[jobuctl][session][capture]")
{
    auto encoded = job_to_json(fixture_job(), StandardAttributeRegistry{});
    REQUIRE(encoded);
    std::optional<CapturedSession> captured;
    {
        ScriptedPeer peer{
            {handshake(), {.method = "job.get", .value = *encoded}}
        };
        captured = run_captured(command_for(peer.path(), {"job", "get", fixture_id, "--json"}));
        peer.verify();
    }
    check_silent(*captured);
    REQUIRE(captured->result.exit_code == 0);
    REQUIRE(captured->result.reply);
    CHECK_FALSE(captured->result.error);
    auto const& job = std::get<JobDefinition>(*captured->result.reply);
    CHECK(job.id.to_string() == fixture_id);
    CHECK(job.revision == 7);
    CHECK(job.name == "original");
    CHECK(job.payload == fixture_job().payload);

    // A new ordinary session starts only after the fetch's complete Application scope has gone away.
    auto empty_page = parse_json(R"({"items":[],"next_after_id":null})");
    REQUIRE(empty_page);
    ScriptedPeer peer{
        {handshake(), {.method = "queue.list", .value = *empty_page}}
    };
    auto rendered = run_captured(command_for(peer.path(), {"queue", "list", "--json"}), SessionMode::Render);
    peer.verify();
    CHECK(rendered.result.exit_code == 0);
    CHECK_FALSE(rendered.result.reply);
    CHECK(rendered.error.empty());
    auto page = parse_json(rendered.output);
    REQUIRE(page);
    CHECK(page->as_object().at("items").as_array().empty());
    CHECK(Application::instance() == nullptr);
    CHECK(ThreadCtx::current()->event_loop() == nullptr);
    CHECK(job.name == "original");
}

TEST_CASE("capture includes the handshake reply without printing", "[jobuctl][session][capture]")
{
    ScriptedPeer peer{{handshake()}};
    auto         captured = run_captured(command_for(peer.path(), {"system", "info"}));
    peer.verify();
    check_silent(captured);
    REQUIRE(captured.result.reply);
    CHECK(std::get<SystemInfo>(*captured.result.reply).daemon_version == "scoped-fixture");
}

TEST_CASE("capture preserves remote and local failure projections", "[jobuctl][session][capture]")
{
    auto error = parse_json(
        R"({"code":-32000,"message":"fixture rejection","data":{"category":"conflict","code":"jobu.fixture.conflict"}})");
    REQUIRE(error);
    ScriptedPeer peer{
        {handshake(), {.method = "queue.delete", .value = *error, .action = PeerAction::RemoteError}}
    };
    auto captured = run_captured(command_for(peer.path(), {"queue", "delete", "--name", "reports", "--json"}));
    peer.verify();
    check_silent(captured);
    CHECK(captured.result.exit_code == 1);
    CHECK_FALSE(captured.result.reply);
    REQUIRE(captured.result.error);
    CHECK(captured.result.error->kind == "remote");
    CHECK(captured.result.error->rpc_code == -32000);
    CHECK(captured.result.error->category == "conflict");
    CHECK(captured.result.error->code == "jobu.fixture.conflict");
    CHECK(captured.result.error->message == "fixture rejection");
    CHECK_FALSE(captured.result.error->outcome_unknown);

    auto info  = handshake();
    info.value = system_info_to_json({
        .daemon_version = "old-fixture",
        .api_version    = {.major = 1, .minor = 2},
        .capabilities   = {"system.info"}
    });
    ScriptedPeer unsupported{{std::move(info)}};
    auto         local = run_captured(command_for(unsupported.path(), {"queue", "list"}));
    unsupported.verify();
    check_silent(local);
    CHECK(local.result.exit_code == 1);
    REQUIRE(local.result.error);
    CHECK(local.result.error->kind == "local");
    CHECK(local.result.error->code == "jobu.client.unsupported_method");
}

TEST_CASE("capture preserves timeout and mutation uncertainty without printing", "[jobuctl][session][capture]")
{
    SECTION("handshake timeout")
    {
        auto info   = handshake();
        info.action = PeerAction::Silence;
        ScriptedPeer peer{{std::move(info)}};
        auto         captured = run_captured(command_for(peer.path(), {"queue", "list", "--timeout", "200"}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 3);
        REQUIRE(captured.result.error);
        CHECK(captured.result.error->code == "jobu.client.timeout");
        CHECK_FALSE(captured.result.error->outcome_unknown);
    }
    SECTION("connection lost after mutation delivery")
    {
        ScriptedPeer peer{
            {handshake(), {.method = "queue.delete", .value = {}, .action = PeerAction::Disconnect}}
        };
        auto captured = run_captured(command_for(peer.path(), {"queue", "delete", "--name", "reports"}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 3);
        REQUIRE(captured.result.error);
        CHECK(captured.result.error->outcome_unknown);
    }
}

TEST_CASE("capture never delivers attempt bytes or creates the requested output file", "[jobuctl][session][capture]")
{
    auto id = Uuid::parse(fixture_id);
    REQUIRE(id);
    auto chunk = AttemptOutputChunk{
        .attempt        = {.run_id = *id, .attempt_number = 1},
        .channel        = OutputChannel::Stdout,
        .status         = OutputStatus::Available,
        .bytes_returned = 2,
        .retained_bytes = 2,
        .total_bytes    = 2,
        .omitted_bytes  = 0,
        .encoding       = OutputEncoding::Base64,
        .data           = {std::byte{0},  std::byte{0xff}    }
    };
    auto encoded = attempt_output_chunk_to_json(chunk);
    REQUIRE(encoded);
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "must-not-exist.bin";
    for (auto const& delivery : std::vector<std::vector<std::string>>{
             {"--raw"},
             {"--output-file", path.string()}
    }) {
        ScriptedPeer peer{
            {handshake(), {.method = "attempt.output", .value = *encoded}}
        };
        auto arguments = std::vector<std::string>{"attempt", "output", fixture_id, "1", "--channel", "stdout"};
        arguments.insert(arguments.end(), delivery.begin(), delivery.end());
        auto captured = run_captured(command_for(peer.path(), std::move(arguments)));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 0);
        REQUIRE(captured.result.reply);
        CHECK(std::get<AttemptOutputChunk>(*captured.result.reply).data == chunk.data);
        CHECK_FALSE(std::filesystem::exists(path));
    }
}

TEST_CASE("capture retains the final wait reply rather than the initial mutation", "[jobuctl][session][capture]")
{
    StandardAttributeRegistry registry;
    auto                      job = fixture_job();

    SECTION("suspension observation")
    {
        auto queue     = Queue{.id         = job.queue_id,
                               .name       = "reports",
                               .state      = QueueState::Suspending,
                               .created_at = job.created_at,
                               .updated_at = job.updated_at};
        auto pending   = queue_to_json(queue, registry);
        queue.state    = QueueState::Suspended;
        auto completed = queue_to_json(queue, registry);
        REQUIRE(pending);
        REQUIRE(completed);
        ScriptedPeer peer{
            {handshake(),
             {.method = "queue.suspend", .value = *pending},
             {.method = "queue.get", .value = *completed}}
        };
        auto captured = run_captured(command_for(peer.path(), {"queue", "suspend", "--name", "reports", "--wait"}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 0);
        REQUIRE(captured.result.reply);
        CHECK(std::get<Queue>(*captured.result.reply).state == QueueState::Suspended);
    }

    SECTION("cancellation synthesizes the completed result")
    {
        auto at      = std::get<OnceSchedule>(job.schedule).planned_at;
        auto run     = JobRun{.id           = job.id,
                              .job_id       = job.id,
                              .job_revision = job.revision,
                              .queue_id     = job.queue_id,
                              .planned_at   = at,
                              .runnable_at  = at,
                              .attributes   = job.attributes,
                              .payload      = job.payload,
                              .state        = RunState::Running};
        auto pending = cancel_run_result_to_json({.run = run, .disposition = CancelDisposition::Requested}, registry);
        run.state    = RunState::Cancelled;
        run.completed_at = at;
        auto completed   = run_details_to_json(run, registry);
        REQUIRE(pending);
        REQUIRE(completed);
        ScriptedPeer peer{
            {handshake(), {.method = "run.cancel", .value = *pending}, {.method = "run.get", .value = *completed}}
        };
        auto captured = run_captured(command_for(peer.path(), {"run", "cancel", fixture_id, "--wait"}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 0);
        REQUIRE(captured.result.reply);
        auto const& result = std::get<CancelRunResult>(*captured.result.reply);
        CHECK(result.disposition == CancelDisposition::Completed);
        CHECK(result.run.state == RunState::Cancelled);
        CHECK(result.run.id == job.id);
    }
}

TEST_CASE("synchronous start failure completes once and extracts only after unwinding", "[jobuctl][session][capture]")
{
    jb::test::TemporaryDirectory directory;
    auto                         command = command_for(directory.path() / "missing.sock", {"system", "info"});
    StandardAttributeRegistry    registry;
    StreamCapture                output{stdout};
    StreamCapture                error{stderr};
    int                          finishes       = 0;
    bool                         start_returned = false;
    bool                         synchronous    = false;
    std::vector<int>             order;
    std::optional<SessionResult> result;
    {
        Application app{0, nullptr};
        Session     session{std::move(command), registry, SessionMode::Capture};
        REQUIRE_FALSE(session.take_result());
        session.finished.connect(&app, [&](int code) {
            ++finishes;
            synchronous = !start_returned;
            order.push_back(1);
            static_cast<void>(app.quit(code));
            session.start(); // Reentrant start after terminal state cannot create a second operation.
            order.push_back(2);
        });
        session.start();
        start_returned = true;
        auto code      = app.exec();
        order.push_back(3);
        result = session.take_result();
        REQUIRE(result);
        CHECK(code == result->exit_code);
        CHECK_FALSE(session.take_result());
    }
    auto error_text  = error.text();
    auto output_text = output.text();
    CHECK(error_text.empty());
    CHECK(output_text.empty());
    CHECK(synchronous);
    CHECK(finishes == 1);
    CHECK(order == std::vector<int>{1, 2, 3});
    REQUIRE(result);
    CHECK(result->exit_code == 3);
    REQUIRE(result->error);
    CHECK(result->error->code == "jobuctl.connection_failed");
    CHECK(Application::instance() == nullptr);
    CHECK(ThreadCtx::current()->event_loop() == nullptr);

    auto scoped = run_captured(command_for(directory.path() / "missing.sock", {"system", "info"}));
    check_silent(scoped);
    CHECK(scoped.result.exit_code == 3);
    REQUIRE(scoped.result.error);
    CHECK(scoped.result.error->code == "jobuctl.connection_failed");
}

TEST_CASE("duplicate peer replies cannot replace a captured result or finish twice", "[jobuctl][session][capture]")
{
    auto info   = handshake();
    info.action = PeerAction::Duplicate;
    ScriptedPeer                 peer{{std::move(info)}};
    auto                         command = command_for(peer.path(), {"system", "info"});
    StandardAttributeRegistry    registry;
    StreamCapture                output{stdout};
    StreamCapture                error{stderr};
    int                          finishes = 0;
    std::vector<int>             order;
    std::optional<SessionResult> result;
    {
        Application app{0, nullptr};
        Session     session{std::move(command), registry, SessionMode::Capture};
        session.finished.connect(&app, [&](int code) {
            ++finishes;
            order.push_back(1);
            static_cast<void>(app.quit(code));
            order.push_back(2);
        });
        app.about_to_quit.connect(&app, [&] { order.push_back(3); });
        session.start();
        CHECK(app.exec() == 0);
        order.push_back(4);
        result = session.take_result();
    }
    auto error_text  = error.text();
    auto output_text = output.text();
    peer.verify();
    CHECK(error_text.empty());
    CHECK(output_text.empty());
    CHECK(finishes == 1);
    CHECK(order == std::vector<int>{1, 2, 3, 4});
    REQUIRE(result);
    CHECK_FALSE(result->error);
    REQUIRE(result->reply);
    CHECK(std::get<SystemInfo>(*result->reply).daemon_version == "scoped-fixture");
}
