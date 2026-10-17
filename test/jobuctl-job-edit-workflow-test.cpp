#include "job_edit_workflow_priv.hpp"

#include "application.hpp"
#include "command_argv_priv.hpp"
#include "command_line_priv.hpp"
#include "database.hpp"
#include "domain_storage_priv.hpp"
#include "event_loop.hpp"
#include "event_thread.hpp"
#include "framing.hpp"
#include "input_priv.hpp"
#include "job_edit_priv.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "management.hpp"
#include "management_json.hpp"
#include "protocol.hpp"
#include "protocol_priv.hpp"
#include "query.hpp"
#include "run_session_priv.hpp"
#include "secret_service.hpp"
#include "sqlite/sqlite_driver.hpp"
#include "sqlite/sqlite_schema.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"
#include "system_info.hpp"
#include "thread_context.hpp"

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
#include <unistd.h>
#include <utility>
#include <variant> // IWYU pragma: keep for std::get in Catch assertions
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;
using namespace jb::net;
using namespace jb::rpc;
using namespace std::chrono_literals;

namespace {

auto parsed(std::string_view text) -> JsonValue
{
    auto value = parse_json(text);
    REQUIRE(value);
    return std::move(value).value();
}

auto serialized(JsonValue const& value) -> std::string
{
    auto text = serialize_json(value);
    REQUIRE(text);
    return std::move(text).value();
}

auto read_text(std::filesystem::path const& path) -> std::string
{
    auto file = std::ifstream{path, std::ios::binary};
    REQUIRE(file);
    return {std::istreambuf_iterator<char>{file}, {}};
}

auto read_draft(std::filesystem::path const& path) -> JsonValue
{
    auto value = parse_job_edit_draft(read_text(path));
    REQUIRE(value);
    return std::move(value).value();
}

void write_text(std::filesystem::path const& path, std::string_view text)
{
    auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
    REQUIRE(file);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    REQUIRE(file);
}

void edit_priority(std::filesystem::path const& path)
{
    // Change the visible field while preserving the preamble and every unrelated byte, as a real editor would.
    auto           text     = read_text(path);
    constexpr auto original = std::string_view{"\"priority\": 0"};
    auto const     position = text.find(original);
    REQUIRE(position != std::string::npos);
    text.replace(position, original.size(), "\"priority\": 50");
    write_text(path, text);
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
};

/// Captures the real stdout/stderr descriptors; restore before Catch reports assertions.
class StreamCapture final {
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
        auto                   result = std::string{};
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

auto sequence_id(std::uint8_t suffix) -> Uuid
{
    auto bytes = Uuid::Storage{};
    bytes[6]   = std::byte{0x70};
    bytes[8]   = std::byte{0x80};
    bytes[15]  = static_cast<std::byte>(suffix);
    return Uuid{bytes};
}

enum class Submission : std::uint8_t {
    Reply,
    DisconnectBeforeUpdate,
    DisconnectAfterUpdate
};

enum class InitialJob : std::uint8_t {
    Cli,
    LegacyHttp
};

/// All service/database/socket state belongs to the worker event loop. Main-thread calls synchronize via posts/latches.
struct PeerState {
    explicit PeerState(std::filesystem::path const& directory)
        : database{std::make_unique<jb::db::sqlite::Driver>(
              jb::db::sqlite::Options{.database_file = directory / "jobs.sqlite"})}
    {}

    auto initialize(InitialJob initial_job) -> bool
    {
        auto opened = database.open();
        if (!opened) {
            error = opened.error().message;
            return false;
        }
        auto schema = jb::jobu::sqlite::ensure_schema(database);
        if (!schema) {
            error = schema.error().message;
            return false;
        }
        time.set_utc(UtcTimePoint{1s});
        service    = std::make_unique<ManagementService>(database, registry, cron, generator, time);
        auto queue = service->create_queue({.name = "edit-workflow"});
        if (!queue) {
            error = queue.error().message;
            return false;
        }
        auto const legacy_http = initial_job == InitialJob::LegacyHttp;
        if (legacy_http) {
            SecretService secrets{database, time};
            auto          secret = secrets.set({.name = "workflow.token"});
            if (!secret) {
                error = secret.error().message;
                return false;
            }
        }
        auto payload = parse_json(legacy_http ? R"({"url":"http://127.0.0.1/legacy","method":"GET",
                                            "headers":[{"name":"X-Token","value":{"secret":"workflow.token"}}]})"
                                              : R"({"command":"/bin/true"})");
        auto created = service->create_job({.queue    = queue->id,
                                            .name     = "original",
                                            .type     = legacy_http ? JobType::Http : JobType::Cli,
                                            .schedule = OnceSchedule{.planned_at = UtcTimePoint{1h}},
                                            .payload  = std::move(payload).value()});
        if (!created) {
            error = created.error().message;
            return false;
        }
        job = std::move(created).value();
        if (legacy_http) {
            return seed_legacy_http_payload();
        }
        return true;
    }

