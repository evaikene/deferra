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
#include "output_priv.hpp"
#include "session_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_event_loop_backend.hpp"
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
#include <fstream>
#include <functional>
#include <iterator>
#include <latch>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sys/stat.h>
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
    std::string              method;
    JsonValue                value;
    PeerAction               action{PeerAction::Reply};
    std::optional<JsonValue> params;
};

struct RequestObservation {
    EventLoop*                              loop;
    std::function<void(std::string const&)> observe;
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
                           "run.get", "attempt.list",
                           "attempt.get", "attempt.output"}
    };
    return {.method = "system.info", .value = system_info_to_json(info)};
}

/// A worker owns only an EventThread and socket peer; it never creates a second Application.
/// Readiness and join synchronize all access to worker-owned state. No timing sleeps or worker assertions.
class ScriptedPeer {
public:
    explicit ScriptedPeer(std::vector<PeerStep> steps, std::optional<RequestObservation> observation = std::nullopt)
        : _steps{std::move(steps)}
        , _observation{std::move(observation)}
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
            if (step.params) {
                // Compare the wire shape, independent of in-memory signed/unsigned integer alternatives.
                auto expected = serialize_json(*step.params);
                auto actual   = serialize_json(fields.at("params"));
                if (!expected || !actual || *expected != *actual) {
                    _error = "unexpected request parameters for " + step.method;
                    return;
                }
            }

