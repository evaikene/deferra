#include "edit_document_priv.hpp"
#include "edit_resource_priv.hpp"
#include "queue_edit_workflow_priv.hpp"

#include "application.hpp"
#include "command_argv_priv.hpp"
#include "command_line_priv.hpp"
#include "database.hpp"
#include "event_loop.hpp"
#include "event_thread.hpp"
#include "framing.hpp"
#include "input_priv.hpp"
#include "json.hpp"
#include "local_server.hpp"
#include "local_socket.hpp"
#include "management.hpp"
#include "management_json.hpp"
#include "protocol.hpp"
#include "protocol_priv.hpp"
#include "run_session_priv.hpp"
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
    auto value = parse_edit_draft(EditResource::Queue, read_text(path));
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

void edit_weight(std::filesystem::path const& path)
{
    // Change the visible field while preserving the preamble and every unrelated byte, as a real editor would.
    auto           text     = read_text(path);
    constexpr auto original = std::string_view{"\"weight\": 1"};
    auto const     position = text.find(original);
    REQUIRE(position != std::string::npos);
    text.replace(position, original.size(), "\"weight\": 5");
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
    bytes[0]   = std::byte{0xab};
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

/// All service/database/socket state belongs to the worker loop. Calls synchronize through posts and latches.
struct PeerState {
    explicit PeerState(std::filesystem::path const& directory)
        : database{std::make_unique<jb::db::sqlite::Driver>(
              jb::db::sqlite::Options{.database_file = directory / "queues.sqlite"})}
    {}

    auto initialize() -> bool
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
        service      = std::make_unique<ManagementService>(database, registry, cron, generator, time);
        auto created = service->create_queue({.name = "edit-workflow"});
        if (!created) {
            error = created.error().message;
            return false;
        }
        queue = std::move(created).value();
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
            if (method == "queue.update" && submission != Submission::Reply) {
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
                .daemon_version = "queue-edit-fixture",
                .api_version    = {.major = 1, .minor = 6},
                .capabilities   = {"system.info", "queue.get", "queue.update"}
            }));
        }
        auto result = service->get_queue(queue.id);
        if (method == "queue.get") {
            lookups.push_back(params);
            auto request = queue_get_request_from_json(params);
            if (!request) {
                return MethodResult::failure(application_error(request.error()));
            }
            result = service->get_queue(request->queue, request->include_deleted);
        }
        else if (method == "queue.update") {
            updates.push_back(params);
            if (submission == Submission::DisconnectBeforeUpdate) {
                return MethodResult::success(JsonValue{});
            }
            auto request = update_queue_request_from_json(params, registry);
            if (!request) {
                return MethodResult::failure(application_error(request.error()));
            }
            result = service->update_queue(std::move(request).value());
        }
        else {
            error = "unexpected method";
        }
        if (!result) {
            return MethodResult::failure(application_error(result.error()));
        }
        auto encoded = queue_to_json(*result, registry);
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
    Queue                              queue;
    std::optional<Queue>               replacement;
    LocalServer                        listener;
    std::unique_ptr<LocalSocket>       socket;
    StreamFramer                       framer;
    std::vector<std::string>           methods;
    std::vector<JsonValue>             updates;
    std::vector<JsonValue>             lookups;
    std::size_t                        connections{0};
    Submission                         submission{Submission::Reply};
    std::string                        error;
};

struct PeerSnapshot {
    Queue                    queue;
    std::optional<Queue>     replacement;
    std::vector<std::string> methods;
    std::vector<JsonValue>   updates;
    std::vector<JsonValue>   lookups;
    std::size_t              connections;
};

enum class CompetingChange : std::uint8_t {
    Weight,
    Rename,
    Suspend,
    Resume,
    Delete,
};