    auto seed_legacy_http_payload() -> bool
    {
        // Only fixture setup bypasses new-input admission. The unchanged reference was registered
        // by create_job; both legacy records still pass the production pre-admission serializer.
        std::get<JsonValue::Object>(job.payload.data).at("method").data = std::string{"get"};
        auto payload = jb::jobu::detail::validate_and_serialize_job_payload(JobType::Http, job.payload);
        if (!payload) {
            error = "Unable to serialize legacy HTTP fixture";
            return false;
        }
        for (auto const* sql : {"UPDATE jobu_jobs SET payload_json = :payload WHERE id = :job_id",
                                "UPDATE jobu_runs SET payload_json = :payload WHERE job_id = :job_id"}) {
            jb::db::Query query{database};
            if (!query.prepare(sql) || !query.bind_value(":payload", std::string{payload->serialized()}) ||
                !query.bind_value(":job_id", jb::jobu::detail::uuid_to_storage(job.id)) || !query.exec()) {
                error = "Unable to seed legacy HTTP fixture";
                return false;
            }
        }
        return true;
    }

    void read_requests()
    {
        auto bodies = framer.append(socket->read_all());
        if (!bodies) {
            error = "invalid request framing";
            return;
        }
        for (auto const& body : *bodies) {
            auto request = parse_json(body);
            if (!request) {
                error = "invalid JSON request";
                return;
            }
            auto const& fields = request->as_object();
            auto const& method = fields.at("method").as_string();
            methods.push_back(method);
            auto const params = fields.find("params");
            auto       result = dispatch(method, params == fields.end() ? JsonValue{} : params->second);
            if (method == "job.update" && submission != Submission::Reply) {
                socket->abort();
                return;
            }
            auto const& id         = fields.at("id");
            auto        request_id = id.is_uint() ? RequestId{id.as_uint()} : RequestId{id.as_int()};
            auto        response   = result ? jb::rpc::detail::encode_success_response(request_id, *result)
                                            : jb::rpc::detail::encode_error_response(request_id, result.error());
            auto        encoded    = serialize_json(response);
            auto framed = encoded ? frame_message(*encoded) : Result<std::string, Error>::failure(encoded.error());
            if (!framed || socket->write(*framed) != framed->size()) {
                error = "response write failed";
                return;
            }
        }
    }

    auto dispatch(std::string const& method, JsonValue const& params) -> MethodResult
    {
        if (method == "system.info") {
            return MethodResult::success(system_info_to_json({
                .daemon_version = "edit-fixture",
                .api_version    = {.major = 1, .minor = 5},
                .capabilities   = {"system.info", "job.get", "job.update"}
            }));
        }
        auto id     = job_id_from_json(params);
        auto result = method == "job.get" && id ? service->get_job(*id) : service->get_job(job.id);
        if (method == "job.update") {
            updates.push_back(params);
            if (submission == Submission::DisconnectBeforeUpdate) {
                return MethodResult::success(JsonValue{});
            }
            auto update = update_job_request_from_json(params, registry);
            if (!update) {
                return MethodResult::failure(application_error(update.error()));
            }
            result = service->update_job(std::move(update).value());
        }
        else if (method != "job.get") {
            error = "unexpected method";
        }
        if (!result) {
            return MethodResult::failure(application_error(result.error()));
        }
        auto encoded = job_to_json(*result, registry);
        if (!encoded) {
            return MethodResult::failure(application_error(encoded.error()));
        }
        return MethodResult::success(std::move(encoded).value());
    }

    jb::db::Database                database;
    StandardAttributeRegistry       registry;
    jb::test::FakeCronEngine        cron;
    jb::test::SequenceUuidGenerator generator{
        {sequence_id(1), sequence_id(2), sequence_id(3)}
    };
    jb::test::FakeTimeSource           time;
    std::unique_ptr<ManagementService> service;
    JobDefinition                      job;
    LocalServer                        listener;
    std::unique_ptr<LocalSocket>       socket;
    StreamFramer                       framer;
    std::vector<std::string>           methods;
    std::vector<JsonValue>             updates;
    std::size_t                        connections{0};
    Submission                         submission{Submission::Reply};
    std::string                        error;
};

struct PeerSnapshot {
    JobDefinition            job;
    std::vector<std::string> methods;
    std::vector<JsonValue>   updates;
    std::size_t              connections;
};

