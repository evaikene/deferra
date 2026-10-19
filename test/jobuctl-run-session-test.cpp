#include "run_session_priv.hpp"

#include "application.hpp"
#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "control_json.hpp"
#include "event_loop.hpp"
#include "event_thread.hpp"
#include "framing.hpp"
#include "history_json.hpp"
#include "input_priv.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "management_json.hpp"
#include "output_priv.hpp"
#include "session_priv.hpp"
#include "statistics_json.hpp"
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
        .api_version    = {.major = 1, .minor = 6},
        .capabilities   = {"system.info",
                           "job.get", "queue.list",
                           "queue.delete", "queue.suspend",
                           "queue.resume", "queue.update",
                           "queue.stats", "queue.get",
                           "run.cancel", "run.get",
                           "attempt.list", "attempt.get",
                           "attempt.output"}
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

auto output_attempt(AttemptNumber number, AttemptState state = AttemptState::Completed) -> AttemptSummary
{
    auto attempt           = inspection_attempt(state);
    attempt.attempt_number = number;
    return attempt;
}

auto fixture_output_request(OutputChannel channel = OutputChannel::Stdout) -> AttemptOutputRequest
{
    auto id = Uuid::parse(fixture_id);
    REQUIRE(id);
    return {
        .attempt = {.run_id = *id, .attempt_number = 7},
        .channel = channel,
        .offset  = 2,
        .limit   = 4
    };
}

auto fixture_output_chunk(AttemptOutputRequest const& request) -> AttemptOutputChunk
{
    return {
        .attempt        = request.attempt,
        .channel        = request.channel,
        .status         = OutputStatus::Available,
        .offset         = request.offset,
        .bytes_returned = 4,
        .next_offset    = request.offset + 4,
        .retained_bytes = request.offset + 8,
        .total_bytes    = request.offset + 12,
        .omitted_bytes  = 4,
        .truncated      = true,
        .encoding       = OutputEncoding::Base64,
        .data           = {std::byte{'A'}, std::byte{0}, std::byte{0xff}, std::byte{'Z'}}
    };
}

auto output_command(std::filesystem::path const& path,
                    AttemptOutputRequest const&  request,
                    std::vector<std::string>     options = {}) -> Command
{
    auto encoded = attempt_output_request_to_json(request);
    REQUIRE(encoded);
    auto arguments = std::vector<std::string>{"run",
                                              "output",
                                              request.attempt.run_id.to_string(),
                                              "--channel",
                                              encoded->as_object().at("channel").as_string(),
                                              "--offset",
                                              std::to_string(request.offset),
                                              "--limit",
                                              std::to_string(request.limit)};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return command_for(path, std::move(arguments));
}

/// Script exact wire parameters, including cursor-only continuations and an independently specified selected key.
/// A supplied chunk ends the script: any fallback, attempt.get, or automatic next_offset read is unexpected.
auto output_steps(RunDetails const&                        run,
                  std::vector<AttemptPage> const&          pages,
                  AttemptOutputRequest const&              request,
                  std::optional<AttemptOutputChunk> const& chunk = std::nullopt) -> std::vector<PeerStep>
{
    auto encoded_run = run_details_to_json(run, StandardAttributeRegistry{});
    auto run_params  = run_get_request_to_json(request.attempt.run_id);
    REQUIRE(encoded_run);
    REQUIRE(run_params);
    auto steps = std::vector<PeerStep>{
        handshake(),
        {.method = "run.get", .value = *encoded_run, .params = *run_params}
    };
    for (auto index = std::size_t{0}; index < pages.size(); ++index) {
        auto params = attempt_list_request_to_json(AttemptQuery{.run_id = request.attempt.run_id, .limit = 200});
        if (index > 0) {
            REQUIRE(pages[index - 1].next_cursor);
            params = attempt_list_request_to_json(CursorRequest{.cursor = *pages[index - 1].next_cursor});
        }
        auto encoded = attempt_page_to_json(pages[index]);
        REQUIRE(params);
        REQUIRE(encoded);
        steps.push_back({.method = "attempt.list", .value = *encoded, .params = *params});
    }
    if (chunk) {
        auto encoded = attempt_output_chunk_to_json(*chunk);
        auto params  = attempt_output_request_to_json(request);
        REQUIRE(encoded);
        REQUIRE(params);
        steps.push_back({.method = "attempt.output", .value = *encoded, .params = *params});
    }
    return steps;
}

void check_once(std::string const& text, std::string_view marker)
{
    auto const position = text.find(marker);
    CHECK(position != std::string::npos);
    CHECK(position == text.rfind(marker));
}

auto fixture_queue(std::string name = "reports", std::string_view identity = fixture_id) -> Queue
{
    auto id = Uuid::parse(identity);
    auto at = parse_utc_timestamp("2030-01-01T00:00:00Z");
    REQUIRE(id);
    REQUIRE(at);
    return {.id = *id, .revision = 7, .name = std::move(name), .created_at = *at, .updated_at = *at};
}

auto queue_json(Queue const& queue) -> JsonValue
{
    auto value = queue_to_json(queue, StandardAttributeRegistry{});
    REQUIRE(value);
    return *value;
}

auto queue_failure(std::string code = "jobu.queue.not_found", std::string category = "not_found") -> JsonValue
{
    auto data = JsonValue::Object{
        {"code",     JsonValue{.data = std::move(code)}    },
        {"category", JsonValue{.data = std::move(category)}}
    };
    return {
        .data = JsonValue::Object{{"code", JsonValue{.data = std::int64_t{-32000}}},
                                  {"message", JsonValue{.data = std::string{"fixture queue failure"}}},
                                  {"data", JsonValue{.data = std::move(data)}}}
    };
}

/// Scripts a lookup with exact wire selector/visibility; nullopt means only the domain not-found response.
auto queue_lookup(QueueSelector selector, std::optional<Queue> const& queue, bool historical = false) -> PeerStep
{
    auto params = queue_get_request_to_json({.queue = std::move(selector), .include_deleted = historical});
    REQUIRE(params);
    return {.method = "queue.get",
            .value  = queue ? queue_json(*queue) : queue_failure(),
            .action = queue ? PeerAction::Reply : PeerAction::RemoteError,
            .params = *params};
}

auto queue_mutation(std::string method, Queue const& queue) -> PeerStep
{
    auto params = queue_selector_to_json(QueueSelector{queue.id});
    REQUIRE(params);
    auto const deleting = method == "queue.delete";
    return {.method = std::move(method), .value = deleting ? JsonValue{} : queue_json(queue), .params = *params};
}