/// Runs a real competing mutation on the worker loop, preserving the editor's original queue snapshot.
auto change_queue_while_editing(PeerState& state, CompetingChange change) -> Result<void, Error>
{
    if (change == CompetingChange::Weight || change == CompetingChange::Rename) {
        auto request = UpdateQueueRequest{.queue = state.queue.id, .expected_revision = 1};
        if (change == CompetingChange::Weight) {
            request.weight = 9;
        }
        else {
            request.name = "renamed-while-editing";
        }
        auto updated = state.service->update_queue(std::move(request));
        if (!updated) {
            return Result<void, Error>::failure(updated.error());
        }
    }
    else {
        // An empty queue drains immediately, so suspension advances through both durable transitions.
        auto suspended = state.service->suspend_queue(state.queue.id);
        if (!suspended) {
            return Result<void, Error>::failure(suspended.error());
        }
        if (change == CompetingChange::Resume) {
            auto resumed = state.service->resume_queue(state.queue.id);
            if (!resumed) {
                return Result<void, Error>::failure(resumed.error());
            }
        }
        else if (change == CompetingChange::Delete) {
            auto deleted = state.service->delete_queue(state.queue.id);
            if (!deleted) {
                return Result<void, Error>::failure(deleted.error());
            }
        }
    }

    // Reuse the original name at revision 1 to expose any accidental submission-time name resolution.
    if (change == CompetingChange::Rename || change == CompetingChange::Delete) {
        auto replacement = state.service->create_queue({.name = state.queue.name});
        if (!replacement) {
            return Result<void, Error>::failure(replacement.error());
        }
        state.replacement = *replacement;
    }
    return Result<void, Error>::success();
}