class ServicePeer final {
public:
    explicit ServicePeer(InitialJob initial_job = InitialJob::Cli)
    {
        REQUIRE(_thread.start());
        // Watched I/O is created after run() adopts the worker's context, never in about_to_start.
        std::latch ready{1};
        REQUIRE(_thread.as_event_loop()->post([this, &ready, initial_job] {
            _state = std::make_unique<PeerState>(_directory.path());
            if (_state->initialize(initial_job)) {
                _state->listener.new_connection.connect(&_state->listener, [this] {
                    _state->socket = _state->listener.take_next_connection();
                    _state->framer = StreamFramer{};
                    ++_state->connections;
                    _state->socket->ready_read.connect(_state->socket.get(), [this] { _state->read_requests(); });
                    _state->read_requests();
                });
                if (!_state->listener.listen(path())) {
                    _state->error = "unable to listen";
                }
            }
            ready.count_down();
        }));
        ready.wait();
        REQUIRE(_state->error.empty());
    }

    ~ServicePeer()
    {
        // Tear down watched I/O while the worker still owns its loop context.
        if (!_thread.as_event_loop()->post([this] {
                _state.reset();
                static_cast<void>(_thread.quit());
            })) {
            std::terminate();
        }
        _thread.wait();
    }

    ServicePeer(ServicePeer const&)                    = delete;
    auto operator=(ServicePeer const&) -> ServicePeer& = delete;

    auto path() const -> std::filesystem::path { return _directory.path() / "peer.sock"; }

    void synchronize(std::function<void(PeerState&)> operation)
    {
        std::latch done{1};
        REQUIRE(_thread.as_event_loop()->post([this, &operation, &done] {
            operation(*_state);
            done.count_down();
        }));
        done.wait();
        INFO(_state->error);
        REQUIRE(_state->error.empty());
    }

    auto snapshot() -> PeerSnapshot
    {
        auto value = PeerSnapshot{};
        synchronize([&value](PeerState& state) {
            auto job = state.service->get_job(state.job.id);
            if (!job) {
                state.error = job.error().message;
                return;
            }
            value = {.job         = std::move(job).value(),
                     .methods     = state.methods,
                     .updates     = state.updates,
                     .connections = state.connections};
        });
        return value;
    }

    /// Read both stored documents on their owner loop; callers never borrow a database or query.
    auto payload_documents() -> std::array<std::string, 2>
    {
        auto documents = std::array<std::string, 2>{};
        synchronize([&documents](PeerState& state) {
            jb::db::Query query{state.database};
            if (!query.prepare("SELECT j.payload_json,r.payload_json FROM jobu_jobs j "
                               "JOIN jobu_runs r ON r.job_id=j.id WHERE j.id=:job_id") ||
                !query.bind_value(":job_id", jb::jobu::detail::uuid_to_storage(state.job.id)) || !query.exec()) {
                state.error = "Unable to query stored payload documents";
                return;
            }
            auto row = query.next();
            if (!row || !*row) {
                state.error = "Missing stored payload documents";
                return;
            }
            for (auto index = std::size_t{0}; index < documents.size(); ++index) {
                auto const* text = std::get_if<std::string>(&query.value(index));
                if (!text) {
                    state.error = "Invalid stored payload document";
                    return;
                }
                documents[index] = *text;
            }
        });
        return documents;
    }

private:
    jb::test::TemporaryDirectory _directory;
    std::unique_ptr<PeerState>   _state;
    EventThread                  _thread;
};

auto edit_command(ServicePeer& peer) -> Command
{
    return {
        .socket_path = peer.path(),
        .kind        = CommandKind::JobUpdate,
        .method      = "job.update",
        .request     = InteractiveJobUpdate{.job_id = peer.snapshot().job.id, .editor = "'fake editor' --wait"},
        .timeout     = 2s,
        .json        = true
    };
}

struct WorkflowResult {
    int         code;
    std::string output;
    std::string error;
};

auto run_workflow(Command command, JobEditWorkflowIo io) -> WorkflowResult
{
    StandardAttributeRegistry registry;
    StreamCapture             output{stdout};
    StreamCapture             error{stderr};
    auto const                code    = run_job_edit(std::move(command), registry, std::move(io));
    auto                      errors  = error.text();
    auto                      printed = output.text();
    CHECK(Application::instance() == nullptr);
    CHECK(ThreadCtx::current()->event_loop() == nullptr);
    return {.code = code, .output = std::move(printed), .error = std::move(errors)};
}