            // Gate a response through an owner-loop observation when testing timer continuity. All peer I/O
            // still runs on its worker, and the observing task completes before the next request can arrive.
            auto respond = [this, id = fields.at("id"), step] { send_response(id, step); };
            if (_observation) {
                if (!_observation->loop->post([this, method = step.method, respond = std::move(respond)] {
                        _observation->observe(method);
                        if (!_thread.as_event_loop()->post(respond)) {
                            std::terminate();
                        }
                    })) {
                    _error = "unable to post request observation";
                    return;
                }
            }
            else {
                respond();
            }
        }
    }

    void send_response(JsonValue const& id, PeerStep const& step)
    {
        if (step.action == PeerAction::Silence) {
            return;
        }
        if (step.action == PeerAction::Disconnect) {
            _socket->abort();
            return;
        }

        auto response = JsonValue{
            .data = JsonValue::Object{{"jsonrpc", JsonValue{.data = std::string{"2.0"}}},
                                      {"id", id},
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

    jb::test::TemporaryDirectory      _directory;
    std::vector<PeerStep>             _steps;
    std::optional<RequestObservation> _observation;
    std::size_t                       _next_step{0};
    std::string                       _error;
    StreamFramer                      _framer;
    std::unique_ptr<LocalServer>      _listener;
    std::unique_ptr<LocalSocket>      _socket;
    std::latch                        _ready{1};
    bool                              _stopped{false};
    EventThread                       _thread;
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

auto inspection_run(JobType type = JobType::Cli, RunState state = RunState::Failed) -> RunDetails
{
    auto       job  = fixture_job();
    auto const at   = std::get<OnceSchedule>(job.schedule).planned_at;
    auto       run  = RunDetails{};
    run.id          = job.id;
    run.job_id      = job.id;
    run.queue_id    = job.queue_id;
    run.planned_at  = at;
    run.runnable_at = at;
    run.started_at  = at;
    run.type        = type;
    run.state       = state;
    run.attributes  = std::move(job.attributes);
    run.payload     = std::move(job.payload);
    if (state == RunState::Failed || state == RunState::Succeeded) {
        run.completed_at = at;
    }
    if (type == JobType::Http) {
        auto payload = parse_json(R"({"url":"http://localhost/","method":"GET"})");
        REQUIRE(payload);
        run.payload = *payload;
    }
    return run;
}

auto inspection_attempt(AttemptState state = AttemptState::Completed) -> AttemptDetails
{
    auto run               = inspection_run();
    auto attempt           = AttemptDetails{};
    attempt.run_id         = run.id;
    attempt.attempt_number = 7;
    attempt.due_at         = run.planned_at;
    attempt.state          = state;
    if (state != AttemptState::Pending) {
        attempt.started_at = run.planned_at;
    }
    if (state == AttemptState::Completed) {
        attempt.completed_at = run.planned_at;
        attempt.outcome      = AttemptOutcome::Failed;
    }
    return attempt;
}

auto inspection_steps(RunDetails const& run, AttemptDetails const& attempt) -> std::vector<PeerStep>
{
    auto encoded_run = run_details_to_json(run, StandardAttributeRegistry{});
    auto page        = attempt_page_to_json({.items = {attempt}, .next_cursor = "older-attempts"});
    auto details     = attempt_details_to_json(attempt);
    auto run_params  = run_get_request_to_json(run.id);
    auto list_params = attempt_list_request_to_json(AttemptQuery{.run_id = run.id, .limit = 1});
    auto get_params  = attempt_get_request_to_json({.run_id = run.id, .attempt_number = attempt.attempt_number});
    REQUIRE(encoded_run);
    REQUIRE(page);
    REQUIRE(details);
    REQUIRE(run_params);
    REQUIRE(list_params);
    REQUIRE(get_params);
    return {
        handshake(),
        {.method = "run.get",      .value = *encoded_run, .params = *run_params },
        {.method = "attempt.list", .value = *page,        .params = *list_params},
        {.method = "attempt.get",  .value = *details,     .params = *get_params }
    };
}

void check_once(std::string const& text, std::string_view marker)
{
    auto const position = text.find(marker);
    CHECK(position != std::string::npos);
    CHECK(position == text.rfind(marker));
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

TEST_CASE("both output command identities share chunk rendering and binary delivery", "[jobuctl][output]")
{
    auto id = Uuid::parse(fixture_id);
    REQUIRE(id);
    auto chunk = AttemptOutputChunk{
        .attempt        = {.run_id = *id, .attempt_number = 7},
        .channel        = OutputChannel::Stderr,
        .status         = OutputStatus::Available,
        .offset         = 2,
        .bytes_returned = 4,
        .next_offset    = 6,
        .retained_bytes = 8,
        .total_bytes    = 12,
        .omitted_bytes  = 4,
        .truncated      = true,
        .encoding       = OutputEncoding::Base64,
        .data           = {std::byte{'A'}, std::byte{0}, std::byte{0xff}, std::byte{'Z'}},
    };
    StandardAttributeRegistry    registry;
    jb::test::TemporaryDirectory directory;

    // Exercise delivery directly: U.16 has no run-attempt selection or network orchestration yet.
    for (auto kind : {CommandKind::AttemptOutput, CommandKind::RunOutput}) {
        auto          command = Command{.kind = kind, .method = {}};
        StreamCapture human_output{stdout};
        auto          printed = print_command_result(command, ControlReply{chunk}, registry);
        auto          human   = human_output.text();
        REQUIRE(printed);
        CHECK(human.find("Output " + std::string{fixture_id} + ":7 channel=stderr, status=available") !=
              std::string::npos);
        CHECK(human.find("offset=2, bytes_returned=4, retained_bytes=8") != std::string::npos);
        CHECK(human.find("total_bytes=12, omitted_bytes=4, next_offset=6, truncated=true") != std::string::npos);
        CHECK(human.find("encoding=base64\nData: QQD/Wg==\n") != std::string::npos);

        for (auto encoding : {OutputEncoding::Base64, OutputEncoding::Utf8}) {
            auto encoded_chunk     = chunk;
            encoded_chunk.encoding = encoding;
            if (encoding == OutputEncoding::Utf8) {
                encoded_chunk.data = {std::byte{'t'}, std::byte{'e'}, std::byte{'x'}, std::byte{'t'}};
            }
            auto encoded = attempt_output_chunk_to_json(encoded_chunk);
            REQUIRE(encoded);
            auto serialized = serialize_json(*encoded);
            REQUIRE(serialized);
            command.json = true;
            StreamCapture json_output{stdout};
            printed   = print_command_result(command, ControlReply{encoded_chunk}, registry);
            auto json = json_output.text();
            REQUIRE(printed);
            CHECK(json == *serialized + "\n");
        }

        command.json = false;
        command.raw  = true;
        StreamCapture raw_output{stdout};
        auto          delivered = write_output_chunk(command, chunk);
        auto          raw       = raw_output.text();
        REQUIRE(delivered);
        CHECK(raw == std::string{"A\0\xffZ", 4});

        command.raw         = false;
        auto const path     = directory.path() / (kind == CommandKind::RunOutput ? "run.bin" : "attempt.bin");
        command.output_file = path;
        StreamCapture file_output{stdout};
        delivered        = write_output_chunk(command, chunk);
        auto file_stdout = file_output.text();
        REQUIRE(delivered);
        CHECK(file_stdout.empty());
        struct stat metadata{};
        REQUIRE(::stat(path.c_str(), &metadata) == 0);
        CHECK((metadata.st_mode & 0777) == 0600);
        auto input = std::ifstream{path, std::ios::binary};
        REQUIRE(input);
        auto bytes = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        CHECK(bytes == raw);

        auto refused = write_output_chunk(command, chunk);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == "jobuctl.output.open_failed");
        auto unchanged = std::ifstream{path, std::ios::binary};
        REQUIRE(unchanged);
        CHECK(std::string{std::istreambuf_iterator<char>{unchanged}, std::istreambuf_iterator<char>{}} == bytes);
        CHECK(command.kind == kind);
    }
}

TEST_CASE("both output command identities preserve empty and unavailable chunk success", "[jobuctl][output]")
{
    auto id = Uuid::parse(fixture_id);
    REQUIRE(id);
    StandardAttributeRegistry registry;
    for (auto kind : {CommandKind::AttemptOutput, CommandKind::RunOutput}) {
        for (auto status :
             {OutputStatus::Available, OutputStatus::Pending, OutputStatus::NotCaptured, OutputStatus::Lost}) {
            jb::test::TemporaryDirectory directory;
            auto                         chunk = AttemptOutputChunk{
                .attempt       = {.run_id = *id, .attempt_number = 3},
                .channel       = OutputChannel::Body,
                .status        = status,
                .total_bytes   = 0,
                .omitted_bytes = 0,
                .capture_lost  = status == OutputStatus::Lost,
                .encoding      = OutputEncoding::Utf8,
            };
            auto encoded = attempt_output_chunk_to_json(chunk);
            REQUIRE(encoded);
            auto          command = Command{.kind = kind, .method = {}};
            StreamCapture human_output{stdout};
            auto          printed = print_command_result(command, ControlReply{chunk}, registry);
            auto          human   = human_output.text();
            REQUIRE(printed);
            CHECK(human.find("status=" + encoded->as_object().at("status").as_string()) != std::string::npos);

            command.json = true;
            StreamCapture json_output{stdout};
            printed   = print_command_result(command, ControlReply{chunk}, registry);
            auto json = json_output.text();
            REQUIRE(printed);
            auto serialized = serialize_json(*encoded);
            REQUIRE(serialized);
            CHECK(json == *serialized + "\n");

            command.json = false;
            command.raw  = true;
            StreamCapture raw_output{stdout};
            auto          delivered = write_output_chunk(command, chunk);
            auto          raw       = raw_output.text();
            REQUIRE(delivered);
            CHECK(raw.empty());

            command.raw         = false;
            command.output_file = directory.path() / "empty.bin";
            REQUIRE(write_output_chunk(command, chunk));
            CHECK(std::filesystem::file_size(*command.output_file) == 0);
        }
    }
}

TEST_CASE("unfinished run output completes locally without delivery in render or capture mode", "[jobuctl][session]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "must-not-exist.bin";
    for (auto mode : {SessionMode::Render, SessionMode::Capture}) {
        auto command =
            command_for(directory.path() / "missing.sock",
                        {"run", "output", fixture_id, "--channel", "stdout", "--output-file", path.string()});
        auto result = run_captured(std::move(command), mode);
        CHECK(result.result.exit_code == 1);
        REQUIRE(result.result.error);
        CHECK(result.result.error->code == "jobuctl.run.output_unavailable");
        CHECK_FALSE(result.result.reply);
        CHECK(result.output.empty());
        CHECK_FALSE(std::filesystem::exists(path));
        if (mode == SessionMode::Capture) {
            check_silent(result);
        }
        else {
            CHECK(result.error.find("run output selection is not available in this build") != std::string::npos);
        }
        CHECK(Application::instance() == nullptr);
        CHECK(ThreadCtx::current()->event_loop() == nullptr);
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

TEST_CASE("human run inspection handles an empty attempt page without fetching details", "[jobuctl][inspection]")
{
    auto steps = inspection_steps(inspection_run(), inspection_attempt());
    auto empty = attempt_page_to_json(AttemptPage{});
    REQUIRE(empty);
    steps[2].value = *empty;
    steps.resize(3);
    ScriptedPeer peer{std::move(steps)};
    auto         rendered = run_captured(command_for(peer.path(), {"run", "get", fixture_id}), SessionMode::Render);
    peer.verify();
    CHECK(rendered.result.exit_code == 0);
    CHECK_FALSE(rendered.result.reply);
    CHECK_FALSE(rendered.result.error);
    CHECK(rendered.error.empty());
    CHECK(rendered.output.starts_with("Run " + std::string{fixture_id} + ": state=failed"));
    check_once(rendered.output, "Last attempt: none\n");
    check_once(rendered.output, "jobuctl run output " + std::string{fixture_id} + " --channel stderr");
}

TEST_CASE("human run inspection renders the newest attempt with bounded runner diagnostics", "[jobuctl][inspection]")
{
    for (auto type : {JobType::Cli, JobType::Http}) {
        for (auto state : {AttemptState::Pending, AttemptState::Running, AttemptState::Completed}) {
            for (auto failed : {false, true}) {
                CAPTURE(type, state, failed);
                auto run_state = RunState::RetryWait;
                if (state == AttemptState::Completed) {
                    run_state = failed ? RunState::Failed : RunState::Succeeded;
                }
                auto run     = inspection_run(type, run_state);
                auto attempt = inspection_attempt(state);
                if (state == AttemptState::Completed) {
                    attempt.outcome = failed ? AttemptOutcome::Failed : AttemptOutcome::Succeeded;
                }
                // Even a hostile result attached to an incomplete attempt must not invent completion diagnostics.
                auto result = parse_json(
                    type == JobType::Cli
                        ? R"({"exit_code":17,"outcome":"unexpected_exit","message":"ATTEMPT_CONTENT","data":"ATTEMPT_CONTENT"})"
                        : R"({"status":503,"outcome":"unexpected_status","error_category":"timeout","message":"ATTEMPT_CONTENT","body":"ATTEMPT_CONTENT"})");
                REQUIRE(result);
                attempt.result     = *result;
                auto         steps = inspection_steps(run, attempt);
                // The page has a cursor to older attempts: human inspection still requests only number 7.
                ScriptedPeer peer{std::move(steps)};
                auto rendered = run_captured(command_for(peer.path(), {"run", "get", fixture_id}), SessionMode::Render);
                peer.verify();
                CHECK(rendered.result.exit_code == 0);
                CHECK_FALSE(rendered.result.error);
                CHECK(rendered.error.empty());
                check_once(rendered.output, "Last attempt: 7:");
                auto expected = std::string_view{"state=pending, outcome=none"};
                if (state == AttemptState::Running) {
                    expected = "state=running, outcome=none";
                }
                else if (state == AttemptState::Completed) {
                    expected = failed ? "state=completed, outcome=failed" : "state=completed, outcome=succeeded";
                }
                CHECK(rendered.output.find(expected) != std::string::npos);
                auto const diagnostic = std::string_view{type == JobType::Cli ? "exit_code=17" : "status=503"};
                CHECK((rendered.output.find(diagnostic) != std::string::npos) == (state == AttemptState::Completed));
                CHECK(rendered.output.find("ATTEMPT_CONTENT") == std::string::npos);
                CHECK(rendered.output.find(type == JobType::Cli ? "--channel stderr" : "--channel body") !=
                      std::string::npos);
                CHECK(rendered.output.find("older than a running retry") != std::string::npos);
            }
        }
    }
}

TEST_CASE("inspection details supersede the earlier attempt snapshot", "[jobuctl][inspection]")
{
    auto completed = inspection_attempt();
    auto result    = parse_json(R"({"exit_code":9})");
    REQUIRE(result);
    completed.result = *result;
    auto steps =
        inspection_steps(inspection_run(JobType::Cli, RunState::RetryWait), inspection_attempt(AttemptState::Running));
    auto details = attempt_details_to_json(completed);
    REQUIRE(details);
    steps[3].value = *details;
    ScriptedPeer peer{std::move(steps)};
    auto         rendered = run_captured(command_for(peer.path(), {"run", "get", fixture_id}), SessionMode::Render);
    peer.verify();
    CHECK(rendered.result.exit_code == 0);
    CHECK(rendered.error.empty());
    check_once(rendered.output, "Last attempt: 7: state=completed, outcome=failed");
    CHECK(rendered.output.find("state=running") == std::string::npos);
    CHECK(rendered.output.find("exit_code=9") != std::string::npos);
}

TEST_CASE("run inspection retains each successful partial result on query failure", "[jobuctl][inspection]")
{
    for (auto phase : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        for (auto action : {PeerAction::RemoteError, PeerAction::Reply, PeerAction::Silence, PeerAction::Disconnect}) {
            CAPTURE(phase, action);
            auto steps = inspection_steps(inspection_run(), inspection_attempt(AttemptState::Running));
            steps.resize(phase + 1);
            auto& failure  = steps.back();
            failure.action = action;
            failure.value  = JsonValue{}; // A null result is invalid for each of these three typed methods.
            if (action == PeerAction::RemoteError) {
                auto error = parse_json(
                    R"({"code":-32000,"message":"retention removed fixture","data":{"category":"not_found","code":"jobu.fixture.removed"}})");
                REQUIRE(error);
                failure.value = *error;
            }
            ScriptedPeer peer{std::move(steps)};
            auto rendered = run_captured(command_for(peer.path(), {"run", "get", fixture_id, "--timeout", "200"}),
                                         SessionMode::Render);
            peer.verify();
            CHECK(rendered.result.exit_code == (action == PeerAction::RemoteError ? 1 : 3));
            REQUIRE(rendered.result.error);
            CHECK_FALSE(rendered.result.error->outcome_unknown);
            CHECK_FALSE(rendered.result.reply);
            CHECK_FALSE(rendered.error.empty());
            CHECK((rendered.output.find("Run " + std::string{fixture_id}) != std::string::npos) == (phase > 1));
            if (phase == 3) {
                check_once(rendered.output, "Last attempt: 7: state=running, outcome=none");
            }
            else {
                CHECK(rendered.output.find("Last attempt:") == std::string::npos);
            }
            if (action == PeerAction::RemoteError) {
                CHECK(rendered.result.error->code == "jobu.fixture.removed");
                check_once(rendered.error, "retention removed fixture");
            }
            else if (action == PeerAction::Reply) {
                CHECK(rendered.result.error->code == "jobu.client.invalid_response");
            }
            else if (action == PeerAction::Silence) {
                CHECK(rendered.result.error->code == "jobu.client.timeout");
            }
        }
    }
}

TEST_CASE("run inspection preserves partial data on synchronous capability rejection", "[jobuctl][inspection]")
{
    for (auto phase : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        CAPTURE(phase);
        auto  steps        = inspection_steps(inspection_run(), inspection_attempt());
        auto  missing      = steps[phase].method;
        auto& fields       = std::get<JsonValue::Object>(steps[0].value.data);
        auto& capabilities = std::get<JsonValue::Array>(fields.at("capabilities").data);
        std::erase_if(capabilities, [&](JsonValue const& item) { return item.as_string() == missing; });
        steps.resize(phase);
        ScriptedPeer peer{std::move(steps)};
        auto         rendered = run_captured(command_for(peer.path(), {"run", "get", fixture_id}), SessionMode::Render);
        peer.verify();
        CHECK(rendered.result.exit_code == 1);
        REQUIRE(rendered.result.error);
        CHECK(rendered.result.error->code == "jobu.client.unsupported_method");
        CHECK_FALSE(rendered.result.error->outcome_unknown);
        CHECK((rendered.output.find("Run " + std::string{fixture_id}) != std::string::npos) == (phase > 1));
        if (phase == 3) {
            check_once(rendered.output, "Last attempt: 7: state=completed, outcome=failed");
        }
        else {
            CHECK(rendered.output.find("Last attempt:") == std::string::npos);
        }
    }
}

TEST_CASE("run inspection rejects mismatched identities and oversized attempt pages", "[jobuctl][inspection]")
{
    auto other = Uuid::parse("00112233-4455-6677-8899-aabbccddee00");
    REQUIRE(other);
    for (auto const* scenario : {"run identity", "summary identity", "page limit", "details run", "details number"}) {
        CAPTURE(scenario);
        auto run     = inspection_run();
        auto attempt = inspection_attempt();
        auto steps   = inspection_steps(run, attempt);
        auto phase   = std::size_t{3};
        if (std::string_view{scenario} == "run identity") {
            run.id       = *other;
            auto encoded = run_details_to_json(run, StandardAttributeRegistry{});
            REQUIRE(encoded);
            steps[1].value = *encoded;
            phase          = 1;
        }
        else if (std::string_view{scenario} == "summary identity" || std::string_view{scenario} == "page limit") {
            auto page = AttemptPage{.items = {attempt}};
            if (std::string_view{scenario} == "summary identity") {
                page.items[0].run_id = *other;
            }
            else {
                --attempt.attempt_number;
                page.items.push_back(attempt);
            }
            auto encoded = attempt_page_to_json(page);
            REQUIRE(encoded);
            steps[2].value = *encoded;
            phase          = 2;
        }
        else {
            if (std::string_view{scenario} == "details run") {
                attempt.run_id = *other;
            }
            else {
                ++attempt.attempt_number;
            }
            auto encoded = attempt_details_to_json(attempt);
            REQUIRE(encoded);
            steps[3].value = *encoded;
        }
        steps.resize(phase + 1);
        ScriptedPeer peer{std::move(steps)};
        auto         rendered = run_captured(command_for(peer.path(), {"run", "get", fixture_id}), SessionMode::Render);
        peer.verify();
        CHECK(rendered.result.exit_code == 3);
        REQUIRE(rendered.result.error);
        CHECK(rendered.result.error->code == "jobu.client.invalid_response");
        if (phase == 3) {
            check_once(rendered.output, "Last attempt: 7:");
        }
        else {
            CHECK(rendered.output.find("Last attempt:") == std::string::npos);
        }
    }
}

TEST_CASE("JSON and captured run get retain their single-call contracts", "[jobuctl][inspection]")
{
    for (auto mode : {SessionMode::Capture, SessionMode::Render}) {
        for (auto json : {false, true}) {
            if (mode == SessionMode::Render && !json) {
                continue;
            }
            CAPTURE(mode, json);
            auto steps       = inspection_steps(inspection_run(), inspection_attempt());
            auto encoded_run = steps[1].value;
            // A peer without enrichment capabilities still supports both fast paths.
            steps[0].value   = system_info_to_json({
                .daemon_version = "run-only",
                .api_version    = {.major = 1,    .minor = 5},
                .capabilities   = {"system.info", "run.get" }
            });
            steps.resize(2);
            ScriptedPeer peer{std::move(steps)};
            auto         arguments = std::vector<std::string>{"run", "get", fixture_id};
            if (json) {
                arguments.emplace_back("--json");
            }
            auto captured = run_captured(command_for(peer.path(), std::move(arguments)), mode);
            peer.verify();
            CHECK(captured.result.exit_code == 0);
            CHECK_FALSE(captured.result.error);
            CHECK(captured.error.empty());
            if (mode == SessionMode::Capture) {
                check_silent(captured);
                REQUIRE(captured.result.reply);
                CHECK(std::get<RunDetails>(*captured.result.reply).state == RunState::Failed);
            }
            else {
                auto value = parse_json(captured.output);
                REQUIRE(value);
                CHECK(serialize_json(*value).value() == serialize_json(encoded_run).value());
                CHECK_FALSE(captured.result.reply);
            }
        }
    }
}

TEST_CASE("run inspection keeps the initial deadline through every query and finishes once", "[jobuctl][inspection]")
{
    StandardAttributeRegistry    registry;
    StreamCapture                output{stdout};
    StreamCapture                error{stderr};
    std::vector<TimePoint>       observed_deadlines;
    std::optional<TimePoint>     initial_deadline;
    std::optional<SessionResult> result;
    auto                         finishes = 0;
    {
        Application app{0, nullptr};
        auto*       loop = ThreadCtx::current()->event_loop();
        REQUIRE(loop);
        auto steps          = inspection_steps(inspection_run(), inspection_attempt(AttemptState::Running));
        steps.back().action = PeerAction::Silence;
        ScriptedPeer peer{
            std::move(steps),
            RequestObservation{.loop = loop, .observe = [&](std::string const& method) {
                                   auto deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*loop);
                                   CHECK(deadline);
                                   if (deadline && initial_deadline) {
                                       CHECK(*deadline <= *initial_deadline);
                                   }
                                   if (deadline) {
                                       observed_deadlines.push_back(*deadline);
                                   }
                                   if (method == "attempt.get") {
                                       // Expire the original session timer deterministically while the final call is
                                       // pending.
                                       jb::core::priv::EventLoopTestAccess::fire_next_timer(*loop);
                                   }
                               }}
        };
        Session session{command_for(peer.path(), {"run", "get", fixture_id}), registry};
        session.finished.connect(&app, [&](int code) {
            ++finishes;
            static_cast<void>(app.quit(code));
        });
        session.start();
        initial_deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*loop);
        REQUIRE(initial_deadline);
        CHECK(app.exec() == 3);
        result = session.take_result();
        CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*loop) == 0);
        peer.verify();
    }
    auto error_text  = error.text();
    auto output_text = output.text();
    CHECK(observed_deadlines.size() == 4);
    CHECK(finishes == 1);
    REQUIRE(result);
    CHECK(result->exit_code == 3);
    REQUIRE(result->error);
    CHECK(result->error->code == "jobu.client.timeout");
    CHECK_FALSE(result->error->outcome_unknown);
    check_once(error_text, "Overall command deadline expired");
    check_once(output_text, "Last attempt: 7: state=running, outcome=none");
}

TEST_CASE("duplicate inspection replies cannot repeat partial output or completion", "[jobuctl][inspection]")
{
    auto steps = inspection_steps(inspection_run(), inspection_attempt());
    for (auto& step : steps) {
        step.action = PeerAction::Duplicate;
    }
    ScriptedPeer              peer{std::move(steps)};
    StandardAttributeRegistry registry;
    StreamCapture             output{stdout};
    StreamCapture             error{stderr};
    auto                      finishes = 0;
    {
        Application app{0, nullptr};
        Session     session{command_for(peer.path(), {"run", "get", fixture_id}), registry};
        session.finished.connect(&app, [&](int code) {
            ++finishes;
            static_cast<void>(app.quit(code));
            session.start();
        });
        session.start();
        CHECK(app.exec() == 0);
        auto result = session.take_result();
        REQUIRE(result);
        CHECK(result->exit_code == 0);
        CHECK_FALSE(result->error);
        CHECK_FALSE(session.take_result());
    }
    auto error_text  = error.text();
    auto output_text = output.text();
    peer.verify();
    CHECK(error_text.empty());
    CHECK(finishes == 1);
    check_once(output_text, "Run " + std::string{fixture_id});
    check_once(output_text, "Last attempt: 7:");
    check_once(output_text, "jobuctl run output");
}