class ServicePeer final {
public:
    ServicePeer()
    {
        REQUIRE(_thread.start());
        // Watched I/O is created after run() adopts the worker's context, never in about_to_start.
        std::latch ready{1};
        REQUIRE(_thread.as_event_loop()->post([this, &ready] {
            _state = std::make_unique<PeerState>(_directory.path());
            if (_state->initialize()) {
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
            auto queue = state.service->get_queue(state.queue.id, true);
            if (!queue) {
                state.error = queue.error().message;
                return;
            }
            auto replacement = state.replacement;
            if (replacement) {
                auto current = state.service->get_queue(replacement->id);
                if (!current) {
                    state.error = current.error().message;
                    return;
                }
                replacement = *current;
            }
            value = {.queue       = *queue,
                     .replacement = std::move(replacement),
                     .methods     = state.methods,
                     .updates     = state.updates,
                     .lookups     = state.lookups,
                     .connections = state.connections};
        });
        return value;
    }

private:
    jb::test::TemporaryDirectory _directory;
    std::unique_ptr<PeerState>   _state;
    EventThread                  _thread;
};

auto edit_command(ServicePeer& peer, std::vector<std::string> selector = {"edit-workflow"}) -> Command
{
    auto arguments = std::vector<std::string>{"jobuctl", "queue", "update"};
    arguments.insert(arguments.end(), selector.begin(), selector.end());
    arguments.insert(
        arguments.end(),
        {"--editor", "'fake editor' --wait", "--socket", peer.path().string(), "--timeout", "2000", "--json"});
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    StandardAttributeRegistry registry;
    auto                      parsed_command = parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
    REQUIRE(parsed_command.action);
    return std::get<Command>(std::move(*parsed_command.action));
}

struct WorkflowResult {
    int         code;
    std::string output;
    std::string error;
};

auto run_workflow(Command command, EditWorkflowIo io) -> WorkflowResult
{
    StandardAttributeRegistry registry;
    StreamCapture             output{stdout};
    StreamCapture             error{stderr};
    auto const                code    = run_queue_edit(std::move(command), registry, std::move(io));
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
    REQUIRE(command.kind == CommandKind::QueueUpdate);
    REQUIRE(load_request_file(command, registry));
    return command;
}

auto test_io(std::filesystem::path const& parent, EditorLauncher launch) -> EditWorkflowIo
{
    return {.check_terminal   = [] { return Result<void, Error>::success(); },
            .launch           = std::move(launch),
            .temporary_parent = parent};
}

void check_methods(PeerSnapshot const& snapshot, bool submitted, bool inferred = false)
{
    auto expected = std::vector<std::string>{"system.info", "queue.get"};
    if (inferred) {
        expected.emplace_back("queue.get");
    }
    if (submitted) {
        expected.insert(expected.end(), {"system.info", "queue.update"});
    }
    CHECK(snapshot.methods == expected);
    CHECK(snapshot.connections == (submitted ? 2 : 1));
    CHECK(snapshot.updates.size() == (submitted ? 1 : 0));
    CHECK(snapshot.lookups.size() == (inferred ? 2 : 1));
    for (auto const& params : snapshot.lookups) {
        auto request = queue_get_request_from_json(params);
        REQUIRE(request);
        CHECK_FALSE(request->include_deleted);
    }
}

} // namespace

TEST_CASE("queue editor captures every selector form and submits one minimal UUID update",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto const                   original = peer.snapshot().queue;
    auto                         upper    = original.id.to_string();
    upper.replace(0, 2, "AB");
    auto selection = std::vector<std::string>{original.name};
    auto inferred  = false;
    auto json      = true;
    SECTION("exact name")
    {}
    SECTION("explicit name")
    {
        selection = {"name:" + original.name};
    }
    SECTION("name alias")
    {
        selection = {"--name", original.name};
    }
    SECTION("explicit ID")
    {
        selection = {"id:" + original.id.to_string()};
    }
    SECTION("ID alias")
    {
        selection = {"--id", original.id.to_string()};
    }
    SECTION("inferred uppercase ID")
    {
        selection = {upper};
        inferred  = true;
    }
    SECTION("human result")
    {
        json = false;
    }

    auto command                         = edit_command(peer, selection);
    command.json                         = json;
    auto                      draft_path = std::filesystem::path{};
    auto                      launches   = 0;
    StandardAttributeRegistry registry;
    auto                      io = test_io(files.path(), [&](auto argv, auto const& draft) {
        ++launches;
        CHECK(Application::instance() == nullptr);
        CHECK(ThreadCtx::current()->event_loop() == nullptr);
        CHECK(std::vector<std::string>(argv.begin(), argv.end()) == std::vector<std::string>{"fake editor", "--wait"});
        draft_path = draft;
        CHECK(draft.filename() == "draft.jsonc");
        CHECK(std::filesystem::status(draft.parent_path()).permissions() == std::filesystem::perms::owner_all);
        CHECK(std::filesystem::status(draft).permissions() ==
              (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
        CHECK(read_text(draft).find("// Keep queue_id and expected_revision unchanged.") != std::string::npos);
        CHECK(read_text(draft).find("// defaults replaces the whole queue-default layer") != std::string::npos);
        auto projection = read_draft(draft);
        CHECK(projection.as_object().at("queue_id").as_string() == original.id.to_string());
        CHECK(projection.as_object().at("expected_revision").as_uint() == original.revision);
        CHECK_FALSE(projection.as_object().contains("state"));
        edit_weight(draft);
        return Result<void, Error>::success();
    });
    io.run                       = [&](Command next, SessionMode mode) {
        if (mode == SessionMode::Render) {
            CHECK_FALSE(next.queue_selector_intent);
            auto params = update_queue_request_to_json(std::get<UpdateQueueRequest>(next.request), registry);
            REQUIRE(params);
            // The actual recovery file must already be closed and strict-loadable before any mutation starts.
            CHECK(serialized(*params) == read_text(draft_path.parent_path() / "update-request.json"));
        }
        return run_session(std::move(next), registry, mode);
    };
    auto result = run_workflow(command, std::move(io));
    CHECK(result.code == 0);
    CHECK(result.error.empty());
    CHECK(launches == 1);
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, true, inferred);
    CHECK(snapshot.queue.revision == original.revision + 1);
    CHECK(snapshot.queue.weight == 5);
    auto const& params = snapshot.updates.front().as_object();
    CHECK(params.size() == 3);
    CHECK(params.at("queue_id").as_string() == original.id.to_string());
    CHECK(params.at("expected_revision").as_uint() == original.revision);
    CHECK(params.at("weight").as_uint() == 5);
    if (inferred) {
        CHECK(snapshot.lookups.back().as_object().at("queue_name").as_string() == upper);
    }
    if (json) {
        auto printed = parsed(result.output);
        CHECK(printed.as_object().at("revision").as_uint() == original.revision + 1);
        CHECK(printed.as_object().at("weight").as_uint() == 5);
    }
    else {
        CHECK(result.output.find("weight=5") != std::string::npos);
    }
}

TEST_CASE("queue editor no-op and comment-only edits submit nothing and leave stdout empty",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         command  = edit_command(peer);
    auto                         comments = false;
    SECTION("JSON no-op")
    {}
    SECTION("human no-op")
    {
        command.json = false;
    }
    SECTION("comment-only edit")
    {
        comments = true;
    }
    auto result = run_workflow(command, test_io(files.path(), [&](auto, auto const& draft) {
                                   if (comments) {
                                       write_text(draft, "/* operator comment */\r\n" + read_text(draft));
                                   }
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 0);
    CHECK(result.output.empty());
    CHECK(result.error == "No changes; no update submitted\n");
    CHECK(std::filesystem::is_empty(files.path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.queue.revision == 1);
    CHECK(snapshot.queue.weight == 1);
}

TEST_CASE("queue editor cancellation and launch failure clean files without a mutation",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto failure   = Error{.category = ErrorCategory::Cancelled, .code = "editor.cancelled", .message = "Cancelled"};
    auto exit_code = 1;
    SECTION("cancelled")
    {}
    SECTION("launch failed")
    {
        failure.category = ErrorCategory::Io;
        exit_code        = 3;
    }
    auto result =
        run_workflow(edit_command(peer),
                     test_io(files.path(), [&](auto, auto const&) { return Result<void, Error>::failure(failure); }));
    CHECK(result.code == exit_code);
    CHECK(result.output.empty());
    CHECK(std::filesystem::is_empty(files.path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.queue.revision == 1);
}

TEST_CASE("invalid queue drafts retain original saved bytes without a submit-ready request",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path = std::filesystem::path{};
    auto                         saved      = std::string{};
    auto                         missing    = false;
    auto                         scenario   = std::string{};
    SECTION("invalid JSON")
    {
        scenario = "json";
    }
    SECTION("changed revision")
    {
        scenario = "revision";
    }
    SECTION("changed UUID")
    {
        scenario = "uuid";
    }
    SECTION("unknown sensitive member")
    {
        scenario = "unknown";
    }
    SECTION("trailing comma")
    {
        scenario = "comma";
    }
    SECTION("missing draft")
    {
        missing = true;
    }
    auto result =
        run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                         draft_path = draft;
                         if (missing) {
                             REQUIRE(std::filesystem::remove(draft));
                             return Result<void, Error>::success();
                         }
                         auto  value  = read_draft(draft);
                         auto& fields = std::get<JsonValue::Object>(value.data);
                         if (scenario == "revision") {
                             fields.at("expected_revision") = JsonValue{.data = std::uint64_t{99}};
                         }
                         else if (scenario == "uuid") {
                             fields.at("queue_id") = JsonValue{.data = sequence_id(99).to_string()};
                         }
                         else if (scenario == "unknown") {
                             fields.emplace("sensitive-value", JsonValue{.data = std::string{"secret-default-value"}});
                         }
                         saved = scenario == "json" ? "{ secret-default-value" : serialized(value);
                         if (scenario == "comma") {
                             saved.insert(saved.size() - 1, ",");
                         }
                         write_text(draft, saved);
                         return Result<void, Error>::success();
                     }));
    CHECK(result.code == 2);
    CHECK(result.output.empty());
    CHECK(result.error.find("Recovery command:") == std::string::npos);
    CHECK(result.error.find("secret-default-value") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(draft_path.parent_path() / "update-request.json"));
    if (missing) {
        CHECK(result.error.find("Missing draft:") != std::string::npos);
    }
    else {
        CHECK(result.error.find("Retained draft:") != std::string::npos);
        CHECK(read_text(draft_path) == saved);
    }
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.queue.revision == 1);
}

TEST_CASE("queue workflow refuses file failures before submission and preserves edited work",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         draft_path     = std::filesystem::path{};
    auto                         saved          = std::string{};
    auto                         create_failure = false;
    SECTION("draft directory creation fails")
    {
        create_failure = true;
    }
    SECTION("minimal request cannot overwrite unexpected file")
    {}
    auto launches = 0;
    auto parent   = create_failure ? files.path() / "missing-parent" : files.path();
    auto result   = run_workflow(edit_command(peer), test_io(parent, [&](auto, auto const& draft) {
                                   ++launches;
                                   draft_path = draft;
                                   edit_weight(draft);
                                   saved = read_text(draft);
                                   write_text(draft.parent_path() / "update-request.json", "unexpected file");
                                   return Result<void, Error>::success();
                                 }));
    CHECK(result.code == 2);
    CHECK(result.output.empty());
    CHECK(result.error.find("Recovery command:") == std::string::npos);
    if (create_failure) {
        CHECK(launches == 0);
        CHECK(std::filesystem::is_empty(files.path()));
    }
    else {
        CHECK(read_text(draft_path) == saved);
        CHECK(read_text(draft_path.parent_path() / "update-request.json") == "unexpected file");
        CHECK(result.error.find("No complete minimal request is available") != std::string::npos);
    }
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false);
    CHECK(snapshot.queue.revision == 1);
    CHECK(snapshot.queue.weight == 1);
}

TEST_CASE("queue changes while editing retain the original UUID revision and recovery request",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto const                   parent = files.path() / "operator's $HOME; edit files";
    REQUIRE(std::filesystem::create_directory(parent));
    auto const original          = peer.snapshot().queue;
    auto       command           = edit_command(peer);
    auto       draft_path        = std::filesystem::path{};
    auto       change            = CompetingChange::Weight;
    auto       expected_revision = QueueRevision{2};
    auto       expected_weight   = std::uint32_t{9};
    auto       expected_name     = original.name;
    auto       expected_state    = QueueState::Active;
    SECTION("configuration change")
    {}
    SECTION("rename and old name reuse")
    {
        change          = CompetingChange::Rename;
        expected_weight = 1;
        expected_name   = "renamed-while-editing";
    }
    SECTION("suspension completes")
    {
        change            = CompetingChange::Suspend;
        expected_weight   = 1;
        expected_revision = 3;
        expected_state    = QueueState::Suspended;
    }
    SECTION("suspend and resume")
    {
        change            = CompetingChange::Resume;
        expected_weight   = 1;
        expected_revision = 4;
    }
    SECTION("delete and old name reuse")
    {
        change            = CompetingChange::Delete;
        expected_weight   = 1;
        expected_revision = 4;
        expected_state    = QueueState::Deleted;
    }
    SECTION("human conflict")
    {
        command.json = false;
    }

    auto io     = test_io(parent, [&](auto, auto const& draft) {
        draft_path = draft;
        edit_weight(draft);
        // The competing operation commits while this draft still holds revision 1.
        peer.synchronize([&](PeerState& state) {
            auto changed = change_queue_while_editing(state, change);
            if (!changed) {
                state.error = changed.error().message;
            }
        });
        return Result<void, Error>::success();
    });
    auto result = run_workflow(command, std::move(io));
    CHECK(result.code == 1);
    CHECK(result.output.empty());
    CHECK(result.error.find("Recovery command:") != std::string::npos);
    if (change != CompetingChange::Delete) {
        CHECK(result.error.find("jobu.queue.revision_conflict") != std::string::npos);
        CHECK(result.error.find("manually reconcile") != std::string::npos);
        CHECK(result.error.find("old revision") != std::string::npos);
    }
    auto snapshot = peer.snapshot();
    check_methods(snapshot, true);
    CHECK(snapshot.queue.id == original.id);
    CHECK(snapshot.queue.weight == expected_weight);
    CHECK(snapshot.queue.revision == expected_revision);
    CHECK(snapshot.queue.name == expected_name);
    CHECK(snapshot.queue.state == expected_state);
    if (snapshot.replacement) {
        CHECK(snapshot.replacement->id != original.id);
        CHECK(snapshot.replacement->name == original.name);
        CHECK(snapshot.replacement->revision == 1);
        CHECK(snapshot.replacement->weight == 1);
    }
    CHECK(std::filesystem::exists(draft_path));
    auto request = parsed(read_text(draft_path.parent_path() / "update-request.json"));
    CHECK(request == snapshot.updates.front());
    CHECK(request.as_object().size() == 3);
    CHECK(request.as_object().at("queue_id").as_string() == original.id.to_string());
    CHECK(request.as_object().at("expected_revision").as_uint() == 1);
    CHECK(request.as_object().at("weight").as_uint() == 5);

    // Decode the displayed shell command through the actual CLI and strict request-file loader, without retrying.
    StandardAttributeRegistry registry;
    auto                      recovered = load_recovery_request(result.error, registry);
    CHECK(recovered.socket_path == command.socket_path);
    CHECK(recovered.timeout == command.timeout);
    CHECK_FALSE(recovered.queue_selector_intent);
    auto outgoing = update_queue_request_to_json(std::get<UpdateQueueRequest>(recovered.request), registry);
    REQUIRE(outgoing);
    CHECK(serialized(*outgoing) == serialized(request));
    CHECK(peer.snapshot().methods == snapshot.methods);
}

TEST_CASE("lost queue update replies preserve the request and require inspecting state",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         committed = false;
    auto                         refused   = false;
    SECTION("disconnected before applying update")
    {}
    SECTION("committed update with lost reply")
    {
        committed = true;
    }
    SECTION("submission connection refused")
    {
        refused = true;
    }
    peer.synchronize([&](PeerState& state) {
        state.submission = committed ? Submission::DisconnectAfterUpdate : Submission::DisconnectBeforeUpdate;
    });
    auto draft_path = std::filesystem::path{};
    auto result     = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   edit_weight(draft);
                                   if (refused) {
                                       peer.synchronize([](PeerState& state) { state.listener.close(); });
                                   }
                                   return Result<void, Error>::success();
                                   }));
    CHECK(result.code == 3);
    CHECK(result.output.empty());
    CHECK(result.error.find("Recovery command:") != std::string::npos);
    if (!refused) {
        CHECK(result.error.find("\"outcome_unknown\":true") != std::string::npos);
        CHECK(result.error.find("Inspect current server state before any retry") != std::string::npos);
    }
    auto snapshot = peer.snapshot();
    check_methods(snapshot, !refused);
    CHECK(snapshot.queue.revision == (committed ? 2 : 1));
    CHECK(snapshot.queue.weight == (committed ? 5 : 1));
    auto request = parsed(read_text(draft_path.parent_path() / "update-request.json"));
    CHECK(request.as_object().at("expected_revision").as_uint() == 1);
    CHECK(std::filesystem::exists(draft_path));
    if (!refused) {
        CHECK(request == snapshot.updates.front());
    }
}