/// Decode the actual displayed command and load its request, without executing a shell or another mutation.
auto load_recovery_request(std::string_view diagnostic, StandardAttributeRegistry const& registry) -> Command
{
    constexpr auto prefix   = std::string_view{"Recovery command: "};
    auto const     position = diagnostic.find(prefix);
    REQUIRE(position != std::string_view::npos);
    diagnostic.remove_prefix(position + prefix.size());
    auto arguments = parse_command_argv(diagnostic.substr(0, diagnostic.find('\n')));
    REQUIRE(arguments);
    auto argv = std::vector<char*>{};
    for (auto& argument : *arguments) {
        argv.push_back(argument.data());
    }

    auto parsed_command = parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
    REQUIRE(parsed_command.action);
    REQUIRE(std::holds_alternative<Command>(*parsed_command.action));
    auto command = std::get<Command>(std::move(*parsed_command.action));
    REQUIRE(command.kind == CommandKind::JobUpdate);
    REQUIRE(load_request_file(command, registry));
    return command;
}

auto test_io(std::filesystem::path const& parent, EditorLauncher launch) -> JobEditWorkflowIo
{
    return {.check_terminal   = [] { return Result<void, Error>::success(); },
            .launch           = std::move(launch),
            .temporary_parent = parent};
}

void check_methods(PeerSnapshot const& snapshot, bool submitted)
{
    auto expected = std::vector<std::string>{"system.info", "job.get"};
    if (submitted) {
        expected.insert(expected.end(), {"system.info", "job.update"});
    }
    CHECK(snapshot.methods == expected);
    CHECK(snapshot.connections == (submitted ? 2 : 1));
    CHECK(snapshot.updates.size() == (submitted ? 1 : 0));
}

} // namespace