auto statistics_json() -> JsonValue
{
    auto from = parse_utc_timestamp("2030-01-01T00:00:00Z");
    auto to   = parse_utc_timestamp("2030-01-02T00:00:00Z");
    REQUIRE(from);
    REQUIRE(to);
    auto page = statistics_page_to_json({
        .window = {.from = *from, .to = *to},
        .groups = {StatisticsGroup{}}
    });
    REQUIRE(page);
    return *page;
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
        {handshake(),
         queue_lookup(std::string{"reports"}, fixture_queue()),
         {.method = "queue.delete", .value = *error, .action = PeerAction::RemoteError}}
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

    // Older peers fail the revision contract; current peers still report a missing advertised capability.
    for (auto minor : {2U, 6U}) {
        CAPTURE(minor);
        auto info  = handshake();
        info.value = system_info_to_json({
            .daemon_version = "limited-fixture",
            .api_version    = {.major = 1, .minor = minor},
            .capabilities   = {"system.info"}
        });
        ScriptedPeer unsupported{{std::move(info)}};
        auto         local = run_captured(command_for(unsupported.path(), {"queue", "list"}));
        unsupported.verify();
        check_silent(local);
        CHECK(local.result.exit_code == 1);
        REQUIRE(local.result.error);
        CHECK(local.result.error->kind == "local");
        CHECK(local.result.error->code ==
              (minor < 6U ? "jobu.client.unsupported_feature" : "jobu.client.unsupported_method"));
    }
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
            {handshake(),
             queue_lookup(std::string{"reports"}, fixture_queue()),
             {.method = "queue.delete", .value = {}, .action = PeerAction::Disconnect}}
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

    // Exercise delivery directly, independently of the selection and network orchestration tests below.
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

TEST_CASE("run output connection failures never create output files in render or capture mode", "[jobuctl][session]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "must-not-exist.bin";
    for (auto mode : {SessionMode::Render, SessionMode::Capture}) {
        auto command =
            command_for(directory.path() / "missing.sock",
                        {"run", "output", fixture_id, "--channel", "stdout", "--output-file", path.string()});
        auto result = run_captured(std::move(command), mode);
        CHECK(result.result.exit_code == 3);
        REQUIRE(result.result.error);
        CHECK(result.result.error->code == "jobuctl.connection_failed");
        CHECK_FALSE(result.result.reply);
        CHECK(result.output.empty());
        CHECK_FALSE(std::filesystem::exists(path));
        if (mode == SessionMode::Capture) {
            check_silent(result);
        }
        else {
            CHECK(result.error.find("Daemon socket connection failed") != std::string::npos);
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
             {.method = "queue.get", .value = *pending},
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

TEST_CASE("run output selects the latest completed attempt across bounded descending pages", "[jobuctl][run-output]")
{
    auto pages = std::vector<AttemptPage>{
        {.items = {output_attempt(7), output_attempt(6)}, .next_cursor = "unused"}
    };
    SECTION("latest attempt completed")
    {}
    SECTION("multiple newer attempts pending or running")
    {
        pages.front().items = {output_attempt(10, AttemptState::Pending),
                               output_attempt(9, AttemptState::Running),
                               output_attempt(8, AttemptState::Pending),
                               output_attempt(7),
                               output_attempt(6)};
    }
    SECTION("short pages continue with the cursor alone")
    {
        pages = {
            {.items       = {output_attempt(10, AttemptState::Pending), output_attempt(9, AttemptState::Running)},
             .next_cursor = "first"                                                                                                       },
            {.items = {output_attempt(8, AttemptState::Pending)},                                                  .next_cursor = "second"},
            {.items = {output_attempt(7), output_attempt(6)},                                                      .next_cursor = "unused"}
        };
    }
    SECTION("a full maximum-sized page can contain no completed attempt")
    {
        auto full = AttemptPage{.next_cursor = "older"};
        for (auto number = AttemptNumber{207}; number > 7; --number) {
            full.items.push_back(output_attempt(number, AttemptState::Pending));
        }
        REQUIRE(full.items.size() == 200);
        pages.insert(pages.begin(), std::move(full));
    }

    auto         request = fixture_output_request();
    auto         chunk   = fixture_output_chunk(request);
    ScriptedPeer peer{output_steps(inspection_run(JobType::Cli, RunState::Running), pages, request, chunk)};
    auto         result = run_captured(output_command(peer.path(), request), SessionMode::Render);
    peer.verify();
    CHECK(result.result.exit_code == 0);
    CHECK_FALSE(result.result.error);
    CHECK(result.error.empty());
    check_once(result.output, "Output " + std::string{fixture_id} + ":7 channel=stdout, status=available");
    CHECK(result.output.find("Last attempt:") == std::string::npos);
    CHECK(result.output.find("jobuctl run output") == std::string::npos);
    CHECK(Application::instance() == nullptr);
}

TEST_CASE("composed run output preserves every delivery mode and channel", "[jobuctl][run-output]")
{
    for (auto channel : {OutputChannel::Stdout, OutputChannel::Stderr, OutputChannel::Body, OutputChannel::Headers}) {
        for (auto encoding : {OutputEncoding::Utf8, OutputEncoding::Base64}) {
            for (auto const* delivery : {"human", "json", "raw", "file", "capture"}) {
                CAPTURE(channel, encoding, delivery);
                jb::test::TemporaryDirectory directory;
                auto const                   path    = directory.path() / "chunk.bin";
                auto                         request = fixture_output_request(channel);
                auto                         chunk   = fixture_output_chunk(request);
                chunk.encoding                       = encoding;
                if (encoding == OutputEncoding::Utf8) {
                    chunk.data = {std::byte{'t'}, std::byte{'e'}, std::byte{'x'}, std::byte{'t'}};
                }
                auto type =
                    channel == OutputChannel::Body || channel == OutputChannel::Headers ? JobType::Http : JobType::Cli;
                ScriptedPeer peer{output_steps(inspection_run(type), {{.items = {output_attempt(7)}}}, request, chunk)};
                auto         command  = output_command(peer.path(), request);
                auto         mode     = SessionMode::Render;
                auto const   selected = std::string_view{delivery};
                if (selected == "json") {
                    command.json = true;
                }
                else if (selected == "raw") {
                    command.raw = true;
                }
                else if (selected == "file" || selected == "capture") {
                    command.output_file = path;
                    if (selected == "capture") {
                        mode = SessionMode::Capture;
                    }
                }
                auto result = run_captured(std::move(command), mode);
                peer.verify();
                CHECK(result.result.exit_code == 0);
                CHECK_FALSE(result.result.error);
                CHECK(result.error.empty());
                auto encoded = attempt_output_chunk_to_json(chunk);
                REQUIRE(encoded);
                auto serialized = serialize_json(*encoded);
                REQUIRE(serialized);
                auto const bytes = encoding == OutputEncoding::Utf8 ? std::string{"text"} : std::string{"A\0\xffZ", 4};

                if (selected == "capture") {
                    check_silent(result);
                    REQUIRE(result.result.reply);
                    auto const& captured = std::get<AttemptOutputChunk>(*result.result.reply);
                    CHECK(captured.data == chunk.data);
                    CHECK(attempt_output_chunk_to_json(captured)->as_object() == encoded->as_object());
                    CHECK_FALSE(std::filesystem::exists(path));
                }
                else if (selected == "json") {
                    CHECK(result.output == *serialized + "\n");
                }
                else if (selected == "raw") {
                    CHECK(result.output == bytes);
                }
                else if (selected == "file") {
                    CHECK(result.output.empty());
                    auto input = std::ifstream{path, std::ios::binary};
                    REQUIRE(input);
                    CHECK(std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}} ==
                          bytes);
                    struct stat metadata{};
                    REQUIRE(::stat(path.c_str(), &metadata) == 0);
                    CHECK((metadata.st_mode & 0777) == 0600);
                }
                else {
                    check_once(result.output, "Output " + std::string{fixture_id} + ":7 channel=");
                    CHECK(result.output.find("status=available") != std::string::npos);
                    CHECK(result.output.find("truncated=true") != std::string::npos);
                }
            }
        }
    }
}

TEST_CASE("selected empty and unavailable run output chunks succeed without fallback", "[jobuctl][run-output]")
{
    for (auto status :
         {OutputStatus::Available, OutputStatus::Pending, OutputStatus::NotCaptured, OutputStatus::Lost}) {
        CAPTURE(status);
        auto request   = fixture_output_request(OutputChannel::Body);
        request.offset = 0;
        auto chunk     = AttemptOutputChunk{.attempt      = request.attempt,
                                            .channel      = request.channel,
                                            .status       = status,
                                            .capture_lost = status == OutputStatus::Lost};
        auto encoded   = attempt_output_chunk_to_json(chunk);
        REQUIRE(encoded);
        // An older attempt is available, but the selected channel's status must never trigger a second read.
        ScriptedPeer peer{
            output_steps(inspection_run(JobType::Http),
                         {{.items = {output_attempt(7), output_attempt(6)}, .next_cursor = "unused"}},
                         request,
                         chunk)
        };
        auto result = run_captured(output_command(peer.path(), request), SessionMode::Render);
        peer.verify();
        CHECK(result.result.exit_code == 0);
        CHECK_FALSE(result.result.error);
        CHECK(result.error.empty());
        CHECK(result.output.find("status=" + encoded->as_object().at("status").as_string()) != std::string::npos);
    }
}

TEST_CASE("run output distinguishes absent attempts from no completed attempt", "[jobuctl][run-output]")
{
    auto pages    = std::vector<AttemptPage>{{}};
    auto expected = std::string{"jobuctl.run.no_attempts"};
    SECTION("no retained attempts")
    {}
    SECTION("only incomplete attempts")
    {
        pages    = {{.items = {output_attempt(9, AttemptState::Pending), output_attempt(8, AttemptState::Running)}}};
        expected = "jobuctl.run.no_completed_attempt";
    }
    SECTION("all pages contain incomplete attempts")
    {
        pages = {
            {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older"},
            {.items = {output_attempt(8, AttemptState::Pending)}}
        };
        expected = "jobuctl.run.no_completed_attempt";
    }
    SECTION("retention empties a continuation after an incomplete attempt was observed")
    {
        pages = {
            {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older"},
            {}
        };
        expected = "jobuctl.run.no_completed_attempt";
    }

    for (auto mode : {SessionMode::Render, SessionMode::Capture}) {
        jb::test::TemporaryDirectory directory;
        auto const                   path    = directory.path() / "must-not-exist.bin";
        auto                         request = fixture_output_request();
        ScriptedPeer                 peer{output_steps(inspection_run(), pages, request)};
        auto result = run_captured(output_command(peer.path(), request, {"--output-file", path.string()}), mode);
        peer.verify();
        CHECK(result.result.exit_code == 1);
        REQUIRE(result.result.error);
        CHECK(result.result.error->code == expected);
        CHECK_FALSE(result.result.reply);
        CHECK(result.output.empty());
        CHECK_FALSE(std::filesystem::exists(path));
        if (mode == SessionMode::Capture) {
            check_silent(result);
        }
        else {
            CHECK(result.error.find(expected == "jobuctl.run.no_attempts"
                                        ? "no retained attempts"
                                        : "no completed retained attempt") != std::string::npos);
        }
    }
}

TEST_CASE("run output rejects a channel incompatible with the fetched run", "[jobuctl][run-output]")
{
    for (auto channel : {OutputChannel::Stdout, OutputChannel::Stderr, OutputChannel::Body, OutputChannel::Headers}) {
        CAPTURE(channel);
        auto type = channel == OutputChannel::Body || channel == OutputChannel::Headers ? JobType::Cli : JobType::Http;
        auto request = fixture_output_request(channel);
        ScriptedPeer peer{output_steps(inspection_run(type), {}, request)};
        auto         result = run_captured(output_command(peer.path(), request, {"--json"}), SessionMode::Render);
        peer.verify();
        CHECK(result.result.exit_code == 2);
        CHECK(result.output.empty());
        REQUIRE(result.result.error);
        CHECK(result.result.error->code == "jobuctl.run.invalid_channel");
        auto error = parse_json(result.error);
        REQUIRE(error);
        CHECK(error->as_object().at("error").as_object().at("code").as_string() == "jobuctl.run.invalid_channel");
    }
}

TEST_CASE("run output rejects invalid page identity order and continuation progress", "[jobuctl][run-output]")
{
    auto other = Uuid::parse("00112233-4455-6677-8899-aabbccddee00");
    REQUIRE(other);
    auto pages = std::vector<AttemptPage>{{.items = {output_attempt(7), output_attempt(6)}}};
    SECTION("identity after the first completed member is still validated")
    {
        pages.front().items.back().run_id = *other;
    }
    SECTION("duplicate attempt numbers within a page")
    {
        pages.front().items.back().attempt_number = 7;
    }
    SECTION("ascending attempt numbers within a page")
    {
        pages.front().items.back().attempt_number = 8;
    }
    SECTION("overlapping page boundary")
    {
        pages = {
            {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older"},
            {.items = {output_attempt(9)}}
        };
    }
    SECTION("ascending page boundary")
    {
        pages = {
            {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older"},
            {.items = {output_attempt(10)}}
        };
    }
    SECTION("repeated cursor even with decreasing item numbers")
    {
        pages = {
            {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "same"},
            {.items = {output_attempt(8, AttemptState::Pending)}, .next_cursor = "same"}
        };
    }
    SECTION("cursor cycle replays a previously visited page")
    {
        pages = {
            {.items = {output_attempt(10, AttemptState::Running)}, .next_cursor = "first" },
            {.items = {output_attempt(9, AttemptState::Pending)},  .next_cursor = "second"},
            {.items = {output_attempt(10, AttemptState::Running)}, .next_cursor = "first" }
        };
    }

    jb::test::TemporaryDirectory directory;
    auto const                   path    = directory.path() / "must-not-exist.bin";
    auto                         request = fixture_output_request();
    ScriptedPeer                 peer{output_steps(inspection_run(), pages, request)};
    auto                         result =
        run_captured(output_command(peer.path(), request, {"--output-file", path.string()}), SessionMode::Render);
    peer.verify();
    CHECK(result.result.exit_code == 3);
    REQUIRE(result.result.error);
    CHECK(result.result.error->code == "jobu.client.invalid_response");
    CHECK(result.output.empty());
    CHECK_FALSE(result.result.reply);
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("run output correlates run and chunk responses before delivery", "[jobuctl][run-output]")
{
    auto other = Uuid::parse("00112233-4455-6677-8899-aabbccddee00");
    REQUIRE(other);
    auto request = fixture_output_request();
    auto chunk   = fixture_output_chunk(request);
    auto steps   = output_steps(inspection_run(), {{.items = {output_attempt(7)}}}, request, chunk);
    SECTION("run identity")
    {
        std::get<JsonValue::Object>(steps[1].value.data).at("id").data = other->to_string();
        steps.resize(2);
    }
    SECTION("chunk run identity")
    {
        std::get<JsonValue::Object>(steps.back().value.data).at("run_id").data = other->to_string();
    }
    SECTION("chunk attempt number")
    {
        std::get<JsonValue::Object>(steps.back().value.data).at("attempt_number").data = std::uint64_t{6};
    }
    SECTION("chunk channel")
    {
        std::get<JsonValue::Object>(steps.back().value.data).at("channel").data = std::string{"stderr"};
    }
    SECTION("chunk offset")
    {
        --chunk.offset;
        --*chunk.next_offset;
        steps.back().value = *attempt_output_chunk_to_json(chunk);
    }
    SECTION("chunk exceeds requested limit")
    {
        request.limit = 3;
        steps         = output_steps(inspection_run(), {{.items = {output_attempt(7)}}}, request, chunk);
    }
    SECTION("empty page with a cursor is malformed")
    {
        auto& fields = std::get<JsonValue::Object>(steps[2].value.data);
        std::get<JsonValue::Array>(fields.at("items").data).clear();
        fields.at("next_cursor").data = std::string{"invalid"};
        steps.resize(3);
    }
    SECTION("page exceeds maximum size")
    {
        auto& items = std::get<JsonValue::Array>(std::get<JsonValue::Object>(steps[2].value.data).at("items").data);
        items.resize(201, items.front());
        steps.resize(3);
    }
    SECTION("malformed chunk")
    {
        std::get<JsonValue::Object>(steps.back().value.data).erase("data");
    }

    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "must-not-exist.bin";
    ScriptedPeer                 peer{std::move(steps)};
    auto                         result =
        run_captured(output_command(peer.path(), request, {"--output-file", path.string()}), SessionMode::Render);
    peer.verify();
    CHECK(result.result.exit_code == 3);
    REQUIRE(result.result.error);
    CHECK(result.result.error->code == "jobu.client.invalid_response");
    CHECK(result.output.empty());
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("run output preserves remote malformed and transport failures without fallback", "[jobuctl][run-output]")
{
    auto request = fixture_output_request();
    auto chunk   = fixture_output_chunk(request);
    auto pages   = std::vector<AttemptPage>{
        {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older" },
        {.items = {output_attempt(7), output_attempt(6)},     .next_cursor = "unused"}
    };
    for (auto phase : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
        for (auto action : {PeerAction::RemoteError, PeerAction::Reply, PeerAction::Disconnect}) {
            for (auto mode : {SessionMode::Render, SessionMode::Capture}) {
                CAPTURE(phase, action, mode);
                auto steps = output_steps(inspection_run(), pages, request, chunk);
                steps.resize(phase + 1);
                steps.back().action = action;
                steps.back().value  = JsonValue{};
                if (action == PeerAction::RemoteError) {
                    auto error = parse_json(
                        R"({"code":-32000,"message":"retention removed fixture","data":{"category":"not_found","code":"jobu.fixture.removed"}})");
                    REQUIRE(error);
                    steps.back().value = *error;
                }

                jb::test::TemporaryDirectory directory;
                auto const                   path = directory.path() / "must-not-exist.bin";
                ScriptedPeer                 peer{std::move(steps)};
                auto                         result =
                    run_captured(output_command(peer.path(), request, {"--output-file", path.string()}), mode);
                peer.verify();
                CHECK(result.result.exit_code == (action == PeerAction::RemoteError ? 1 : 3));
                REQUIRE(result.result.error);
                CHECK_FALSE(result.result.error->outcome_unknown);
                CHECK_FALSE(result.result.reply);
                CHECK(result.output.empty());
                CHECK_FALSE(std::filesystem::exists(path));
                if (action == PeerAction::RemoteError) {
                    CHECK(result.result.error->kind == "remote");
                    CHECK(result.result.error->code == "jobu.fixture.removed");
                }
                else if (action == PeerAction::Reply) {
                    CHECK(result.result.error->code == "jobu.client.invalid_response");
                }
                if (mode == SessionMode::Capture) {
                    check_silent(result);
                }
            }
        }
    }
}

TEST_CASE("run output preserves synchronous capability failures at each method", "[jobuctl][run-output]")
{
    auto request = fixture_output_request();
    auto chunk   = fixture_output_chunk(request);
    for (auto phase : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        CAPTURE(phase);
        auto  steps   = output_steps(inspection_run(), {{.items = {output_attempt(7)}}}, request, chunk);
        auto  missing = steps[phase].method;
        auto& capabilities =
            std::get<JsonValue::Array>(std::get<JsonValue::Object>(steps[0].value.data).at("capabilities").data);
        std::erase_if(capabilities, [&](JsonValue const& item) { return item.as_string() == missing; });
        steps.resize(phase);
        ScriptedPeer peer{std::move(steps)};
        auto         result = run_captured(output_command(peer.path(), request, {"--json"}), SessionMode::Render);
        peer.verify();
        CHECK(result.result.exit_code == 1);
        REQUIRE(result.result.error);
        CHECK(result.result.error->code == "jobu.client.unsupported_method");
        CHECK(result.output.empty());
        auto error = parse_json(result.error);
        REQUIRE(error);
        CHECK(error->as_object().at("error").as_object().at("code").as_string() == "jobu.client.unsupported_method");
    }
}

TEST_CASE("run output shares one deadline through handshake lookup pages and output", "[jobuctl][run-output]")
{
    auto request = fixture_output_request();
    auto chunk   = fixture_output_chunk(request);
    auto pages   = std::vector<AttemptPage>{
        {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older"},
        {.items = {output_attempt(7)}}
    };
    for (auto phase : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}}) {
        CAPTURE(phase);
        jb::test::TemporaryDirectory directory;
        auto const                   path = directory.path() / "must-not-exist.bin";
        StandardAttributeRegistry    registry;
        StreamCapture                output{stdout};
        StreamCapture                error{stderr};
        std::optional<TimePoint>     initial_deadline;
        std::optional<SessionResult> result;
        auto                         observed = std::size_t{0};
        auto                         finishes = 0;
        {
            Application app{0, nullptr};
            auto*       loop = ThreadCtx::current()->event_loop();
            REQUIRE(loop);
            auto steps = output_steps(inspection_run(), pages, request, chunk);
            steps.resize(phase + 1);
            steps.back().action = PeerAction::Silence;
            ScriptedPeer peer{
                std::move(steps),
                RequestObservation{.loop = loop, .observe = [&](std::string const&) {
                                       auto deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*loop);
                                       REQUIRE(deadline);
                                       REQUIRE(initial_deadline);
                                       CHECK(*deadline <= *initial_deadline);
                                       if (observed++ == phase) {
                                           // Expire the original timer with this call pending; no sleeps or per-call
                                           // budget resets.
                                           jb::core::priv::EventLoopTestAccess::fire_next_timer(*loop);
                                       }
                                   }}
            };
            Session session{output_command(peer.path(), request, {"--output-file", path.string()}), registry};
            session.finished.connect(&app, [&](int code) {
                ++finishes;
                static_cast<void>(app.quit(code));
                session.start();
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
        CHECK(observed == phase + 1);
        CHECK(finishes == 1);
        REQUIRE(result);
        CHECK(result->exit_code == 3);
        REQUIRE(result->error);
        CHECK(result->error->code == "jobu.client.timeout");
        CHECK_FALSE(result->error->outcome_unknown);
        CHECK_FALSE(result->reply);
        CHECK(output_text.empty());
        check_once(error_text, "Overall command deadline expired");
        CHECK_FALSE(std::filesystem::exists(path));
    }
}

TEST_CASE("duplicate run output replies cannot reselect deliver twice or finish twice", "[jobuctl][run-output]")
{
    auto request = fixture_output_request();
    auto chunk   = fixture_output_chunk(request);
    auto steps   = output_steps(inspection_run(JobType::Cli, RunState::Running),
                                {
                                    {.items = {output_attempt(9, AttemptState::Running)}, .next_cursor = "older"},
                                    {.items = {output_attempt(7), output_attempt(6)}}
    },
                                request,
                                chunk);
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
        Session     session{output_command(peer.path(), request), registry};
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
        CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*ThreadCtx::current()->event_loop()) == 0);
    }
    auto error_text  = error.text();
    auto output_text = output.text();
    peer.verify();
    CHECK(error_text.empty());
    CHECK(finishes == 1);
    check_once(output_text, "Output " + std::string{fixture_id} + ":7 channel=stdout");
    CHECK(output_text.find("Last attempt:") == std::string::npos);
}

TEST_CASE("queue inferred UUID selection covers both interpretations without fallback", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue(fixture_id);
    auto const other = fixture_queue(fixture_id, "10112233-4455-6677-8899-aabbccddeeff");

    struct Scenario {
        bool                 id_exists;
        std::optional<Queue> name_match;
        int                  exit;
    };

    auto const scenarios = std::vector<Scenario>{
        {.id_exists = true,  .name_match = std::nullopt, .exit = 0},
        {.id_exists = true,  .name_match = queue,        .exit = 0},
        {.id_exists = true,  .name_match = other,        .exit = 1},
        {.id_exists = false, .name_match = other,        .exit = 1},
        {.id_exists = false, .name_match = std::nullopt, .exit = 1}
    };
    for (auto const& scenario : scenarios) {
        CAPTURE(scenario.id_exists, scenario.name_match.has_value(), scenario.exit);
        ScriptedPeer peer{
            {handshake(),
             queue_lookup(queue.id, scenario.id_exists ? std::optional{queue} : std::nullopt),
             queue_lookup(std::string{fixture_id}, scenario.name_match)}
        };
        auto captured = run_captured(command_for(peer.path(), {"queue", "get", fixture_id}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == scenario.exit);
        if (scenario.exit == 0) {
            REQUIRE(captured.result.reply);
            CHECK(std::get<Queue>(*captured.result.reply).id == queue.id);
        }
        else {
            CHECK_FALSE(captured.result.reply);
            REQUIRE(captured.result.error);
            auto const& error = *captured.result.error;
            CHECK_FALSE(error.outcome_unknown);
            if (scenario.id_exists) {
                CHECK(error.code == "jobuctl.queue.ambiguous_selector");
                CHECK(error.message.find("id:" + std::string{fixture_id}) != std::string::npos);
                CHECK(error.message.find("name:" + std::string{fixture_id}) != std::string::npos);
            }
            else {
                CHECK(error.code == "jobu.queue.not_found");
                CHECK(error.kind == "remote");
                CHECK(error.rpc_code == -32000);
                CHECK(error.category == "not_found");
                CHECK((error.message.find("name:") != std::string::npos) == scenario.name_match.has_value());
            }
        }
    }
}

TEST_CASE("queue probes retain uppercase name bytes and require explicit collision remedies",
          "[jobuctl][queue-selection]")
{
    auto const   upper = std::string{"00112233-4455-6677-8899-AABBCCDDEEFF"};
    auto const   queue = fixture_queue();
    auto const   other = fixture_queue(upper, "10112233-4455-6677-8899-aabbccddeeff");
    ScriptedPeer peer{
        {handshake(), queue_lookup(queue.id, queue), queue_lookup(upper, other)}
    };
    auto rendered = run_captured(command_for(peer.path(), {"queue", "delete", upper, "--json"}), SessionMode::Render);
    peer.verify();
    CHECK(rendered.result.exit_code == 1);
    CHECK(rendered.output.empty());
    auto value = parse_json(rendered.error);
    REQUIRE(value);
    auto const& error = value->as_object().at("error").as_object();
    CHECK(error.at("code").as_string() == "jobuctl.queue.ambiguous_selector");
    CHECK(error.at("message").as_string().find("id:" + upper) != std::string::npos);
    CHECK(error.at("message").as_string().find("name:" + upper) != std::string::npos);
    CHECK_FALSE(error.at("outcome_unknown").as_bool());
}

TEST_CASE("explicit queue aliases and prefixes bypass only inference probes", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue(fixture_id);
    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"id:" + std::string{fixture_id}},
             {"--id", fixture_id},
             {"name:" + std::string{fixture_id}},
             {"--name", fixture_id}
    }) {
        CAPTURE(arguments);
        auto const explicit_id = arguments.front().starts_with("id:") || arguments.front() == "--id";
        auto       steps       = std::vector<PeerStep>{handshake()};
        if (!explicit_id) {
            steps.push_back(queue_lookup(queue.name, queue));
        }
        steps.push_back(queue_mutation("queue.delete", queue));
        ScriptedPeer peer{std::move(steps)};
        auto         command = std::vector<std::string>{"queue", "delete"};
        command.insert(command.end(), arguments.begin(), arguments.end());
        command.emplace_back("--json");
        auto rendered = run_captured(command_for(peer.path(), std::move(command)), SessionMode::Render);
        peer.verify();
        CHECK(rendered.result.exit_code == 0);
        CHECK(rendered.output == "null\n");
        CHECK(rendered.error.empty());
    }
}

TEST_CASE("resolved queue names pin updates lifecycle calls and human acknowledgements", "[jobuctl][queue-selection]")
{
    auto const original = fixture_queue();
    auto       renamed  = original;
    renamed.name        = "renamed-after-selection";
    for (auto const* action : {"update", "suspend", "resume", "delete"}) {
        CAPTURE(action);
        auto command = std::vector<std::string>{"queue", action, "reports"};
        auto final   = queue_mutation("queue." + std::string{action}, renamed);
        if (std::string_view{action} == "update") {
            command.insert(command.end(), {"--revision", "7", "--weight", "4"});
            auto params =
                parse_json(R"({"queue_id":"00112233-4455-6677-8899-aabbccddeeff","expected_revision":7,"weight":4})");
            REQUIRE(params);
            final.params = *params;
        }
        // The peer changes the name between discovery and mutation, accepting only the original UUID on the wire.
        // A replacement queue named reports cannot become the mutation's target.
        ScriptedPeer peer{
            {handshake(), queue_lookup(original.name, original), std::move(final)}
        };
        auto rendered = run_captured(command_for(peer.path(), std::move(command)), SessionMode::Render);
        peer.verify();
        CHECK(rendered.result.exit_code == 0);
        CHECK(rendered.error.empty());
        if (std::string_view{action} == "delete") {
            CHECK(rendered.output == "Deleted queue name=reports\n");
        }
        else {
            check_once(rendered.output, "renamed-after-selection");
        }
    }
}

TEST_CASE("queue get reuses the first result and prints or captures it only once", "[jobuctl][queue-selection]")
{
    auto queue     = fixture_queue(fixture_id);
    auto later     = queue;
    later.revision = 8;
    for (auto const mode : {SessionMode::Capture, SessionMode::Render}) {
        for (auto const inferred : {false, true}) {
            CAPTURE(mode, inferred);
            auto steps = std::vector<PeerStep>{
                handshake(),
                queue_lookup(inferred ? QueueSelector{queue.id} : QueueSelector{queue.name}, queue)};
            if (inferred) {
                steps.push_back(queue_lookup(queue.name, later));
            }
            ScriptedPeer peer{std::move(steps)};
            auto         command =
                std::vector<std::string>{"queue", "get", inferred ? fixture_id : "name:" + queue.name, "--json"};
            auto captured = run_captured(command_for(peer.path(), std::move(command)), mode);
            peer.verify();
            CHECK(captured.result.exit_code == 0);
            if (mode == SessionMode::Capture) {
                check_silent(captured);
                REQUIRE(captured.result.reply);
                CHECK(std::get<Queue>(*captured.result.reply).revision == 7);
            }
            else {
                CHECK(captured.error.empty());
                auto value = parse_json(captured.output);
                REQUIRE(value);
                CHECK(value->as_object().at("revision").as_uint() == 7);
                check_once(captured.output, "\"revision\"");
            }
        }
    }
}

TEST_CASE("queue selection preserves operational errors at either lookup", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue();
    for (auto const first_lookup : {true, false}) {
        for (auto const action : {PeerAction::RemoteError, PeerAction::Disconnect, PeerAction::Reply}) {
            CAPTURE(first_lookup, action);
            auto steps = std::vector<PeerStep>{handshake()};
            if (!first_lookup) {
                steps.push_back(queue_lookup(queue.id, queue));
            }
            auto failure = queue_lookup(first_lookup ? QueueSelector{queue.id} : QueueSelector{std::string{fixture_id}},
                                        std::nullopt);
            failure.action = action;
            // A valid domain failure, disconnect, or malformed result must stop discovery without a final mutation.
            failure.value =
                action == PeerAction::RemoteError ? queue_failure("jobu.fixture.storage", "storage") : JsonValue{};
            steps.push_back(std::move(failure));
            ScriptedPeer peer{std::move(steps)};
            auto         captured = run_captured(command_for(peer.path(), {"queue", "delete", fixture_id}));
            peer.verify();
            check_silent(captured);
            CHECK(captured.result.exit_code == (action == PeerAction::RemoteError ? 1 : 3));
            REQUIRE(captured.result.error);
            CHECK_FALSE(captured.result.error->outcome_unknown);
            CHECK_FALSE(captured.result.reply);
            if (action == PeerAction::RemoteError) {
                CHECK(captured.result.error->code == "jobu.fixture.storage");
            }
        }
    }
}

TEST_CASE("only the application queue not-found error counts as absent", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue();
    for (auto const wrong_category : {false, true}) {
        auto failure = queue_lookup(std::string{fixture_id}, std::nullopt);
        if (wrong_category) {
            failure.value = queue_failure("jobu.queue.not_found", "storage");
        }
        else {
            std::get<JsonValue::Object>(failure.value.data).at("code").data = std::int64_t{-32602};
        }
        ScriptedPeer peer{
            {handshake(), queue_lookup(queue.id, queue), std::move(failure)}
        };
        auto captured = run_captured(command_for(peer.path(), {"queue", "get", fixture_id}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 1);
        REQUIRE(captured.result.error);
        CHECK_FALSE(captured.result.reply);
        CHECK_FALSE(captured.result.error->outcome_unknown);
    }
}

TEST_CASE("queue statistics resolve retained rows and retain every filter", "[jobuctl][queue-selection]")
{
    auto queue      = fixture_queue();
    queue.state     = QueueState::Deleted;
    auto const page = statistics_json();
    for (auto const inferred : {false, true}) {
        CAPTURE(inferred);
        auto steps = std::vector<PeerStep>{
            handshake(),
            queue_lookup(inferred ? QueueSelector{queue.id} : QueueSelector{queue.name}, queue, true)};
        if (inferred) {
            steps.push_back(queue_lookup(std::string{fixture_id}, std::nullopt, true));
        }
        auto params = parse_json(
            R"({"queue_id":"00112233-4455-6677-8899-aabbccddeeff","job_id":"10112233-4455-6677-8899-aabbccddeeff","type":"cli","origin":"manual","planned":{"from":"2030-01-01T00:00:00.000000Z","to":"2030-01-02T00:00:00.000000Z"},"group_by":"state","limit":17})");
        REQUIRE(params);
        steps.push_back({.method = "queue.stats", .value = page, .params = *params});
        ScriptedPeer peer{std::move(steps)};
        auto         captured = run_captured(command_for(peer.path(),
                                                         {"queue",
                                                          "stats",
                                                          inferred ? fixture_id : "reports",
                                                          "--job-id",
                                                          "10112233-4455-6677-8899-aabbccddeeff",
                                                          "--type",
                                                          "cli",
                                                          "--origin",
                                                          "manual",
                                                          "--planned-from",
                                                          "2030-01-01T00:00:00Z",
                                                          "--planned-to",
                                                          "2030-01-02T00:00:00Z",
                                                          "--group-by",
                                                          "state",
                                                          "--limit",
                                                          "17",
                                                          "--json"}),
                                             SessionMode::Render);
        peer.verify();
        CHECK(captured.result.exit_code == 0);
        CHECK(captured.error.empty());
        auto value = parse_json(captured.output);
        REQUIRE(value);
        CHECK(serialize_json(*value).value() == serialize_json(page).value());
    }
}

TEST_CASE("historical ambiguity stops statistics with explicit identity guidance", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue();
    for (auto const id_exists : {false, true}) {
        auto steps =
            std::vector<PeerStep>{handshake(),
                                  queue_lookup(queue.id, id_exists ? std::optional{queue} : std::nullopt, true)};
        auto failure  = queue_lookup(std::string{fixture_id}, std::nullopt, true);
        failure.value = queue_failure("jobu.queue.ambiguous_deleted_name", "conflict");
        steps.push_back(std::move(failure));
        ScriptedPeer peer{std::move(steps)};
        auto         captured = run_captured(command_for(peer.path(), {"queue", "stats", fixture_id}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 1);
        REQUIRE(captured.result.error);
        CHECK(captured.result.error->code == "jobu.queue.ambiguous_deleted_name");
        CHECK(captured.result.error->message.find("id:UUID") != std::string::npos);
    }
}

TEST_CASE("cursor raw requests and explicit IDs keep their original dispatch contract", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue();
    SECTION("cursor-only stats")
    {
        auto params = parse_json(R"({"cursor":"opaque"})");
        REQUIRE(params);
        ScriptedPeer peer{
            {handshake(), {.method = "queue.stats", .value = statistics_json(), .params = *params}}
        };
        auto captured = run_captured(command_for(peer.path(), {"queue", "stats", "--cursor", "opaque"}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 0);
    }
    SECTION("raw name and prefix-looking strings are sent literally")
    {
        for (auto const* action : {"get", "update", "suspend", "resume", "delete", "stats"}) {
            CAPTURE(action);
            // Load through the actual request-file path; Session must not strip prefixes or preflight.
            auto text  = std::string_view{R"({"queue_name":"name:reports"})"};
            auto value = queue_json(queue);
            if (std::string_view{action} == "get") {
                text = R"({"queue_name":"name:reports","include_deleted":true})";
            }
            else if (std::string_view{action} == "update") {
                text = R"({"queue_name":"name:reports","expected_revision":7,"weight":4})";
            }
            else if (std::string_view{action} == "stats") {
                text  = R"({"queue_name":"name:reports","group_by":"none","limit":100,"planned":{}})";
                value = statistics_json();
            }
            else if (std::string_view{action} == "delete") {
                value = JsonValue{};
            }
            auto params = parse_json(text);
            REQUIRE(params);
            ScriptedPeer peer{
                {handshake(),
                 {.method = "queue." + std::string{action}, .value = std::move(value), .params = *params}}
            };
            jb::test::TemporaryDirectory directory;
            auto                         path    = directory.path() / "request.json";
            auto                         encoded = serialize_json(*params);
            REQUIRE(encoded);
            {
                std::ofstream file{path};
                file << *encoded;
                REQUIRE(file.good());
            }
            auto command = command_for(peer.path(), {"queue", action, "--request-file", path.string()});
            REQUIRE_FALSE(command.queue_selector_intent);
            REQUIRE(load_request_file(command, StandardAttributeRegistry{}));
            auto captured = run_captured(std::move(command));
            peer.verify();
            check_silent(captured);
            CHECK(captured.result.exit_code == 0);
        }
    }
    SECTION("explicit delete remains available without revision-bearing lookup support")
    {
        auto  info               = handshake();
        auto& fields             = std::get<JsonValue::Object>(info.value.data);
        auto& version            = std::get<JsonValue::Object>(fields.at("api_version").data);
        version.at("minor").data = std::uint64_t{5};
        ScriptedPeer peer{
            {std::move(info), queue_mutation("queue.delete", queue)}
        };
        auto captured = run_captured(command_for(peer.path(), {"queue", "delete", "--id", fixture_id}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 0);
    }
}

TEST_CASE("queue selection rejects mismatched lookup identities and live-only tombstones", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue();
    for (auto const* scenario : {"id", "name", "probe", "deleted"}) {
        CAPTURE(scenario);
        auto returned  = queue;
        auto arguments = std::vector<std::string>{"queue", "delete", fixture_id};
        auto selector  = QueueSelector{queue.id};
        auto steps     = std::vector<PeerStep>{handshake()};
        if (std::string_view{scenario} == "id") {
            returned = fixture_queue("reports", "10112233-4455-6677-8899-aabbccddeeff");
        }
        else if (std::string_view{scenario} == "deleted") {
            returned.state = QueueState::Deleted;
        }
        else if (std::string_view{scenario} == "name") {
            arguments.back() = "reports";
            selector         = std::string{"reports"};
            returned.name    = "unrelated";
        }
        else {
            steps.push_back(queue_lookup(queue.id, queue));
            selector = std::string{fixture_id};
        }
        steps.push_back(queue_lookup(selector, returned));
        ScriptedPeer peer{std::move(steps)};
        auto         captured = run_captured(command_for(peer.path(), std::move(arguments)));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 3);
        REQUIRE(captured.result.error);
        CHECK(captured.result.error->code == "jobu.client.invalid_response");
        CHECK_FALSE(captured.result.error->outcome_unknown);
    }
}

TEST_CASE("queue selection preserves synchronous admission failures without uncertainty", "[jobuctl][queue-selection]")
{
    auto const queue = fixture_queue();
    for (auto const* scenario : {"old-peer", "missing-lookup", "missing-mutation"}) {
        CAPTURE(scenario);
        auto  info   = handshake();
        auto& fields = std::get<JsonValue::Object>(info.value.data);
        auto  steps  = std::vector<PeerStep>{};
        if (std::string_view{scenario} == "old-peer") {
            auto& version            = std::get<JsonValue::Object>(fields.at("api_version").data);
            version.at("minor").data = std::uint64_t{5};
        }
        else {
            auto info_value = system_info_from_json(info.value);
            REQUIRE(info_value);
            auto const* absent = std::string_view{scenario} == "missing-lookup" ? "queue.get" : "queue.delete";
            std::erase(info_value->capabilities, std::string{absent});
            info.value = system_info_to_json(*info_value);
        }
        steps.push_back(std::move(info));
        if (std::string_view{scenario} == "missing-mutation") {
            steps.push_back(queue_lookup(queue.name, queue));
        }
        ScriptedPeer peer{std::move(steps)};
        auto         captured = run_captured(command_for(peer.path(), {"queue", "delete", "reports"}));
        peer.verify();
        check_silent(captured);
        CHECK(captured.result.exit_code == 1);
        REQUIRE(captured.result.error);
        CHECK(captured.result.error->code == (std::string_view{scenario} == "old-peer"
                                                  ? "jobu.client.unsupported_feature"
                                                  : "jobu.client.unsupported_method"));
        CHECK_FALSE(captured.result.error->outcome_unknown);
    }
}

TEST_CASE("queue selection mutation and polling share one deadline with phase-correct uncertainty",
          "[jobuctl][queue-selection]")
{
    auto queue = fixture_queue(fixture_id);
    for (auto const stop_at : {0U, 1U, 2U, 3U, 4U}) {
        CAPTURE(stop_at);
        StandardAttributeRegistry    registry;
        StreamCapture                output{stdout};
        StreamCapture                error{stderr};
        std::optional<SessionResult> result;
        auto                         finishes = 0;
        auto                         observed = 0U;
        {
            Application app{0, nullptr};
            auto*       loop = ThreadCtx::current()->event_loop();
            REQUIRE(loop);
            auto suspended  = queue;
            suspended.state = QueueState::Suspending;
            auto steps      = std::vector<PeerStep>{handshake(),
                                                    queue_lookup(queue.id, queue),
                                                    queue_lookup(queue.name, queue),
                                                    queue_mutation("queue.suspend", suspended),
                                                    queue_lookup(queue.id, suspended)};
            steps.resize(stop_at + 1);
            steps.back().action = PeerAction::Silence;
            std::optional<TimePoint> initial_deadline;
            ScriptedPeer             peer{
                std::move(steps),
                RequestObservation{.loop = loop, .observe = [&](std::string const&) {
                                       auto deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*loop);
                                       REQUIRE(deadline);
                                       REQUIRE(initial_deadline);
                                       CHECK(*deadline <= *initial_deadline);
                                       if (observed++ == stop_at) {
                                           // Expire the original budget at the chosen phase without sleeps or a fresh
                                           // Session.
                                           jb::core::priv::EventLoopTestAccess::fire_next_timer(*loop);
                                       }
                                   }}
            };
            Session session{command_for(peer.path(), {"queue", "suspend", fixture_id, "--wait", "--json"}), registry};
            session.finished.connect(&app, [&](int code) {
                ++finishes;
                static_cast<void>(app.quit(code));
                session.start();
            });
            session.start();
            initial_deadline = jb::core::priv::EventLoopTestAccess::next_timer_deadline(*loop);
            REQUIRE(initial_deadline);
            CHECK(app.exec() == 3);
            result = session.take_result();
            CHECK_FALSE(session.take_result());
            CHECK(jb::core::priv::EventLoopTestAccess::active_timer_count(*loop) == 0);
            peer.verify();
        }
        auto error_text  = error.text();
        auto output_text = output.text();
        CHECK(output_text.empty());
        CHECK(observed == stop_at + 1);
        CHECK(finishes == 1);
        REQUIRE(result);
        REQUIRE(result->error);
        CHECK(result->error->code == "jobu.client.timeout");
        CHECK(result->error->outcome_unknown == (stop_at == 3));
        auto rendered = parse_json(error_text);
        REQUIRE(rendered);
        CHECK(rendered->as_object().at("error").as_object().at("outcome_unknown").as_bool() == (stop_at == 3));
    }
}

TEST_CASE("queue waits keep the selected UUID and late replies cannot deliver or finish twice",
          "[jobuctl][queue-selection]")
{
    auto queue = fixture_queue(fixture_id);
    for (auto const wait : {false, true}) {
        CAPTURE(wait);
        auto final  = queue;
        final.name  = "renamed-after-resolution";
        final.state = wait ? QueueState::Suspending : QueueState::Suspended;
        auto steps  = std::vector<PeerStep>{handshake(),
                                            queue_lookup(queue.id, queue),
                                            queue_lookup(queue.name, queue),
                                            queue_mutation("queue.suspend", final)};
        if (wait) {
            final.state = QueueState::Suspended;
            steps.push_back(queue_lookup(queue.id, final));
        }
        for (auto& step : steps) {
            step.action = PeerAction::Duplicate;
        }
        ScriptedPeer                 peer{std::move(steps)};
        StreamCapture                output{stdout};
        StreamCapture                error{stderr};
        StandardAttributeRegistry    registry;
        auto                         finishes = 0;
        std::optional<SessionResult> result;
        {
            Application app{0, nullptr};
            auto        command = command_for(peer.path(), {"queue", "suspend", fixture_id, "--json"});
            command.wait        = wait;
            Session session{std::move(command), registry};
            session.finished.connect(&app, [&](int code) {
                ++finishes;
                static_cast<void>(app.quit(code));
                session.start();
            });
            session.start();
            CHECK(app.exec() == 0);
            result = session.take_result();
        }
        auto error_text  = error.text();
        auto output_text = output.text();
        peer.verify();
        CHECK(finishes == 1);
        REQUIRE(result);
        CHECK(result->exit_code == 0);
        CHECK(error_text.empty());
        auto value = parse_json(output_text);
        REQUIRE(value);
        CHECK(value->as_object().at("id").as_string() == fixture_id);
        CHECK(value->as_object().at("state").as_string() == "suspended");
        CHECK(value->as_object().at("name").as_string() == "renamed-after-resolution");
    }
}

TEST_CASE("a lost final mutation reply remains uncertain after successful selection", "[jobuctl][queue-selection]")
{
    auto const queue    = fixture_queue();
    auto       mutation = queue_mutation("queue.delete", queue);
    mutation.action     = PeerAction::Disconnect;
    ScriptedPeer peer{
        {handshake(), queue_lookup(queue.name, queue), std::move(mutation)}
    };
    auto captured = run_captured(command_for(peer.path(), {"queue", "delete", "reports"}));
    peer.verify();
    check_silent(captured);
    CHECK(captured.result.exit_code == 3);
    REQUIRE(captured.result.error);
    CHECK(captured.result.error->outcome_unknown);
    CHECK_FALSE(captured.result.reply);
}