TEST_CASE("queue editor time is outside real Session lifetimes and both budgets remain complete",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    StandardAttributeRegistry    registry;
    jb::test::FakeTimeSource     clock;
    auto                         command        = edit_command(peer);
    auto                         modes          = std::vector<SessionMode>{};
    auto                         fetch_deadline = TimePoint{};
    auto                         io             = test_io(files.path(), [&](auto, auto const& draft) {
        CHECK(Application::instance() == nullptr);
        CHECK(ThreadCtx::current()->event_loop() == nullptr);
        clock.advance(1h);
        CHECK(clock.monotonic_now() > fetch_deadline);
        edit_weight(draft);
        return Result<void, Error>::success();
    });
    io.run                                      = [&](Command next, SessionMode mode) {
        modes.push_back(mode);
        CHECK(next.timeout == command.timeout);
        if (mode == SessionMode::Capture) {
            fetch_deadline = clock.monotonic_now() + next.timeout;
        }
        else {
            CHECK(clock.monotonic_now() > fetch_deadline);
        }
        return run_session(std::move(next), registry, mode);
    };
    auto result = run_workflow(command, std::move(io));
    CHECK(result.code == 0);
    CHECK(modes == std::vector<SessionMode>{SessionMode::Capture, SessionMode::Render});
    check_methods(peer.snapshot(), true);
}