TEST_CASE("interactive update uses two scoped sessions and submits only the edit", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         command    = edit_command(peer);
    auto                         original   = peer.snapshot().job;
    auto                         draft_path = std::filesystem::path{};
    auto                         launches   = 0;

    SECTION("JSON result")
    {}
    SECTION("human result")
    {
        command.json = false;
    }

    auto result = run_workflow(command, test_io(files.path(), [&](auto argv, auto const& draft) {
                                   ++launches;
                                   CHECK(Application::instance() == nullptr);
                                   CHECK(ThreadCtx::current()->event_loop() == nullptr);
                                   CHECK(std::vector<std::string>(argv.begin(), argv.end()) ==
                                         std::vector<std::string>{"fake editor", "--wait"});
                                   draft_path = draft;
                                   CHECK(read_text(draft).starts_with("jobu:"));
                                   auto projection = read_draft(draft);
                                   CHECK(projection.as_object().at("job_id").as_string() == original.id.to_string());
                                   CHECK(projection.as_object().at("expected_revision").as_uint() == original.revision);
                                   edit_priority(draft);
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 0);
    CHECK(result.error.empty());
    CHECK(launches == 1);
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, true);
    CHECK(snapshot.job.revision == original.revision + 1);
    CHECK(snapshot.job.priority == 50);
    CHECK(snapshot.job.payload == original.payload);
    REQUIRE(snapshot.updates.size() == 1);
    auto const& params = snapshot.updates.front().as_object();
    CHECK(params.size() == 3);
    CHECK(params.at("expected_revision").as_uint() == original.revision);
    CHECK_FALSE(params.contains("payload"));
    if (command.json) {
        auto printed = parsed(result.output);
        CHECK(printed.as_object().at("revision").as_uint() == original.revision + 1);
    }
    else {
        CHECK(result.output.find("priority=50") != std::string::npos);
    }
}

TEST_CASE("interactive metadata edits preserve legacy HTTP payloads through success and conflict",
          "[jobuctl][edit][workflow][usability]")
{
    ServicePeer                  peer{InitialJob::LegacyHttp};
    jb::test::TemporaryDirectory files;
    auto const                   original  = peer.snapshot().job;
    auto const                   documents = peer.payload_documents();
    CHECK(original.payload.as_object().at("method").as_string() == "get");
    CHECK(documents[0] == documents[1]);
    auto const command     = edit_command(peer);
    auto       draft_path  = std::filesystem::path{};
    auto       edited_text = std::string{};
    auto       conflict    = false;
    SECTION("successful metadata update")
    {}
    SECTION("another operator wins")
    {
        conflict = true;
    }

    auto result =
        run_workflow(command, test_io(files.path(), [&](auto, auto const& draft) {
                         CHECK(Application::instance() == nullptr);
                         CHECK(ThreadCtx::current()->event_loop() == nullptr);
                         draft_path            = draft;
                         auto const projection = read_draft(draft);
                         CHECK(projection.as_object().at("payload") == original.payload);
                         CHECK(projection.as_object().at("expected_revision").as_uint() == original.revision);
                         edit_priority(draft);
                         edited_text = read_text(draft);
                         if (conflict) {
                             // The second operator advances the revision while the displayed baseline stays fixed.
                             peer.synchronize([&](PeerState& state) {
                                 auto changed = state.service->update_job(
                                     {.job_id = original.id, .expected_revision = original.revision, .priority = 99});
                                 if (!changed) {
                                     state.error = changed.error().message;
                                 }
                             });
                         }
                         return Result<void, Error>::success();
                     }));

    auto snapshot = peer.snapshot();
    check_methods(snapshot, true);
    CHECK(snapshot.job.revision == original.revision + 1);
    CHECK(snapshot.job.priority == (conflict ? 99 : 50));
    CHECK(snapshot.job.payload == original.payload);
    // Definition and eligible unstarted run are separate durable observations, including exact spelling.
    CHECK(peer.payload_documents() == documents);
    REQUIRE(snapshot.updates.size() == 1);
    auto const& submitted = snapshot.updates.front();
    CHECK(submitted.as_object().size() == 3);
    CHECK(submitted.as_object().at("job_id").as_string() == original.id.to_string());
    CHECK(submitted.as_object().at("expected_revision").as_uint() == original.revision);
    CHECK(submitted.as_object().at("priority").as_uint() == 50);
    CHECK_FALSE(submitted.as_object().contains("payload"));
    CHECK(result.error.find("http://127.0.0.1/legacy") == std::string::npos);
    if (conflict) {
        CHECK(result.code == 1);
        CHECK(result.output.empty());
        CHECK(result.error.find("jobu.job.revision_conflict") != std::string::npos);
        CHECK(result.error.find("manually reconcile") != std::string::npos);
        CHECK(result.error.find("old revision") != std::string::npos);
        CHECK(read_text(draft_path) == edited_text);
        auto retained = parsed(read_text(draft_path.parent_path() / "update-request.json"));
        CHECK(retained == submitted);
        StandardAttributeRegistry registry;
        auto                      recovered = load_recovery_request(result.error, registry);
        auto request = update_job_request_to_json(std::get<UpdateJobRequest>(recovered.request), registry);
        REQUIRE(request);
        CHECK(serialized(*request) == serialized(submitted));
        CHECK(peer.snapshot().methods == snapshot.methods);
    }
    else {
        CHECK(result.code == 0);
        CHECK(result.error.empty());
        CHECK(parsed(result.output).as_object().at("payload") == original.payload);
        CHECK_FALSE(std::filesystem::exists(draft_path.parent_path()));
    }
}

TEST_CASE("interactive no-op cleans files without an update or stdout", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         command    = edit_command(peer);
    auto                         draft_path = std::filesystem::path{};
    SECTION("JSON mode")
    {}
    SECTION("human mode")
    {
        command.json = false;
    }
    auto result = run_workflow(command, test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 0);
    CHECK(result.output.empty());
    CHECK(result.error.find("No changes; no update submitted") != std::string::npos);
    CHECK(result.error.find("Keep job_id") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.job.revision == 1);
    CHECK(snapshot.job.priority == 0);
}

TEST_CASE("editor cancellation and launch failure clean known files without mutation", "[jobuctl][edit][workflow]")
{
    auto failure       = Error{.category = ErrorCategory::Cancelled,
                               .code     = "jobuctl.editor.cancelled",
                               .message  = "Editor cancelled the update"};
    auto expected_exit = 1;
    SECTION("nonzero exit")
    {}
    SECTION("signal interruption")
    {
        failure.code = "jobuctl.editor.interrupted";
    }
    SECTION("launch failure")
    {
        failure.category = ErrorCategory::Io;
        failure.code     = "jobuctl.editor.spawn_failed";
        expected_exit    = 3;
    }
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path = std::filesystem::path{};
    auto result = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   edit_priority(draft);
                                   return Result<void, Error>::failure(failure);
                               }));
    CHECK(result.code == expected_exit);
    CHECK(result.output.empty());
    CHECK(result.error.find(failure.code) != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.job.revision == 1);
    CHECK(snapshot.job.priority == 0);
}