TEST_CASE("queue terminal and editor configuration failures precede the Capture fetch",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         command = edit_command(peer);
    auto                         io      = test_io(files.path(), [](auto, auto const&) {
        FAIL("editor must not launch");
        return Result<void, Error>::success();
    });
    auto                         runs    = 0;
    io.run                               = [&](Command const&, SessionMode) {
        ++runs;
        return SessionResult{.exit_code = 3};
    };
    SECTION("terminal refusal")
    {
        io.check_terminal = [] {
            return Result<void, Error>::failure(
                {.category = ErrorCategory::InvalidArgument, .code = "fixture.terminal", .message = "No terminal"});
        };
    }
    SECTION("malformed editor command")
    {
        std::get<InteractiveQueueUpdate>(command.request).editor = "'unclosed";
    }
    auto result = run_workflow(command, std::move(io));
    CHECK(result.code == 2);
    CHECK(result.output.empty());
    CHECK(runs == 0);
    CHECK(std::filesystem::is_empty(files.path()));
    CHECK(peer.snapshot().methods.empty());
}

TEST_CASE("queue Capture failures and ambiguous UUID names never open an editor or claim a mutation",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto const                   original  = peer.snapshot().queue;
    auto                         selection = std::vector<std::string>{"missing-queue"};
    auto                         ambiguous = false;
    SECTION("missing name")
    {}
    SECTION("ID and exact name refer to different queues")
    {
        ambiguous  = true;
        auto upper = original.id.to_string();
        upper.replace(0, 2, "AB");
        selection = {upper};
        peer.synchronize([&](PeerState& state) {
            auto created = state.service->create_queue({.name = upper});
            if (!created) {
                state.error = created.error().message;
            }
        });
    }
    auto result = run_workflow(edit_command(peer, selection), test_io(files.path(), [](auto, auto const&) {
                                   FAIL("editor must not launch");
                                   return Result<void, Error>::success();
                               }));
    CHECK(result.code == 1);
    CHECK(result.output.empty());
    CHECK(result.error.find(ambiguous ? "jobuctl.queue.ambiguous_selector" : "jobu.queue.not_found") !=
          std::string::npos);
    auto error = parsed(result.error);
    CHECK_FALSE(error.as_object().at("error").as_object().at("outcome_unknown").as_bool());
    CHECK(std::filesystem::is_empty(files.path()));
    auto snapshot = peer.snapshot();
    check_methods(snapshot, false, ambiguous);
    CHECK(snapshot.queue.revision == 1);
}

TEST_CASE("queue coordinator rejects mismatched captured identity or a deleted reply before editing",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         queue   = peer.snapshot().queue;
    auto                         command = edit_command(peer, {"id:" + queue.id.to_string()});
    auto                         io      = test_io(files.path(), [](auto, auto const&) {
        FAIL("editor must not launch");
        return Result<void, Error>::success();
    });
    SECTION("different UUID")
    {
        queue.id = sequence_id(99);
    }
    SECTION("deleted")
    {
        queue.state = QueueState::Deleted;
    }
    io.run = [&](Command const&, SessionMode mode) {
        CHECK(mode == SessionMode::Capture);
        return SessionResult{.exit_code = 0, .reply = ControlReply{queue}};
    };
    auto result = run_workflow(command, std::move(io));
    CHECK(result.code == 3);
    CHECK(result.output.empty());
    CHECK(result.error.find("jobuctl.edit.invalid_reply") != std::string::npos);
    CHECK(std::filesystem::is_empty(files.path()));
}

TEST_CASE("queue cleanup retains unexpected editor backups and reports their directory",
          "[jobuctl][queue-edit][workflow]")
{
    ServicePeer                  peer;
    jb::test::TemporaryDirectory files;
    auto                         changed = false;
    SECTION("no-op cleanup")
    {}
    SECTION("cleanup after a committed update")
    {
        changed = true;
    }
    auto draft_path = std::filesystem::path{};
    auto result     = run_workflow(edit_command(peer), test_io(files.path(), [&](auto, auto const& draft) {
                                   draft_path = draft;
                                   if (changed) {
                                       edit_weight(draft);
                                   }
                                   write_text(draft.parent_path() / "editor.backup", "operator backup");
                                   return Result<void, Error>::success();
                                   }));
    CHECK(result.code == 3);
    CHECK(result.error.find("Retained edit directory:") != std::string::npos);
    CHECK(result.error.find("Recovery command:") == std::string::npos);
    CHECK(read_text(draft_path.parent_path() / "editor.backup") == "operator backup");
    auto snapshot = peer.snapshot();
    check_methods(snapshot, changed);
    CHECK(snapshot.queue.revision == (changed ? 2 : 1));
    CHECK(snapshot.queue.weight == (changed ? 5 : 1));
    if (changed) {
        CHECK(parsed(result.output).as_object().at("revision").as_uint() == 2);
    }
    else {
        CHECK(result.output.empty());
    }
}