TEST_CASE("invalid saved drafts are retained unchanged without mutation", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path = std::filesystem::path{};
    auto                         bytes      = std::string{};
    auto                         missing    = false;
    auto const                   sensitive  = std::string{"literal-sensitive-edit-value"};
    auto result = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   SECTION("invalid JSON")
                                   {
                                       write_text(draft, "{" + sensitive);
                                   }
                                   SECTION("invalid revision")
                                   {
                                       auto value = read_draft(draft);
                                       std::get<JsonValue::Object>(value.data).at("expected_revision") =
                                           JsonValue{.data = std::uint64_t{2}};
                                       write_text(draft, serialized(value));
                                   }
                                   SECTION("invalid attribute value")
                                   {
                                       auto  value      = read_draft(draft);
                                       auto& attributes = std::get<JsonValue::Object>(
                                           std::get<JsonValue::Object>(value.data).at("attributes").data);
                                       attributes.at("retry.max_attempts") = JsonValue{.data = sensitive};
                                       write_text(draft, serialized(value));
                                   }
                                   SECTION("unknown property containing sensitive input")
                                   {
                                       auto value = read_draft(draft);
                                       std::get<JsonValue::Object>(value.data).emplace(sensitive, JsonValue{});
                                       write_text(draft, serialized(value));
                                   }
                                   SECTION("empty file")
                                   {
                                       write_text(draft, "");
                                   }
                                   SECTION("missing file")
                                   {
                                       REQUIRE(std::filesystem::remove(draft));
                                       missing = true;
                                   }
                                   if (!missing) {
                                       bytes = read_text(draft);
                                   }
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 2);
    CHECK(result.output.empty());
    CHECK(result.error.find(sensitive) == std::string::npos);
    CHECK(result.error.find(draft_path.string()) != std::string::npos);
    CHECK(result.error.find("Recovery command:") == std::string::npos);
    CHECK(result.error.find("Minimal update request:") == std::string::npos);
    CHECK(result.error.find("No complete minimal request is available") != std::string::npos);
    CHECK(std::filesystem::exists(draft_path.parent_path()));
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path() / "update-request.json"));
    if (missing) {
        CHECK(result.error.find("jobuctl.edit.missing_draft") != std::string::npos);
        CHECK(result.error.find("Missing draft: " + draft_path.string()) != std::string::npos);
        CHECK(result.error.find("Retained draft:") == std::string::npos);
        CHECK(result.error.find("Correct the retained draft") == std::string::npos);
        CHECK(result.error.find("Start a new interactive edit") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(draft_path));
        CHECK(std::filesystem::is_empty(draft_path.parent_path()));
    }
    else {
        CHECK(result.error.find("Retained draft: " + draft_path.string()) != std::string::npos);
        CHECK(result.error.find("select only intended changed fields") != std::string::npos);
        CHECK(read_text(draft_path) == bytes);
    }
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.job.revision == 1);
    CHECK(snapshot.job.priority == 0);
}

TEST_CASE("editor time is outside both real session lifetimes and submission receives a full budget",
          "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    StandardAttributeRegistry    registry;
    jb::test::FakeTimeSource     clock;
    auto                         command        = edit_command(peer);
    auto                         modes          = std::vector<SessionMode>{};
    auto                         budgets        = std::vector<std::chrono::milliseconds>{};
    auto                         fetch_deadline = TimePoint{};
    auto                         io             = test_io(files.path(), [&](auto, auto const& draft) {
        CHECK(Application::instance() == nullptr);
        // Advance controlled elapsed time beyond the configured timeout without a real editor or timing sleep.
        clock.advance(1h);
        CHECK(clock.monotonic_now() > fetch_deadline);
        edit_priority(draft);
        return Result<void, Error>::success();
    });
    io.run                                      = [&](Command next, SessionMode mode) {
        modes.push_back(mode);
        budgets.push_back(next.timeout);
        if (mode == SessionMode::Capture) {
            fetch_deadline = clock.monotonic_now() + next.timeout;
        }
        else {
            CHECK(clock.monotonic_now() > fetch_deadline);
            CHECK(next.timeout == command.timeout);
        }
        return run_session(std::move(next), registry, mode);
    };
    auto result = run_workflow(command, std::move(io));
    CHECK(result.code == 0);
    CHECK(modes == std::vector<SessionMode>{SessionMode::Capture, SessionMode::Render});
    CHECK(budgets == std::vector<std::chrono::milliseconds>{command.timeout, command.timeout});
    check_methods(peer.snapshot(), true);
}

TEST_CASE("real revision conflict preserves another edit and the original minimal request", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto const                   parent = files.path() / "operator's $HOME; edit files";
    REQUIRE(std::filesystem::create_directory(parent));
    auto       command    = edit_command(peer);
    auto       draft_path = std::filesystem::path{};
    auto const sensitive  = std::string{"literal-sensitive-payload-value"};
    auto const original   = peer.snapshot().job;
    SECTION("JSON error and recovery")
    {}
    SECTION("human error and recovery")
    {
        command.json = false;
    }
    auto result = run_workflow(
        command,
        test_io(parent, [&](auto, auto const& draft) {
            draft_path = draft;
            edit_priority(draft);
            auto  value   = read_draft(draft);
            auto& payload = std::get<JsonValue::Object>(std::get<JsonValue::Object>(value.data).at("payload").data);
            payload.emplace("arguments", JsonValue{.data = JsonValue::Array{JsonValue{.data = sensitive}}});
            write_text(draft, serialized(value));
            // Another operator commits while this draft still holds revision 1. No workflow
            // refresh is permitted.
            peer.synchronize([](PeerState& state) {
                auto changed =
                    state.service->update_job({.job_id = state.job.id, .expected_revision = 1, .priority = 99});
                if (!changed) {
                    state.error = changed.error().message;
                }
            });
            return Result<void, Error>::success();
        }));
    CHECK(result.code == 1);
    CHECK(result.output.empty());
    CHECK(result.error.find("jobu.job.revision_conflict") != std::string::npos);
    CHECK(result.error.find("manually reconcile") != std::string::npos);
    CHECK(result.error.find("old revision") != std::string::npos);
    CHECK(result.error.find(sensitive) == std::string::npos);
    auto snapshot = peer.snapshot();
    check_methods(snapshot, true);
    CHECK(snapshot.job.revision == 2);
    CHECK(snapshot.job.priority == 99);
    CHECK(snapshot.job.payload == original.payload);
    auto request = parsed(read_text(draft_path.parent_path() / "update-request.json"));
    CHECK(request == snapshot.updates.front());
    CHECK(request.as_object().size() == 4);
    CHECK(request.as_object().at("expected_revision").as_uint() == 1);
    CHECK(request.as_object().at("priority").as_uint() == 50);
    CHECK(std::filesystem::exists(draft_path));

    StandardAttributeRegistry registry;
    auto                      recovered = load_recovery_request(result.error, registry);
    CHECK(recovered.socket_path == command.socket_path);
    CHECK(recovered.timeout == command.timeout);
    auto outgoing = update_job_request_to_json(std::get<UpdateJobRequest>(recovered.request), registry);
    REQUIRE(outgoing);
    // The codec uses signed priority, while parsing its nonnegative JSON token produces unsigned storage.
    CHECK(serialized(*outgoing) == serialized(request));
    CHECK(read_text(*recovered.request_file).find(sensitive) != std::string::npos);
    CHECK(peer.snapshot().methods == snapshot.methods);
}

TEST_CASE("failed submission retains draft and minimal request including uncertain outcomes",
          "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path        = std::filesystem::path{};
    auto                         expected_revision = JobRevision{1};
    auto                         expected_priority = 0;
    auto                         outcome_unknown   = false;
    auto                         refusal           = false;
    auto                         remote_failure    = false;
    auto const                   sensitive         = std::string{"literal-sensitive-payload-value"};
    SECTION("connection refused before submission")
    {
        refusal = true;
    }
    SECTION("domain rejection")
    {
        remote_failure = true;
    }
    SECTION("connection lost before applying received mutation")
    {
        outcome_unknown = true;
        peer.synchronize([](PeerState& state) { state.submission = Submission::DisconnectBeforeUpdate; });
    }
    SECTION("committed mutation with lost response")
    {
        outcome_unknown   = true;
        expected_revision = 2;
        expected_priority = 50;
        peer.synchronize([](PeerState& state) { state.submission = Submission::DisconnectAfterUpdate; });
    }
    auto result = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   edit_priority(draft);
                                   auto  value   = read_draft(draft);
                                   auto& payload = std::get<JsonValue::Object>(
                                       std::get<JsonValue::Object>(value.data).at("payload").data);
                                   payload.emplace("arguments",
                                                   JsonValue{.data = JsonValue::Array{JsonValue{.data = sensitive}}});
                                   if (remote_failure) {
                                       payload.at("command") = JsonValue{.data = std::string{}};
                                   }
                                   write_text(draft, serialized(value));
                                   if (refusal) {
                                       peer.synchronize([](PeerState& state) { state.listener.close(); });
                                   }
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == (remote_failure ? 1 : 3));
    CHECK(result.output.empty());
    CHECK(result.error.find("Minimal update request:") != std::string::npos);
    CHECK(result.error.find("Recovery command:") != std::string::npos);
    CHECK(result.error.find(sensitive) == std::string::npos);
    if (outcome_unknown) {
        CHECK(result.error.find("\"outcome_unknown\":true") != std::string::npos);
        CHECK(result.error.find("Inspect current server state before any retry") != std::string::npos);
    }
    auto snapshot = peer.snapshot();
    check_methods(snapshot, !refusal);
    CHECK(snapshot.job.revision == expected_revision);
    CHECK(snapshot.job.priority == expected_priority);
    auto request = parsed(read_text(draft_path.parent_path() / "update-request.json"));
    CHECK(request.as_object().at("expected_revision").as_uint() == 1);
    CHECK(request.as_object().at("priority").as_uint() == 50);
    CHECK(request.as_object().contains("payload"));
    CHECK(std::filesystem::exists(draft_path));
    if (!refusal) {
        CHECK(request == snapshot.updates.front());
    }
    StandardAttributeRegistry registry;
    auto                      recovered = load_recovery_request(result.error, registry);
    auto outgoing = update_job_request_to_json(std::get<UpdateJobRequest>(recovered.request), registry);
    REQUIRE(outgoing);
    CHECK(serialized(*outgoing) == serialized(request));
    CHECK(peer.snapshot().methods == snapshot.methods);
}

TEST_CASE("terminal and editor configuration failures precede fetching", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         command  = edit_command(peer);
    auto                         checked  = 0;
    auto                         launched = 0;
    auto                         io       = test_io(files.path(), [&](auto, auto const&) {
        ++launched;
        return Result<void, Error>::success();
    });
    SECTION("terminal refusal")
    {
        io.check_terminal = [&] {
            ++checked;
            return Result<void, Error>::failure({.category = ErrorCategory::InvalidArgument,
                                                 .code     = "jobuctl.edit.terminal_required",
                                                 .message  = "Terminal required"});
        };
    }
    SECTION("malformed explicit configuration")
    {
        std::get<InteractiveJobUpdate>(command.request).editor = "'unclosed";
        io.check_terminal                                      = [&] {
            ++checked;
            return Result<void, Error>::success();
        };
    }
    auto result = run_workflow(std::move(command), std::move(io));
    CHECK(result.code == 2);
    CHECK(result.output.empty());
    CHECK(checked == 1);
    CHECK(launched == 0);
    CHECK(peer.snapshot().methods.empty());
    CHECK(std::filesystem::is_empty(files.path()));
}

TEST_CASE("fetch failure is rendered once without creating a draft", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         command                   = edit_command(peer);
    std::get<InteractiveJobUpdate>(command.request).job_id = sequence_id(42);
    auto result = run_workflow(std::move(command), test_io(files.path(), [](auto, auto const&) {
                                   FAIL("fetch failure must not launch an editor");
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 1);
    CHECK(result.output.empty());
    auto error = parsed(result.error);
    CHECK(error.as_object().at("error").as_object().at("code").as_string() == "jobu.job.not_found");
    CHECK(std::filesystem::is_empty(files.path()));
    check_methods(peer.snapshot(), false);
}

TEST_CASE("cleanup preserves unexpected editor backups and reports the retained directory", "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path = std::filesystem::path{};
    auto                         changed    = false;
    SECTION("no-op cleanup")
    {}
    SECTION("cleanup after a committed update")
    {
        changed = true;
    }
    auto result = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   if (changed) {
                                       edit_priority(draft);
                                   }
                                   write_text(draft.parent_path() / "editor.backup", "backup");
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 3);
    if (changed) {
        auto reply = parsed(result.output);
        CHECK(reply.as_object().at("revision").as_uint() == 2);
        CHECK(reply.as_object().at("priority").as_uint() == 50);
    }
    else {
        CHECK(result.output.empty());
    }
    auto const error_start = result.error.find("{\"error\":");
    REQUIRE(error_start != std::string::npos);
    auto error =
        parsed(std::string_view{result.error}.substr(error_start, result.error.find('\n', error_start) - error_start));
    CHECK(error.as_object().at("error").as_object().at("code").as_string() == "jobuctl.edit.cleanup_failed");
    CHECK(result.error.find("Retained edit directory:") != std::string::npos);
    CHECK(std::filesystem::exists(draft_path.parent_path() / "editor.backup"));
    CHECK_FALSE(std::filesystem::exists(draft_path));
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path() / "update-request.json"));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, changed);
    CHECK(snapshot.job.revision == (changed ? 2 : 1));
    CHECK(snapshot.job.priority == (changed ? 50 : 0));
}

TEST_CASE("failure to save the minimal request prevents submission and preserves the draft",
          "[jobuctl][edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path = std::filesystem::path{};
    auto                         edited     = std::string{};
    auto result = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   edit_priority(draft);
                                   edited = read_text(draft);
                                   // An unexpected entry must never be overwritten or mistaken for the validated
                                   // minimal recovery request.
                                   write_text(draft.parent_path() / "update-request.json", "unexpected file");
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 2);
    CHECK(result.output.empty());
    CHECK(result.error.find("No complete minimal request is available") != std::string::npos);
    CHECK(result.error.find("Recovery command:") == std::string::npos);
    CHECK(read_text(draft_path) == edited);
    CHECK(read_text(draft_path.parent_path() / "update-request.json") == "unexpected file");
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.job.revision == 1);
    CHECK(snapshot.job.priority == 0);
}

TEST_CASE("native preflight refuses redirected stderr before an RPC", "[jobuctl][edit][workflow]")
{
    // A captured stderr is deliberately nonterminal, regardless of the test runner's controlling terminal.
    StreamCapture error{stderr};
    auto          checked = check_editor_terminal();
    static_cast<void>(error.text());
    REQUIRE_FALSE(checked);
    CHECK(checked.error().code == "jobuctl.edit.terminal_required");
    CHECK(checked.error().message.find("--revision") != std::string::npos);
    CHECK(checked.error().message.find("--request-file") != std::string::npos);
}
