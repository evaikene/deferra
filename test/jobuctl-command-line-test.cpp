#include "attribute_registry.hpp"
#include "byte_buffer.hpp"
#include "cli_job_payload_priv.hpp"
#include "command_line_priv.hpp"
#include "command_registry_priv.hpp"
#include "control_json.hpp"
#include "help_priv.hpp"
#include "history.hpp"
#include "history_json.hpp"
#include "input_priv.hpp"
#include "jobu_paths_priv.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "payload_template_priv.hpp"
#include "queue_selector_priv.hpp"
#include "statistics_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/temporary_directory.hpp"
#include "utc_timestamp.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant> // IWYU pragma: keep for std::get in Catch assertions
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;

namespace {

constexpr auto job_id = "00000000-0000-7000-8000-000000000001";

// Destroy the lexer inputs and registry before inspecting the result: parsed commands must own their data.
auto parse_action(std::vector<std::string> arguments) -> ParseResult
{
    arguments.insert(arguments.begin(), "jobuctl");
    auto argv = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    StandardAttributeRegistry registry;
    return parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
}

// Existing parity cases examine remote requests; local-action cases use parse_action directly.
auto parse(std::vector<std::string> arguments) -> CommandBuildResult
{
    auto result = parse_action(std::move(arguments));
    if (result.action) {
        if (auto* command = std::get_if<Command>(&*result.action)) {
            return {.command = std::move(*command)};
        }
    }
    return {.error = std::move(result.error)};
}

auto parse_cli_job_create(std::vector<std::string> options, std::string action = "create") -> CommandBuildResult
{
    auto arguments = std::vector<std::string>{"--socket",
                                              "fixture.sock",
                                              "job",
                                              std::move(action),
                                              "--queue-name",
                                              "reports",
                                              "--type",
                                              "cli",
                                              "--at",
                                              "2030-01-01T00:00:00Z",
                                              "--command",
                                              "/bin/true"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return parse(std::move(arguments));
}

} // namespace

TEST_CASE("jobuctl selects only bare queue updates for interactive editing", "[jobuctl][parse][queue][edit]")
{
    struct Selection {
        std::vector<std::string> arguments;
        QueueSelectionForm       form;
        std::string              text;
        std::string              value;
    };

    for (auto const& selection : std::vector<Selection>{
             {.arguments = {"reports"},                   .form = QueueSelectionForm::ExactName,  .text = "reports", .value = "reports"},
             {.arguments = {"name:id:reports"},
              .form      = QueueSelectionForm::ExactName,
              .text      = "name:id:reports",
              .value     = "id:reports"                                                                                                },
             {.arguments = {"--name", "id:reports"},
              .form      = QueueSelectionForm::ExactName,
              .text      = "id:reports",
              .value     = "id:reports"                                                                                                },
             {.arguments = {job_id},                      .form = QueueSelectionForm::InferredId, .text = job_id,    .value = job_id   },
             {.arguments = {"id:" + std::string{job_id}},
              .form      = QueueSelectionForm::ExplicitId,
              .text      = "id:" + std::string{job_id},
              .value     = job_id                                                                                                      },
             {.arguments = {"--id", job_id},
              .form      = QueueSelectionForm::ExplicitId,
              .text      = job_id,
              .value     = job_id                                                                                                      }
    }) {
        auto arguments = std::vector<std::string>{"queue", "update"};
        arguments.insert(arguments.end(), selection.arguments.begin(), selection.arguments.end());
        arguments.insert(arguments.end(), {"--socket", "edit.sock", "--timeout", "1234", "--json"});
        auto bare = parse(arguments);
        REQUIRE(bare.command);
        CHECK(bare.command->kind == CommandKind::QueueUpdate);
        CHECK(bare.command->socket_path == "edit.sock");
        CHECK(bare.command->timeout == std::chrono::milliseconds{1234});
        CHECK(bare.command->json);
        auto const& edit = std::get<InteractiveQueueUpdate>(bare.command->request);
        CHECK_FALSE(edit.editor);
        REQUIRE(bare.command->queue_selector_intent);
        CHECK(bare.command->queue_selector_intent->form == selection.form);
        CHECK(bare.command->queue_selector_intent->original_text == selection.text);
        if (selection.form == QueueSelectionForm::ExactName) {
            CHECK(std::get<std::string>(edit.queue) == selection.value);
        }
        else {
            CHECK(std::get<Uuid>(edit.queue).to_string() == selection.value);
        }
        arguments.insert(arguments.end(), {"--editor", "'editor path' --wait"});
        auto overridden = parse(std::move(arguments));
        REQUIRE(overridden.command);
        CHECK(std::get<InteractiveQueueUpdate>(overridden.command->request).editor == "'editor path' --wait");
    }

    // Each mutable option still selects an explicit update, even when its value means clearing or inheritance.
    jb::test::TemporaryDirectory directory;
    auto const                   defaults = directory.path() / "defaults.json";
    {
        std::ofstream{defaults} << "{}";
    }
    for (auto const& fields : std::vector<std::vector<std::string>>{
             {"--new-name", "changed"},
             {"--weight", "2"},
             {"--concurrency-limit", "2"},
             {"--recovery-policy", "retry_interrupted"},
             {"--defaults-file", defaults.string()},
             {"--history-retention-seconds", "0"},
             {"--inherit-history-retention"},
             {"--runnable-wait-warning-ms", "0"}
    }) {
        CAPTURE(fields);
        auto arguments = std::vector<std::string>{"queue", "update", "reports"};
        arguments.insert(arguments.end(), fields.begin(), fields.end());
        CHECK(parse(arguments).error == "queue update requires --revision with the inspected queue revision");
        arguments.insert(arguments.end(), {"--editor", "editor"});
        CHECK(parse(arguments).error == "--editor is allowed only with a bare interactive queue update");
        arguments.insert(arguments.end(), {"--revision", "7"});
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
    for (auto const& suffix : std::vector<std::vector<std::string>>{
             {"--editor", ""},
             {"--editor", "one", "--editor", "two"},
             {"--editor", "one", "--revision", "7"},
             {"--editor", "one", "--request-file", "input.json"}
    }) {
        auto arguments = std::vector<std::string>{"queue", "update", "reports"};
        arguments.insert(arguments.end(), suffix.begin(), suffix.end());
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
    CHECK(parse({"queue", "update", "reports", "--revision", "7"}).error ==
          "queue update requires at least one mutable field");
    auto help = parse_action({"queue", "update", "--editor", "'unterminated", "--help"});
    REQUIRE(help.action);
    CHECK(std::holds_alternative<HelpCommand>(*help.action));
}

TEST_CASE("jobuctl selects only bare job updates for interactive editing", "[jobuctl][parse][edit]")
{
    auto bare = parse({"job", "update", job_id, "--json", "--timeout", "1234", "--socket", "edit.sock"});
    REQUIRE(bare.command);
    CHECK(bare.command->kind == CommandKind::JobUpdate);
    CHECK(bare.command->socket_path == "edit.sock");
    CHECK(bare.command->timeout == std::chrono::milliseconds{1234});
    CHECK(bare.command->json);
    auto const& edit = std::get<InteractiveJobUpdate>(bare.command->request);
    CHECK(edit.job_id.to_string() == job_id);
    CHECK_FALSE(edit.editor);

    auto override = parse({"job", "update", job_id, "--editor", "'editor path' --wait"});
    REQUIRE(override.command);
    CHECK(std::get<InteractiveJobUpdate>(override.command->request).editor == "'editor path' --wait");

    auto mutation = parse({"job", "update", job_id, "--revision", "7", "--priority", "50"});
    REQUIRE(mutation.command);
    CHECK(std::get<UpdateJobRequest>(mutation.command->request).expected_revision == 7);
    CHECK(parse({"job", "update", job_id, "--priority", "50"}).error == "job update requires --revision");
    CHECK(parse({"job", "update", job_id, "--revision", "7"}).error ==
          "job update requires at least one mutable field");

    for (auto const& fields : std::vector<std::vector<std::string>>{
             {"--name", "changed"},
             {"--clear-name"},
             {"--attribute", "retry.max_attempts=2"},
             {"--at", "2099-01-01T00:00:00Z"},
             {"--cron", "@daily"}
    }) {
        auto arguments = std::vector<std::string>{"job", "update", job_id};
        arguments.insert(arguments.end(), fields.begin(), fields.end());
        CAPTURE(fields);
        CHECK(parse(std::move(arguments)).error == "job update requires --revision");
    }

    for (auto const& suffix : std::vector<std::vector<std::string>>{
             {"--editor", ""},
             {"--editor", "one", "--editor", "two"},
             {"--editor", "one", "--priority", "50"},
             {"--editor", "one", "--revision", "7"},
             {"--editor", "one", "--request-file", "input.json"}
    }) {
        auto arguments = std::vector<std::string>{"job", "update", job_id};
        arguments.insert(arguments.end(), suffix.begin(), suffix.end());
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
    auto file = parse({"job", "update", "--request-file", "-"});
    REQUIRE(file.command);
    CHECK(file.command->request_file == "-");
    CHECK_FALSE(parse({"job", "update", job_id, "--request-file", "input.json"}).command);

    // Help resolves before malformed editor configuration is interpreted or any terminal is inspected.
    auto help = parse_action({"job", "update", "--help", "--editor", "'unterminated"});
    REQUIRE(help.action);
    CHECK(std::holds_alternative<HelpCommand>(*help.action));

    auto literal = parse_cli_job_create({"--arg", "--editor"});
    REQUIRE(literal.command);
    CHECK(std::get<CreateJobRequest>(literal.command->request)
              .payload.as_object()
              .at("arguments")
              .as_array()
              .front()
              .as_string() == "--editor");
}

TEST_CASE("jobuctl parser retains socket selection and command family errors", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket", "/tmp/custom.sock", "system", "info"});
    REQUIRE(parsed.command);
    CHECK(parsed.command->socket_path == "/tmp/custom.sock");
    CHECK(parsed.command->method == "system.info");
    CHECK(std::holds_alternative<std::monostate>(parsed.command->request));

    CHECK(parse({"system", "info", "--socket", "/tmp/custom.sock"}).command.has_value());
    CHECK(parse({"--socket", "/tmp/custom.sock", "system", "info", "extra"}).error == "unexpected extra operand");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "unknown"}).error == "unknown command action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "job", "unknown"}).error == "unknown command action");
    CHECK(parse({"--socket", "/tmp/custom.sock", "queue", "list", "--socket", "/tmp/other.sock"}).error ==
          "--socket may be supplied only once");
}

TEST_CASE("jobuctl parser shares the compiled endpoint across typed and request-file commands", "[jobuctl][parse]")
{
    auto typed = parse({"queue", "list", "--json", "--timeout", "75"});
    REQUIRE(typed.command);
    CHECK(typed.command->socket_path == jb::jobu::detail::default_socket_path);
    CHECK(typed.command->method == "queue.list");
    CHECK(typed.command->json);
    CHECK(typed.command->timeout == std::chrono::milliseconds{75});
    CHECK(std::holds_alternative<QueueListRequest>(typed.command->request));

    // Parsing selects the raw-input branch without reading its file; main resolves local actions first.
    auto raw = parse({"queue", "create", "--request-file", "unread-request.json"});
    REQUIRE(raw.command);
    CHECK(raw.command->socket_path == typed.command->socket_path);
    CHECK(raw.command->method == "queue.create");
    CHECK(raw.command->request_file == "unread-request.json");
    CHECK(std::holds_alternative<std::monostate>(raw.command->request));

    auto alias = parse({"queue", "add", "reports"});
    REQUIRE(alias.command);
    CHECK(alias.command->socket_path == typed.command->socket_path);
    CHECK(alias.command->method == raw.command->method);
}

TEST_CASE("jobuctl explicit sockets override the default without weakening flag validation", "[jobuctl][parse]")
{
    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"--socket", "custom.sock", "queue", "list"},
             {"queue", "--socket", "custom.sock", "list"},
             {"queue", "list", "--socket", "custom.sock"},
             {"queue", "list", "--socket=custom.sock"},
             {"queue", "create", "--request-file", "request.json", "--socket", "custom.sock"}
    }) {
        CAPTURE(arguments);
        auto result = parse(arguments);
        REQUIRE(result.command);
        CHECK(result.command->socket_path == "custom.sock");
    }

    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"queue", "list", "--socket", ""},
             {"queue", "list", "--socket="},
             {"queue", "list", "--socket"},
             {"--socket", "one.sock", "queue", "list", "--socket=two.sock"}
    }) {
        CAPTURE(arguments);
        auto result = parse(arguments);
        CHECK_FALSE(result.command);
        CHECK_FALSE(result.error.empty());
    }
}

TEST_CASE("jobuctl job list builds explicit state and deletion filters", "[jobuctl][parse][job][list]")
{
    using Case       = std::tuple<std::vector<std::string>, std::optional<JobState>, bool>;
    auto const cases = std::vector<Case>{
        {{},                                            JobState::Active,     false},
        {{"--state", "active"},                         JobState::Active,     false},
        {{"--state", "suspending"},                     JobState::Suspending, false},
        {{"--state", "suspended"},                      JobState::Suspended,  false},
        {{"--state", "succeeded"},                      JobState::Succeeded,  false},
        {{"--state", "failed"},                         JobState::Failed,     false},
        {{"--state", "cancelled"},                      JobState::Cancelled,  false},
        {{"--state", "deleted"},                        JobState::Deleted,    true },
        {{"--state", "deleted", "--include-deleted"},   JobState::Deleted,    true },
        {{"--all"},                                     std::nullopt,         false},
        {{"--include-deleted"},                         std::nullopt,         true },
        {{"--all", "--include-deleted"},                std::nullopt,         true },
        {{"--state", "succeeded", "--include-deleted"}, JobState::Succeeded,  true },
    };
    for (auto const& [options, expected_state, include_deleted] : cases) {
        CAPTURE(options);
        auto arguments = std::vector<std::string>{"--socket", "fixture.sock", "job", "list"};
        arguments.insert(arguments.end(), options.begin(), options.end());
        auto parsed = parse(std::move(arguments));
        REQUIRE(parsed.command);
        auto const& request = std::get<JobListRequest>(parsed.command->request);
        CHECK(request.state == expected_state);
        CHECK(request.include_deleted == include_deleted);

        auto encoded = job_list_request_to_json(request);
        REQUIRE(encoded);
        CHECK(encoded->as_object().contains("state") == expected_state.has_value());
        CHECK(encoded->as_object().at("include_deleted").as_bool() == include_deleted);
    }

    auto scoped = parse({"--socket",
                         "fixture.sock",
                         "job",
                         "list",
                         "--queue-name",
                         "reports",
                         "--state",
                         "failed",
                         "--limit",
                         "2",
                         "--after",
                         job_id});
    REQUIRE(scoped.command);
    auto const& request = std::get<JobListRequest>(scoped.command->request);
    REQUIRE(request.queue);
    CHECK(std::get<std::string>(*request.queue) == "reports");
    CHECK(request.state == JobState::Failed);
    CHECK(request.page.limit == 2);
    auto after_id = Uuid::parse(job_id);
    REQUIRE(after_id);
    CHECK(request.page.after_id == *after_id);

    auto deleted_queue =
        parse({"--socket", "fixture.sock", "job", "list", "--queue-name", "gone", "--state", "deleted"});
    REQUIRE(deleted_queue.command);
    auto const& deleted_request = std::get<JobListRequest>(deleted_queue.command->request);
    REQUIRE(deleted_request.queue);
    CHECK(std::get<std::string>(*deleted_request.queue) == "gone");
    CHECK(deleted_request.state == JobState::Deleted);
    CHECK(deleted_request.include_deleted);
}

TEST_CASE("jobuctl job list rejects conflicting or invalid state options locally", "[jobuctl][parse][job][list]")
{
    for (auto const& options : std::vector<std::vector<std::string>>{
             {"--all", "--state", "active"},
             {"--state", "active", "--all"},
             {"--all", "--all"},
             {"--state", "failed", "--state", "cancelled"},
             {"--state", "ACTIVE"},
             {"--state", "unknown"},
             {"--state", ""},
             {"--state"},
             {"--all=true"},
    }) {
        CAPTURE(options);
        auto arguments = std::vector<std::string>{"--socket", "fixture.sock", "job", "list"};
        arguments.insert(arguments.end(), options.begin(), options.end());
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
}

TEST_CASE("jobuctl job list request files keep raw state defaults", "[jobuctl][input][job][list]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path = directory.path() / "jobs.json";

    auto ordinary = parse({"--socket", "fixture.sock", "job", "list"});
    REQUIRE(ordinary.command);
    CHECK(std::get<JobListRequest>(ordinary.command->request).state == JobState::Active);

    auto file = parse({"--socket", "fixture.sock", "job", "list", "--request-file", path.string()});
    REQUIRE(file.command);
    for (auto const& document : {R"({})", R"({"state":"failed"})"}) {
        auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(output);
        output << document;
        output.close();

        REQUIRE(load_request_file(*file.command, registry));
        auto const& request = std::get<JobListRequest>(file.command->request);
        CHECK(request.state == (document == std::string_view{R"({})"} ? std::optional<JobState>{}
                                                                      : std::optional<JobState>{JobState::Failed}));
    }

    CHECK_FALSE(parse({"--socket", "fixture.sock", "job", "list", "--all", "--request-file", path.string()}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "job", "list", "--state", "failed", "--request-file", path.string()})
                    .command);
}

TEST_CASE("jobuctl job list help explains local defaults without a socket", "[jobuctl][help][job][list]")
{
    auto parsed = parse_action({"job", "list", "--help"});
    REQUIRE(parsed.action);
    auto const& help = render_help(std::get<HelpCommand>(*parsed.action));
    CHECK(help.find("Defaults to active jobs") != std::string::npos);
    CHECK(help.find("--state STATE") != std::string::npos);
    CHECK(help.find("--all") != std::string::npos);
    CHECK(help.find("--include-deleted alone") != std::string::npos);
    CHECK(help.find("--request-file uses the supplied JSON") != std::string::npos);
}

TEST_CASE("jobuctl parses machine options before or after the command", "[jobuctl][parse]")
{
    auto first = parse({"--json", "--timeout", "75", "--socket", "fixture.sock", "queue", "list"});
    auto last  = parse({"--socket", "fixture.sock", "queue", "list", "--timeout=75", "--json"});
    REQUIRE(first.command);
    REQUIRE(last.command);
    CHECK(first.command->json);
    CHECK(last.command->json);
    CHECK(first.command->timeout == std::chrono::milliseconds{75});
    CHECK(last.command->timeout == first.command->timeout);

    auto file = parse({"queue", "create", "--socket", "fixture.sock", "--request-file", "request.json"});
    REQUIRE(file.command);
    CHECK(file.command->request_file == std::filesystem::path{"request.json"});
    CHECK(std::holds_alternative<std::monostate>(file.command->request));
    CHECK_FALSE(
        parse({"queue", "create", "reports", "--request-file", "request.json", "--socket", "fixture.sock"}).command);

    for (auto const* invalid : {"0", "-1", "18446744073709551615", "abc"}) {
        CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "list", "--timeout", invalid}).command);
    }
    auto syntax = parse_action({"queue", "list", "--unknown", "--json"});
    CHECK_FALSE(syntax.action);
    CHECK(syntax.json_requested);
    CHECK_FALSE(parse_action({"queue", "list", "--arg=--json"}).json_requested);
}

TEST_CASE("jobuctl statistics options preserve typed filters and cursor-only continuation", "[jobuctl][parse]")
{
    auto system = parse({"--socket",       "fixture.sock",
                         "system",         "stats",
                         "--queue-id",     job_id,
                         "--job-id",       job_id,
                         "--type",         "cli",
                         "--origin",       "manual",
                         "--planned-from", "2030-01-01T00:00:00Z",
                         "--planned-to",   "2030-01-02T00:00:00Z",
                         "--group-by",     "state",
                         "--limit",        "2"});
    REQUIRE(system.command);
    CHECK(system.command->method == "system.stats");
    auto const& query = std::get<StatisticsRequest>(std::get<StatisticsListRequest>(system.command->request));
    CHECK(query.queue_id.has_value());
    CHECK(query.job_id.has_value());
    CHECK(query.type == JobType::Cli);
    CHECK(query.origin == RunOrigin::Manual);
    CHECK(query.planned.from.has_value());
    CHECK(query.planned.to.has_value());
    CHECK(query.group_by == StatisticsGroupBy::State);
    CHECK(query.limit == 2);

    auto queue = parse({"--socket", "fixture.sock", "queue", "stats", "--name", "reports", "--group-by", "job"});
    REQUIRE(queue.command);
    CHECK(queue.command->method == "queue.stats");
    auto const& scoped = std::get<QueueStatisticsQuery>(std::get<QueueStatisticsListRequest>(queue.command->request));
    CHECK(std::get<std::string>(scoped.selector) == "reports");
    CHECK(scoped.statistics.group_by == StatisticsGroupBy::Job);

    auto continuation = parse({"--socket", "fixture.sock", "queue", "stats", "--cursor", "opaque-token"});
    REQUIRE(continuation.command);
    CHECK(std::get<CursorRequest>(std::get<QueueStatisticsListRequest>(continuation.command->request)).cursor ==
          "opaque-token");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "stats"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "stats", "--id", job_id, "--name", "reports"}).command);
    CHECK_FALSE(
        parse({"--socket", "fixture.sock", "queue", "stats", "--name", "reports", "--cursor", "token"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "system", "stats", "--cursor", "token", "--limit", "2"}).command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "system",
                       "stats",
                       "--planned-from",
                       "2030-01-02T00:00:00Z",
                       "--planned-to",
                       "2030-01-01T00:00:00Z"})
                    .command);
}

TEST_CASE("jobuctl schedule preview uses cron-only typed requests", "[jobuctl][parse]")
{
    auto valid = parse({"--socket", "fixture.sock", "schedule", "validate", "0 9 * * FRI-MON"});
    REQUIRE(valid.command);
    CHECK(valid.command->method == "schedule.validate");
    CHECK_FALSE(std::get<CronScheduleInput>(valid.command->request).timezone);

    auto next = parse({"--socket",
                       "fixture.sock",
                       "schedule",
                       "next",
                       "@daily",
                       "--timezone",
                       "Europe/Tallinn",
                       "--after",
                       "2030-01-01T00:00:00Z",
                       "--count",
                       "2"});
    REQUIRE(next.command);
    CHECK(next.command->method == "schedule.next");
    auto const& request = std::get<ScheduleNextRequest>(next.command->request);
    CHECK(request.schedule.expression == "@daily");
    CHECK(request.schedule.timezone == "Europe/Tallinn");
    CHECK(request.count == 2);

    CHECK_FALSE(parse({"--socket", "fixture.sock", "schedule", "validate"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "schedule", "next", "@daily"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "schedule", "next", "@daily", "--after", "2030-01-01"}).command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "schedule",
                       "next",
                       "@daily",
                       "--after",
                       "2030-01-01T00:00:00Z",
                       "--count",
                       "201"})
                    .command);
}

TEST_CASE("jobuctl statistics and schedule request files use strict public codecs", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   file_path = directory.path() / "params.json";

    struct Case {
        std::vector<std::string> command;
        std::string              document;
    };

    auto const cases = std::vector<Case>{
        {.command = {"system", "stats"},       .document = R"({"group_by":"state","limit":2})"                                },
        {.command = {"queue", "stats"},        .document = R"({"queue_name":"reports","group_by":"job"})"                     },
        {.command  = {"schedule", "validate"},
         .document = R"({"schedule":{"kind":"cron","expression":"@daily","timezone":"UTC"}})"                                 },
        {.command = {"schedule", "next"},
         .document =
             R"({"schedule":{"kind":"cron","expression":"@daily","timezone":"UTC"},"after":"2030-01-01T00:00:00Z","count":2})"},
    };
    for (auto const& item : cases) {
        auto file = std::ofstream{file_path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << item.document;
        file.close();

        auto arguments = std::vector<std::string>{"--socket", "fixture.sock"};
        arguments.insert(arguments.end(), item.command.begin(), item.command.end());
        arguments.insert(arguments.end(), {"--request-file", file_path.string()});
        auto parsed = parse(std::move(arguments));
        REQUIRE(parsed.command);
        REQUIRE(load_request_file(*parsed.command, registry));

        file.open(file_path, std::ios::binary | std::ios::trunc);
        REQUIRE(file);
        file << R"({"unexpected":true})";
        file.close();
        CHECK_FALSE(load_request_file(*parsed.command, registry));
    }
}

TEST_CASE("jobuctl request files are bounded and strictly decoded", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path = directory.path() / "request.json";
    auto command = parse({"--socket", "fixture.sock", "queue", "list", "--request-file", path.string()});
    REQUIRE(command.command);

    {
        auto file = std::ofstream{path, std::ios::binary};
        REQUIRE(file);
        file << "{\"limit\":2}";
    }
    REQUIRE(load_request_file(*command.command, registry));
    CHECK(std::get<QueueListRequest>(command.command->request).page.limit == 2);

    for (auto const& document : {"[]", R"({"limit":2} trailing)", R"({"limit":2,"unexpected":1})"}) {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << document;
        file.close();

        auto result = load_request_file(*command.command, registry);
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find(document) == std::string::npos);
    }

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << std::string((1024U * 1024U) + 1U, 'x');
    }
    auto oversized = load_request_file(*command.command, registry);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobuctl.input.too_large");

    auto const at = parse_utc_timestamp("2030-01-01T00:00:00Z");
    REQUIRE(at);
    auto payload = parse_json(R"({"command":"/bin/true","arguments":[{"secret":"reports.token"}]})");
    REQUIRE(payload);
    auto create = CreateJobRequest{
        .queue    = std::string{"reports"},
        .type     = JobType::Cli,
        .schedule = OnceSchedule{.planned_at = *at},
        .payload  = *payload,
    };
    auto encoded = create_job_request_to_json(create, registry);
    REQUIRE(encoded);
    auto serialized = serialize_json(*encoded);
    REQUIRE(serialized);
    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << *serialized;
    }
    auto job = parse({"--socket", "fixture.sock", "job", "create", "--request-file", path.string()});
    REQUIRE(job.command);
    REQUIRE(load_request_file(*job.command, registry));
    CHECK(std::get<CreateJobRequest>(job.command->request).payload == *payload);

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file
            << R"({"job_id":"00000000-0000-7000-8000-000000000001","expected_revision":7,"name":null,"type":"http","schedule":{"kind":"cron","expression":"0 9 * * *","timezone":"UTC"},"attributes":{"retry.max_attempts":2},"payload":{"url":"https://example.test","headers":[{"name":"Authorization","value":{"secret":"service.token"}}]}})";
    }
    auto update = parse({"--socket", "fixture.sock", "job", "update", "--request-file", path.string()});
    REQUIRE(update.command);
    REQUIRE(load_request_file(*update.command, registry));
    auto const& update_request = std::get<UpdateJobRequest>(update.command->request);
    REQUIRE(update_request.name);
    CHECK_FALSE(*update_request.name);
    CHECK(update_request.expected_revision == 7);
    CHECK(update_request.type == JobType::Http);
    CHECK(std::holds_alternative<CronScheduleInput>(*update_request.schedule));
    CHECK(update_request.attribute_changes.contains("retry.max_attempts"));
    REQUIRE(update_request.payload);
    CHECK(update_request.payload->as_object()
              .at("headers")
              .as_array()
              .front()
              .as_object()
              .at("value")
              .as_object()
              .at("secret")
              .as_string() == "service.token");

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << R"({"queue_name":"reports","expected_revision":7,"defaults":{},"history_retention_seconds":null})";
    }
    auto queue_update = parse({"--socket", "fixture.sock", "queue", "update", "--request-file", path.string()});
    REQUIRE(queue_update.command);
    REQUIRE(load_request_file(*queue_update.command, registry));
    auto const& queue_request = std::get<UpdateQueueRequest>(queue_update.command->request);
    REQUIRE(queue_request.defaults);
    CHECK(queue_request.defaults->empty());
    REQUIRE(queue_request.history_retention);
    CHECK_FALSE(*queue_request.history_retention);
}

TEST_CASE("jobuctl accepts one params object from standard input", "[jobuctl][input]")
{
    auto command = parse({"--socket", "fixture.sock", "queue", "list", "--request-file", "-"});
    REQUIRE(command.command);

    auto                      source   = std::istringstream{"{\"limit\":3}"};
    auto*                     original = std::cin.rdbuf(source.rdbuf());
    StandardAttributeRegistry registry;
    auto                      loaded = load_request_file(*command.command, registry);
    std::cin.rdbuf(original);
    std::cin.clear();

    REQUIRE(loaded);
    CHECK(std::get<QueueListRequest>(command.command->request).page.limit == 3);
}

TEST_CASE("jobuctl shared request decoding owns strict params without CLI defaults", "[jobuctl][input]")
{
    auto decoded = [] {
        StandardAttributeRegistry registry;
        auto                      value = parse_json(
            R"({"job_id":"00000000-0000-7000-8000-000000000001","expected_revision":7,"name":"edited","priority":50})");
        REQUIRE(value);
        return decode_command_request(CommandKind::JobUpdate, *value, registry);
    }();
    REQUIRE(decoded);
    auto const& update = std::get<UpdateJobRequest>(*decoded);
    CHECK(update.job_id.to_string() == job_id);
    CHECK(update.expected_revision == 7);
    REQUIRE(update.name);
    CHECK(*update.name == "edited");
    CHECK(update.priority == 50);
    CHECK_FALSE(update.payload);

    StandardAttributeRegistry registry;
    auto                      empty = parse_json("{}");
    REQUIRE(empty);
    auto system = decode_command_request(CommandKind::SystemInfo, *empty, registry);
    REQUIRE(system);
    CHECK(std::holds_alternative<std::monostate>(*system));
    auto runs = decode_command_request(CommandKind::RunList, *empty, registry);
    REQUIRE(runs);
    auto const& query = std::get<RunQuery>(std::get<RunListRequest>(*runs));
    CHECK_FALSE(query.filters.state);
    CHECK_FALSE(query.filters.states);

    auto cursor = parse_json(R"({"cursor":"opaque-token"})");
    REQUIRE(cursor);
    auto continuation = decode_command_request(CommandKind::RunList, *cursor, registry);
    REQUIRE(continuation);
    CHECK(std::get<CursorRequest>(std::get<RunListRequest>(*continuation)).cursor == "opaque-token");
}

TEST_CASE("jobuctl shared request decoding rejects malformed input with safe errors", "[jobuctl][input]")
{
    StandardAttributeRegistry registry;
    for (auto const* document : {"null", "[]", "true", "42", "\"private-input\""}) {
        auto value = parse_json(document);
        REQUIRE(value);
        for (auto kind : {CommandKind::SystemInfo, CommandKind::JobUpdate, CommandKind::RunList}) {
            auto decoded = decode_command_request(kind, *value, registry);
            REQUIRE_FALSE(decoded);
            CHECK(decoded.error().code == "jobuctl.input.invalid_params");
            CHECK(decoded.error().message == "Request parameters are invalid");
        }
    }

    for (auto const* document :
         {R"({"job_id":"00000000-0000-7000-8000-000000000001","priority":50})",
          R"({"job_id":"00000000-0000-7000-8000-000000000001","expected_revision":7})",
          R"({"job_id":"00000000-0000-7000-8000-000000000001","expected_revision":7,"private-input":50})"}) {
        auto value = parse_json(document);
        REQUIRE(value);
        auto decoded = decode_command_request(CommandKind::JobUpdate, *value, registry);
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().code == "jobuctl.input.invalid_params");
        CHECK(decoded.error().message == "Request parameters are invalid");
    }
    auto unexpected = parse_json(R"({"private-input":true})");
    REQUIRE(unexpected);
    CHECK_FALSE(decode_command_request(CommandKind::SystemInfo, *unexpected, registry));
}

TEST_CASE("jobuctl secret commands select metadata-only requests and deferred input", "[jobuctl][parse]")
{
    auto set = parse({"--socket", "fixture.sock", "secret", "set", "reports.token", "--stdin"});
    REQUIRE(set.command);
    CHECK(set.command->method == "secret.set");
    REQUIRE(set.command->secret_input);
    CHECK(set.command->secret_input->source == SecretInput::Source::Stdin);
    CHECK(std::get<SetSecretRequest>(set.command->request).name == "reports.token");
    CHECK(std::get<SetSecretRequest>(set.command->request).value.empty());

    auto list = parse({"secret", "list", "--socket", "fixture.sock", "--limit", "1", "--after-name", "reports.a"});
    REQUIRE(list.command);
    CHECK(list.command->method == "secret.list");
    CHECK(std::get<SecretListRequest>(list.command->request).limit == 1);
    CHECK(std::get<SecretListRequest>(list.command->request).after_name == "reports.a");

    auto erase = parse({"--socket", "fixture.sock", "secret", "delete", "reports.token"});
    REQUIRE(erase.command);
    CHECK(erase.command->method == "secret.delete");
    CHECK(std::get<std::string>(erase.command->request) == "reports.token");

    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"secret", "set", "reports.token"},
             {"secret", "set", "reports.token", "--file", "a", "--stdin"},
             {"secret", "set", "reports.token", "--stdin", "--value", "literal"},
             {"secret", "set", "reports.token", "--stdin", "--request-file", "request.json"},
             {"secret", "list", "--limit", "0"},
             {"secret", "list", "--after-name", ""},
             {"secret", "delete"},
    }) {
        auto full = arguments;
        full.insert(full.begin(), {"--socket", "fixture.sock"});
        CHECK_FALSE(parse(std::move(full)).command);
    }
}

TEST_CASE("jobuctl reads secret file and stdin as bounded raw bytes", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "secret.bin";
    auto command = parse({"--socket", "fixture.sock", "secret", "set", "reports.token", "--file", path.string()});
    REQUIRE(command.command);

    auto const sample = std::string{"\0\xff\n", 3};
    {
        auto file = std::ofstream{path, std::ios::binary};
        REQUIRE(file);
        file.write(sample.data(), static_cast<std::streamsize>(sample.size()));
    }
    REQUIRE(load_secret_input(*command.command));
    CHECK(as_string_view(std::get<SetSecretRequest>(command.command->request).value) == sample);

    for (auto const size : {std::size_t{0}, std::size_t{65536}, std::size_t{65537}}) {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << std::string(size, 'x');
        file.close();

        auto loaded = load_secret_input(*command.command);
        if (size > 65536) {
            REQUIRE_FALSE(loaded);
            CHECK(loaded.error().code == "jobuctl.input.too_large");
        }
        else {
            REQUIRE(loaded);
            CHECK(std::get<SetSecretRequest>(command.command->request).value.size() == size);
        }
    }

    auto stdin_command = parse({"--socket", "fixture.sock", "secret", "set", "reports.token", "--stdin"});
    REQUIRE(stdin_command.command);
    auto  source   = std::istringstream{sample};
    auto* original = std::cin.rdbuf(source.rdbuf());
    auto  loaded   = load_secret_input(*stdin_command.command);
    std::cin.rdbuf(original);
    std::cin.clear();
    REQUIRE(loaded);
    CHECK(as_string_view(std::get<SetSecretRequest>(stdin_command.command->request).value) == sample);
}

TEST_CASE("jobuctl decodes structured secret requests without exposing input", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path  = directory.path() / "request.json";
    auto                         write = [&](std::string_view document) {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << document;
    };

    auto set = parse({"--socket", "fixture.sock", "secret", "set", "--request-file", path.string()});
    REQUIRE(set.command);
    write(R"({"name":"reports.token","value":{"encoding":"base64","data":"AP8K"}})");
    REQUIRE(load_request_file(*set.command, registry));
    CHECK(as_string_view(std::get<SetSecretRequest>(set.command->request).value) == std::string{"\0\xff\n", 3});

    write(R"({"name":"reports.token","value":{"encoding":"base64","data":"secret-sentinel"}})");
    auto invalid = load_request_file(*set.command, registry);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message.find("secret-sentinel") == std::string::npos);

    auto list = parse({"--socket", "fixture.sock", "secret", "list", "--request-file", path.string()});
    REQUIRE(list.command);
    write(R"({"limit":2,"after_name":"reports.a"})");
    REQUIRE(load_request_file(*list.command, registry));
    CHECK(std::get<SecretListRequest>(list.command->request).limit == 2);

    auto erase = parse({"--socket", "fixture.sock", "secret", "delete", "--request-file", path.string()});
    REQUIRE(erase.command);
    write(R"({"name":"reports.token"})");
    REQUIRE(load_request_file(*erase.command, registry));
    CHECK(std::get<std::string>(erase.command->request) == "reports.token");
}

TEST_CASE("jobuctl parser preserves queue selectors and deletion rendering identity", "[jobuctl][parse]")
{
    for (auto const* action : {"get", "suspend", "resume", "delete"}) {
        CAPTURE(action);
        auto named = parse({"--socket", "fixture.sock", "queue", action, "--name", "queue with spaces"});
        REQUIRE(named.command);
        auto const& selector = std::string_view{action} == "get"
                                 ? std::get<GetQueueRequest>(named.command->request).queue
                                 : std::get<QueueSelector>(named.command->request);
        CHECK(std::get<std::string>(selector) == "queue with spaces");
        CHECK(named.command->method == std::string{"queue."} + action);

        auto identified = parse({"--socket", "fixture.sock", "queue", action, "--id", job_id});
        REQUIRE(identified.command);
        auto const& id_selector = std::string_view{action} == "get"
                                    ? std::get<GetQueueRequest>(identified.command->request).queue
                                    : std::get<QueueSelector>(identified.command->request);
        CHECK(std::get<Uuid>(id_selector).to_string() == job_id);
    }

    auto duplicate = parse({"--socket", "fixture.sock", "queue", "get", "--id", job_id, "--name", "queue"});
    CHECK_FALSE(duplicate.command);
    CHECK(duplicate.error == "expected exactly one selector: SELECTOR, --id UUID, or --name NAME");
}

TEST_CASE("jobuctl parser preserves job identity revisions and signed priority", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket",
                         "fixture.sock",
                         "job",
                         "update",
                         job_id,
                         "--revision",
                         "18446744073709551615",
                         "--priority",
                         "-2147483648",
                         "--clear-name"});
    REQUIRE(parsed.command);
    auto const& request = std::get<UpdateJobRequest>(parsed.command->request);
    CHECK(request.job_id.to_string() == job_id);
    CHECK(request.expected_revision == std::numeric_limits<std::uint64_t>::max());
    CHECK(request.priority == std::numeric_limits<std::int32_t>::min());
    REQUIRE(request.name);
    CHECK_FALSE(*request.name);

    for (auto const* revision : {"0", "-1", "18446744073709551616"}) {
        CAPTURE(revision);
        auto invalid = parse({"--socket", "fixture.sock", "job", "delete", job_id, "--revision", revision});
        CHECK_FALSE(invalid.command);
    }
    for (auto const* action : {"get", "suspend", "resume"}) {
        auto selected = parse({"--socket", "fixture.sock", "job", action, job_id});
        REQUIRE(selected.command);
        CHECK(selected.command->method == std::string{"job."} + action);
        CHECK(std::get<Uuid>(selected.command->request).to_string() == job_id);
    }

    auto deleted = parse({"--socket", "fixture.sock", "job", "delete", job_id, "--revision", "7"});
    REQUIRE(deleted.command);
    auto const& deletion = std::get<DeleteJobRequest>(deleted.command->request);
    CHECK(deletion.expected_revision == 7);
    CHECK(deletion.job_id.to_string() == job_id);
}

TEST_CASE("jobuctl parser retains exact repeated subprocess argument bytes", "[jobuctl][parse]")
{
    auto parsed = parse({"--socket",
                         "fixture.sock",
                         "job",
                         "create",
                         "--queue-name",
                         "queue",
                         "--type",
                         "cli",
                         "--at",
                         "2030-01-01T00:00:00Z",
                         "--command",
                         "/bin/true",
                         "--priority",
                         "-12",
                         "--arg",
                         "",
                         "--arg",
                         "-abc",
                         "--arg",
                         "--unknown",
                         "--arg",
                         "--env",
                         "--arg",
                         "--env=NAME=value",
                         "--arg=--command",
                         "--arg=--help",
                         "--arg=",
                         "--env",
                         "ACTUAL=value"});
    REQUIRE(parsed.command);
    auto const& request = std::get<CreateJobRequest>(parsed.command->request);
    CHECK(request.priority == -12);
    auto expected = parse_json(
        R"({"command":"/bin/true","arguments":["","-abc","--unknown","--env","--env=NAME=value","--command","--help",""],"environment":{"ACTUAL":"value"}})");
    REQUIRE(expected);
    CHECK(request.payload == *expected);
}

TEST_CASE("jobuctl constructs ordered symbolic CLI secret references", "[jobuctl][parse][secret]")
{
    auto options = std::vector<std::string>{"--arg=--token",
                                            "--arg-secret",
                                            "reports.token",
                                            "--arg=--mode",
                                            "--arg",
                                            "daily",
                                            "--arg-secret=reports.token",
                                            "--env",
                                            "MODE=daily",
                                            "--env-secret=REPORT_TOKEN=reports.token",
                                            "--unset-env",
                                            "OLD",
                                            "--env-secret",
                                            "NEXT_TOKEN=reports.next"};
    auto parsed  = parse_cli_job_create(options);
    REQUIRE(parsed.command);
    auto expected = parse_json(
        R"({"command":"/bin/true","arguments":["--token",{"secret":"reports.token"},"--mode","daily",{"secret":"reports.token"}],"environment":{"MODE":"daily","REPORT_TOKEN":{"secret":"reports.token"},"OLD":null,"NEXT_TOKEN":{"secret":"reports.next"}}})");
    REQUIRE(expected);
    CHECK(std::get<CreateJobRequest>(parsed.command->request).payload == *expected);

    auto alias = parse_cli_job_create(std::move(options), "add");
    REQUIRE(alias.command);
    CHECK(alias.command->method == "job.create");
    CHECK(std::get<CreateJobRequest>(alias.command->request).payload == *expected);
}

TEST_CASE("jobuctl counts secret arguments and reference occurrences in shared limits", "[jobuctl][parse][secret]")
{
    auto arguments = std::vector<std::string>(jb::jobu::detail::maximum_cli_arguments - 1U, "--arg=");
    arguments.emplace_back("--arg-secret=reports.token");
    auto accepted_arguments = parse_cli_job_create(arguments);
    REQUIRE(accepted_arguments.command);
    CHECK(std::get<CreateJobRequest>(accepted_arguments.command->request)
              .payload.as_object()
              .at("arguments")
              .as_array()
              .size() == jb::jobu::detail::maximum_cli_arguments);

    arguments.emplace_back("--arg=x");
    CHECK_FALSE(parse_cli_job_create(std::move(arguments)).command);

    // Repeated uses of one name still consume one reference occurrence each.
    auto references = std::vector<std::string>(jb::jobu::detail::maximum_payload_secret_references - 1U,
                                               "--arg-secret=reports.token");
    references.emplace_back("--env-secret=TOKEN=reports.token");
    REQUIRE(parse_cli_job_create(references).command);
    references.emplace_back("--arg-secret=reports.token");
    CHECK_FALSE(parse_cli_job_create(std::move(references)).command);
}

TEST_CASE("jobuctl rejects invalid secret flags before creating a request", "[jobuctl][parse][secret]")
{
    for (auto const& options : std::vector<std::vector<std::string>>{
             {"--arg-secret"},
             {"--arg-secret="},
             {"--arg-secret=Reports.token"},
             {"--arg-secret=reports..token"},
             {"--arg-secret=" + std::string(129, 'a')},
             {"--env-secret"},
             {"--env-secret="},
             {"--env-secret=TOKEN"},
             {"--env-secret==reports.token"},
             {"--env-secret=9TOKEN=reports.token"},
             {"--env-secret=TOKEN="},
             {"--env-secret=TOKEN=Reports.token"},
             {"--env-secret=TOKEN=reports.token=extra"},
             {"--env-secret=PATH=reports.token"},
             {"--env-secret=JOBU_CUSTOM=reports.token"},
             {"--env-secret=JOBU_JOB_ID=reports.token"},
             {"--arg-secret=reports.token", "--request-file", "request.json"},
             {"--env-secret=TOKEN=reports.token", "--request-file", "request.json"},
             {"--request-file", "request.json", "--arg-secret=reports.token"},
    }) {
        CAPTURE(options);
        CHECK_FALSE(parse_cli_job_create(options).command);
    }
}

TEST_CASE("jobuctl rejects duplicate environment keys across literal reference and removal flags",
          "[jobuctl][parse][secret]")
{
    auto const entries = std::array<std::vector<std::string>, 3>{
        std::vector<std::string>{"--env",        "TOKEN=value"        },
        std::vector<std::string>{"--env-secret", "TOKEN=reports.token"},
        std::vector<std::string>{"--unset-env",  "TOKEN"              },
    };
    for (auto const& first : entries) {
        for (auto const& second : entries) {
            auto options = first;
            options.insert(options.end(), second.begin(), second.end());
            CAPTURE(options);
            CHECK_FALSE(parse_cli_job_create(std::move(options)).command);
        }
    }
}

TEST_CASE("jobuctl help is local at root group leaf and alias paths", "[jobuctl][help]")
{
    for (auto const& args : std::vector<std::vector<std::string>>{{}, {"--help"}, {"-h"}, {"help"}}) {
        auto result = parse_action(args);
        REQUIRE(result.action);
        auto const* help = std::get_if<HelpCommand>(&*result.action);
        REQUIRE(help);
        CHECK(help->group.empty());
        auto text = render_help(*help);
        CHECK(text.find("Groups:") != std::string::npos);
        CHECK(text.find("--socket PATH") != std::string::npos);
        CHECK(text.find(jb::jobu::detail::default_socket_path) != std::string::npos);
        CHECK(text.find("--expected-exit-code") == std::string::npos);
    }
    for (auto const& group : command_groups()) {
        for (auto const& args : std::vector<std::vector<std::string>>{
                 {std::string{group.name}},
                 {std::string{group.name}, "--help"},
                 {"help", std::string{group.name}}
        }) {
            auto result = parse_action(args);
            REQUIRE(result.action);
            auto const& help = std::get<HelpCommand>(*result.action);
            CHECK(help.group == group.name);
            CHECK(help.action.empty());
            CHECK(render_help(help).find("Commands:") != std::string::npos);
            CHECK(render_help(help).find(jb::jobu::detail::default_socket_path) != std::string::npos);
        }
    }

    // Every currently supported leaf must explain itself without requiring its operands or a daemon socket.
    for (auto const& spec : command_specs()) {
        for (auto const& args : std::vector<std::vector<std::string>>{
                 {std::string{spec.group}, std::string{spec.name},  "--help"              },
                 {"help",                  std::string{spec.group}, std::string{spec.name}},
                 {"-h",                    std::string{spec.group}, std::string{spec.name}}
        }) {
            CAPTURE(args);
            auto result = parse_action(args);
            REQUIRE(result.action);
            auto text = render_help(std::get<HelpCommand>(*result.action));
            CHECK(text.find(spec.summary) != std::string::npos);
            CHECK(text.find("Example:") != std::string::npos);
            CHECK(text.find(jb::jobu::detail::default_socket_path) != std::string::npos);
            for (auto const& option : spec.options) {
                CHECK(text.find("--" + std::string{option.option.long_name}) != std::string::npos);
            }
        }
        if (!spec.alias.empty()) {
            auto alias = parse_action({std::string{spec.group}, std::string{spec.alias}, "--help"});
            REQUIRE(alias.action);
            auto text = render_help(std::get<HelpCommand>(*alias.action));
            CHECK(text == render_help({std::string{spec.group}, std::string{spec.name}}));
            CHECK(text.find("Alias:") != std::string::npos);
        }
    }
}

TEST_CASE("jobuctl help does not conceal lexical errors or unknown paths", "[jobuctl][help]")
{
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"unknown", "--help"},
             {"queue", "unknown", "--help"},
             {"help", "queue", "unknown"},
             {"queue", "--unknown", "--help"},
             {"queue", "list", "--weight", "2", "--help"},
             {"queue", "create", "--weight", "--help"},
             {"queue", "create", "--weight", "-h"},
             {"queue", "get", "--name", "--help"},
             {"--socket", "--help"},
             {"--socket=", "--help"},
             {"queue", "list", "--include-deleted=true", "--help"},
             {"--help=anything"},
             {"queue", "create", "one", "two", "--help"},
             {"help", "queue", "create", "extra"},
             {"queue", "list", "--", "--help"},
             {"--", "queue", "--help"},
             {"job", "create", "--arg", "--socket", "x", "--help"},
             {"job", "create", "--arg", "--", "--help"},
             {"queue", "--version"},
             {"--version", "--help"},
             {"queue", "list", "--limit", "1", "--limit", "2", "--help"}
    }) {
        CAPTURE(args);
        auto result = parse_action(args);
        CHECK_FALSE(result.action);
        CHECK_FALSE(result.error.empty());
    }
    auto group_error = parse_action({"queue", "unknown", "--help"});
    CHECK(group_error.usage.group == "queue");
    CHECK(group_error.usage.action.empty());
    auto leaf_error = parse_action({"queue", "get", "--name", "--help"});
    CHECK(leaf_error.usage.action == "get");

    // Values need not name a live object or satisfy domain validation merely to request help.
    auto help = parse_action({"queue", "get", "--id", "not-a-uuid", "--help"});
    REQUIRE(help.action);
    CHECK(std::holds_alternative<HelpCommand>(*help.action));
}

TEST_CASE("jobuctl globals and aliases preserve canonical remote requests", "[jobuctl][parse]")
{
    auto expected = parse({"--socket", "fixture.sock", "queue", "create", "reports", "--weight", "2"});
    REQUIRE(expected.command);
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"queue", "add", "reports", "--weight", "2", "--socket", "fixture.sock"},
             {"queue", "--socket", "fixture.sock", "create", "reports", "--weight", "2"},
             {"--socket=fixture.sock", "queue", "add", "reports", "--weight=2"}
    }) {
        auto result = parse(args);
        REQUIRE(result.command);
        CHECK(result.command->method == "queue.create");
        CHECK(result.command->kind == expected.command->kind);
        CHECK(result.command->socket_path == expected.command->socket_path);
        auto const& actual    = std::get<CreateQueueRequest>(result.command->request);
        auto const& reference = std::get<CreateQueueRequest>(expected.command->request);
        CHECK(actual.name == reference.name);
        CHECK(actual.weight == reference.weight);
    }
    auto canonical = parse({"--socket",
                            "fixture.sock",
                            "job",
                            "create",
                            "--queue-name",
                            "reports",
                            "--type",
                            "http",
                            "--at",
                            "2030-01-01T00:00:00Z",
                            "--url",
                            "https://example.test"});
    auto alias     = parse({"job",
                            "add",
                            "--queue-name",
                            "reports",
                            "--type",
                            "http",
                            "--at",
                            "2030-01-01T00:00:00Z",
                            "--url",
                            "https://example.test",
                            "--socket",
                            "fixture.sock"});
    REQUIRE(canonical.command);
    REQUIRE(alias.command);
    CHECK(alias.command->method == "job.create");
    CHECK(alias.command->kind == canonical.command->kind);
    auto const& alias_request     = std::get<CreateJobRequest>(alias.command->request);
    auto const& canonical_request = std::get<CreateJobRequest>(canonical.command->request);
    CHECK(alias_request.queue == canonical_request.queue);
    CHECK(alias_request.type == canonical_request.type);
    CHECK(alias_request.payload == canonical_request.payload);

    CHECK(parse({"queue", "list"}).command);
    CHECK_FALSE(parse_action({"--socket", "one", "queue", "list", "--socket", "two"}).action);
    auto version = parse_action({"--version"});
    REQUIRE(version.action);
    CHECK(std::holds_alternative<VersionCommand>(*version.action));
}

TEST_CASE("jobuctl help-looking option values and terminator operands remain data", "[jobuctl][parse]")
{
    for (auto const& suffix : std::vector<std::vector<std::string>>{
             {"--arg=--help"},
             {"--arg", "--help"},
             {"--arg", "-h"},
             {"--arg", "-ahb"},
             {"--arg", "--version"},
             {"--arg", "--json"},
             {"--arg", "--timeout"},
             {"--arg", "--request-file"},
             {"--arg", "--all"},
             {"--arg", "--unknown"},
             {"--arg", "--env"},
             {"--arg", "--arg-secret"},
             {"--arg", "--arg-secret=reports.token"},
             {"--arg", "--env-secret"},
             {"--arg", "--env-secret=TOKEN=reports.token"}
    }) {
        CAPTURE(suffix);
        auto args = std::vector<std::string>{"job",
                                             "create",
                                             "--socket",
                                             "fixture.sock",
                                             "--queue-name",
                                             "reports",
                                             "--type",
                                             "cli",
                                             "--at",
                                             "2030-01-01T00:00:00Z",
                                             "--command",
                                             "/bin/true"};
        args.insert(args.end(), suffix.begin(), suffix.end());
        auto result = parse(args);
        REQUIRE(result.command);
        auto const& values =
            std::get<CreateJobRequest>(result.command->request).payload.as_object().at("arguments").as_array();
        REQUIRE(values.size() == 1);
        CHECK(values.front().as_string() == (suffix.size() == 1 ? "--help" : suffix.back()));
    }
    for (auto const& args : std::vector<std::vector<std::string>>{
             {"queue", "create", "--socket", "fixture.sock", "--", "--help"},
             {"queue", "create", "--socket", "fixture.sock", "--", "-h"    }
    }) {
        auto result = parse(args);
        REQUIRE(result.command);
        CHECK(std::get<CreateQueueRequest>(result.command->request).name == args.back());
    }
    auto name = parse({"--socket", "fixture.sock", "queue", "get", "--name=--help"});
    REQUIRE(name.command);
    CHECK(std::get<std::string>(std::get<GetQueueRequest>(name.command->request).queue) == "--help");
}

TEST_CASE("jobuctl queue configuration keeps inheritance and clear semantics", "[jobuctl][parse]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "defaults.json";
    {
        auto file = std::ofstream{path};
        REQUIRE(file);
        file << R"({"retry.max_attempts":2})";
    }

    auto created = parse({"--socket",
                          "fixture.sock",
                          "queue",
                          "create",
                          "reports",
                          "--defaults-file",
                          path.string(),
                          "--history-retention-seconds",
                          "0",
                          "--runnable-wait-warning-ms",
                          "250"});
    REQUIRE(created.command);
    auto const& create_request = std::get<CreateQueueRequest>(created.command->request);
    CHECK(create_request.defaults.contains("retry.max_attempts"));
    CHECK(create_request.history_retention == std::chrono::seconds{0});
    CHECK(create_request.runnable_wait_warning == std::chrono::milliseconds{250});

    {
        auto file = std::ofstream{path, std::ios::trunc};
        REQUIRE(file);
        file << "{}";
    }
    auto updated = parse({"--socket",
                          "fixture.sock",
                          "queue",
                          "update",
                          "--revision",
                          "1",
                          "--name",
                          "reports",
                          "--defaults-file",
                          path.string(),
                          "--inherit-history-retention",
                          "--recovery-policy",
                          "retry_interrupted"});
    REQUIRE(updated.command);
    auto const& update_request = std::get<UpdateQueueRequest>(updated.command->request);
    REQUIRE(update_request.defaults);
    CHECK(update_request.defaults->empty());
    REQUIRE(update_request.history_retention);
    CHECK_FALSE(*update_request.history_retention);
    CHECK(update_request.recovery_policy == RecoveryPolicy::RetryInterrupted);

    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "queue",
                       "update",
                       "--revision",
                       "1",
                       "--name",
                       "reports",
                       "--history-retention-seconds",
                       "0",
                       "--inherit-history-retention"})
                    .command);
    CHECK_FALSE(
        parse({"--socket", "fixture.sock", "queue", "create", "reports", "--history-retention-seconds", "-1"}).command);
}

TEST_CASE("jobuctl job schedules and attribute patches use typed request semantics", "[jobuctl][parse]")
{
    auto const base = std::vector<std::string>{"--socket",
                                               "fixture.sock",
                                               "job",
                                               "create",
                                               "--queue-name",
                                               "reports",
                                               "--type",
                                               "cli",
                                               "--command",
                                               "/bin/true"};
    auto       with = [&](std::vector<std::string> suffix) {
        auto args = base;
        args.insert(args.end(), suffix.begin(), suffix.end());
        return parse(std::move(args));
    };

    auto immediate = with({"--now", "--attribute", "retry.max_attempts=2"});
    REQUIRE(immediate.command);
    auto const& create_request = std::get<CreateJobRequest>(immediate.command->request);
    CHECK(std::holds_alternative<ImmediateSchedule>(create_request.schedule));
    CHECK(create_request.attributes.contains("retry.max_attempts"));

    auto recurring = with({"--cron", "0 9 * * 1-5", "--timezone", "Europe/Tallinn"});
    REQUIRE(recurring.command);
    auto const& cron = std::get<CronScheduleInput>(std::get<CreateJobRequest>(recurring.command->request).schedule);
    CHECK(cron.expression == "0 9 * * 1-5");
    CHECK(cron.timezone == "Europe/Tallinn");

    auto updated = parse({"--socket",
                          "fixture.sock",
                          "job",
                          "update",
                          job_id,
                          "--revision",
                          "1",
                          "--cron",
                          "0 9 * * *",
                          "--attribute",
                          "retry.max_attempts=3"});
    REQUIRE(updated.command);
    auto const& update_request = std::get<UpdateJobRequest>(updated.command->request);
    CHECK(update_request.expected_revision == 1);
    CHECK(update_request.attribute_changes.contains("retry.max_attempts"));
    CHECK(std::holds_alternative<CronScheduleInput>(*update_request.schedule));

    for (auto const& suffix : std::vector<std::vector<std::string>>{
             {"--now", "--at", "2030-01-01T00:00:00Z"},
             {"--now", "--cron", "0 9 * * *"},
             {"--at", "2030-01-01T00:00:00Z", "--timezone", "UTC"},
             {"--now", "--attribute", "retry.max_attempts=null"},
             {"--now", "--attribute", "retry.max_attempts=2", "--attribute", "retry.max_attempts=3"}
    }) {
        CAPTURE(suffix);
        CHECK_FALSE(with(suffix).command);
    }
    CHECK_FALSE(parse({"--socket", "fixture.sock", "job", "update", job_id, "--revision", "1", "--now"}).command);
}

TEST_CASE("jobuctl wait is local to suspend and compatible with request files", "[jobuctl][parse]")
{
    auto queue = parse({"--socket", "fixture.sock", "queue", "suspend", "--name", "reports", "--wait"});
    REQUIRE(queue.command);
    CHECK(queue.command->wait);
    CHECK(std::holds_alternative<QueueSelector>(queue.command->request));

    auto file = parse({"--socket", "fixture.sock", "job", "suspend", "--request-file", "job.json", "--wait"});
    REQUIRE(file.command);
    CHECK(file.command->wait);
    CHECK(file.command->request_file == std::filesystem::path{"job.json"});
    CHECK_FALSE(parse({"--socket", "fixture.sock", "queue", "resume", "--name", "reports", "--wait"}).command);
}

TEST_CASE("jobuctl run list maps flag selections to scalar or set filters", "[jobuctl][parse][run][list]")
{
    auto const terminal =
        std::vector{RunState::Succeeded, RunState::Failed, RunState::Interrupted, RunState::Cancelled};

    struct Case {
        std::vector<std::string>             options;
        std::optional<RunState>              state;
        std::optional<std::vector<RunState>> states;
    };

    auto const cases = std::vector<Case>{
        {.options = {}, .states = terminal},
        {.options = {"--job-id", job_id}, .states = terminal},
        {.options = {"--state", "failed"}, .state = RunState::Failed},
        {.options = {"--state", "running"}, .state = RunState::Running},
        {.options = {"--state", "failed", "--state", "interrupted"},
         .states  = std::vector{RunState::Failed, RunState::Interrupted}},
        {.options = {"--state=interrupted", "--state=failed"},
         .states  = std::vector{RunState::Interrupted, RunState::Failed}},
        {.options = {"--all"}},
        {.options = {"--job-id", job_id, "--all"}},
    };
    for (auto const& item : cases) {
        CAPTURE(item.options);
        for (auto const json : {false, true}) {
            auto arguments = std::vector<std::string>{"run", "list"};
            arguments.insert(arguments.end(), item.options.begin(), item.options.end());
            if (json) {
                arguments.emplace_back("--json");
            }
            auto parsed = parse(std::move(arguments));
            REQUIRE(parsed.command);
            CHECK(parsed.command->json == json);
            auto const& request = std::get<RunListRequest>(parsed.command->request);
            auto const& query   = std::get<RunQuery>(request);
            CHECK(query.filters.state == item.state);
            CHECK(query.filters.states == item.states);
            auto encoded = run_list_request_to_json(request);
            REQUIRE(encoded);
            CHECK(encoded->as_object().contains("state") == item.state.has_value());
            CHECK(encoded->as_object().contains("states") == item.states.has_value());
        }
    }

    // Selection policy must not displace the remaining AND filters or their time ranges.
    auto scoped = parse({"run",
                         "list",
                         "--queue-id",
                         job_id,
                         "--job-id",
                         job_id,
                         "--type",
                         "http",
                         "--origin",
                         "scheduled",
                         "--started-from",
                         "2030-01-01T00:00:00Z",
                         "--started-to",
                         "2030-02-01T00:00:00Z",
                         "--completed-from",
                         "2030-01-01T00:00:00Z",
                         "--completed-to",
                         "2030-02-01T00:00:00Z",
                         "--limit",
                         "2"});
    REQUIRE(scoped.command);
    auto const& query = std::get<RunQuery>(std::get<RunListRequest>(scoped.command->request));
    CHECK(query.filters.states == terminal);
    CHECK(query.filters.queue_id == *Uuid::parse(job_id));
    CHECK(query.filters.job_id == *Uuid::parse(job_id));
    CHECK(query.filters.type == JobType::Http);
    CHECK(query.filters.origin == RunOrigin::Scheduled);
    CHECK(query.filters.started.from);
    CHECK(query.filters.started.to);
    CHECK(query.filters.completed.from);
    CHECK(query.filters.completed.to);
    CHECK(query.limit == 2);
}

TEST_CASE("jobuctl run list rejects invalid selections and mixed cursor forms", "[jobuctl][parse][run][list]")
{
    for (auto const& options : std::vector<std::vector<std::string>>{
             {"--state", "failed", "--state", "failed"},
             {"--state", "failed", "--state", "interrupted", "--state", "failed"},
             {"--all", "--state", "failed"},
             {"--state", "failed", "--all"},
             {"--all", "--all"},
             {"--all=true"},
             {"--state", "FAILED"},
             {"--state", "unknown"},
             {"--state", ""},
             {"--state"},
             {"--cursor", "token", "--state", "failed"},
             {"--cursor", "token", "--state", "failed", "--state", "interrupted"},
             {"--cursor", "token", "--all"},
             {"--all", "--cursor", "token"},
             {"--cursor", "token", "--job-id", job_id},
             {"--cursor", "token", "--limit", "1"},
    }) {
        CAPTURE(options);
        auto arguments = std::vector<std::string>{"run", "list"};
        arguments.insert(arguments.end(), options.begin(), options.end());
        auto parsed = parse(std::move(arguments));
        CHECK_FALSE(parsed.command);
        CHECK_FALSE(parsed.error.empty());
    }
    auto comma = parse({"run", "list", "--state", "failed,interrupted"});
    CHECK_FALSE(comma.command);
    CHECK(comma.error.find("--state failed --state interrupted") != std::string::npos);
}

TEST_CASE("jobuctl run list request files preserve raw selections", "[jobuctl][input][run][list]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path   = directory.path() / "runs.json";
    auto                         parsed = parse({"run", "list", "--request-file", path.string()});
    REQUIRE(parsed.command);
    for (auto const* document :
         {R"({})", R"({"state":"running"})", R"({"states":["failed","interrupted"]})", R"({"cursor":"token"})"}) {
        CAPTURE(document);
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << document;
        file.close();
        REQUIRE(load_request_file(*parsed.command, registry));
        auto encoded = run_list_request_to_json(std::get<RunListRequest>(parsed.command->request));
        REQUIRE(encoded);
        auto expected = parse_json(document);
        REQUIRE(expected);
        if (!expected->as_object().contains("cursor")) {
            std::get<JsonValue::Object>(expected->data).emplace("limit", JsonValue{.data = std::uint64_t{100}});
        }
        CHECK(*encoded == *expected);
    }
    for (auto const& options : std::vector<std::vector<std::string>>{
             {"--all"},
             {"--state", "failed"},
             {"--state", "failed", "--state", "interrupted"},
             {"--cursor", "token"},
             {"--limit", "2"}
    }) {
        auto arguments = std::vector<std::string>{"run", "list", "--request-file", path.string()};
        arguments.insert(arguments.end(), options.begin(), options.end());
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
}

TEST_CASE("jobuctl run commands preserve history filters and cursor-only continuations", "[jobuctl][parse]")
{
    auto now = parse({"--socket", "fixture.sock", "job", "run-now", job_id, "--idempotency-key", "manual-1"});
    REQUIRE(now.command);
    CHECK(now.command->method == "job.run_now");
    CHECK(std::get<RunNowRequest>(now.command->request).idempotency_key == "manual-1");

    auto listed = parse({"--socket",
                         "fixture.sock",
                         "run",
                         "list",
                         "--queue-id",
                         job_id,
                         "--state",
                         "failed",
                         "--origin",
                         "manual",
                         "--planned-from",
                         "2030-01-01T00:00:00Z",
                         "--planned-to",
                         "2030-02-01T00:00:00Z",
                         "--limit",
                         "20"});
    REQUIRE(listed.command);
    auto const& query = std::get<RunQuery>(std::get<RunListRequest>(listed.command->request));
    CHECK(query.limit == 20);
    CHECK(query.filters.queue_id == *Uuid::parse(job_id));
    CHECK(query.filters.state == RunState::Failed);
    CHECK(query.filters.origin == RunOrigin::Manual);
    CHECK(query.filters.planned.from.has_value());
    CHECK(query.filters.planned.to.has_value());

    auto cursor = parse({"--socket", "fixture.sock", "run", "list", "--cursor", "token"});
    REQUIRE(cursor.command);
    CHECK(std::get<CursorRequest>(std::get<RunListRequest>(cursor.command->request)).cursor == "token");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "run", "list", "--cursor", "token", "--limit", "2"}).command);
    CHECK_FALSE(parse({"--socket", "fixture.sock", "run", "list", "--origin", "submitted"}).command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "run",
                       "list",
                       "--planned-from",
                       "2031-01-01T00:00:00Z",
                       "--planned-to",
                       "2030-01-01T00:00:00Z"})
                    .command);

    auto cancel = parse({"--socket", "fixture.sock", "run", "cancel", job_id, "--wait"});
    REQUIRE(cancel.command);
    CHECK(cancel.command->wait);
    CHECK(cancel.command->method == "run.cancel");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "run", "get", job_id, "--wait"}).command);
}

TEST_CASE("jobuctl run output owns a CLI-only request and shares output limits", "[jobuctl][parse]")
{
    for (auto const& [channel, expected] : std::array{
             std::pair{"stdout",  OutputChannel::Stdout },
             std::pair{"stderr",  OutputChannel::Stderr },
             std::pair{"body",    OutputChannel::Body   },
             std::pair{"headers", OutputChannel::Headers}
    }) {
        auto output = parse({"run", "output", job_id, "--channel", channel});
        REQUIRE(output.command);
        CHECK(output.command->kind == CommandKind::RunOutput);
        CHECK(output.command->method.empty());
        auto const& request = std::get<RunOutputRequest>(output.command->request);
        CHECK(request.run_id.to_string() == job_id);
        CHECK(request.channel == expected);
        CHECK(request.offset == 0);
        CHECK(request.limit == 16384);
    }

    auto output = parse({"--socket",
                         "output.sock",
                         "run",
                         "output",
                         job_id,
                         "--channel",
                         "stderr",
                         "--offset",
                         "9223372036854775806",
                         "--limit",
                         "65536",
                         "--timeout",
                         "1234",
                         "--raw"});
    REQUIRE(output.command);
    CHECK(output.command->socket_path == "output.sock");
    CHECK(output.command->timeout == std::chrono::milliseconds{1234});
    CHECK(output.command->raw);
    auto const& request = std::get<RunOutputRequest>(output.command->request);
    CHECK(request.offset == 9223372036854775806ULL);
    CHECK(request.limit == 65536);
    REQUIRE(parse({"run", "output", job_id, "--channel", "body", "--offset", "0", "--limit", "1"}).command);

    for (auto const* offset : {"9223372036854775807", "18446744073709551615"}) {
        auto invalid = parse({"run", "output", job_id, "--channel", "stderr", "--offset", offset});
        CHECK_FALSE(invalid.command);
        CHECK(invalid.error == "run output --offset must not exceed INT64_MAX - 1");
    }
    for (auto const& [option, value] : std::array{
             std::pair{"--offset", "-1"                  },
             std::pair{"--offset", "18446744073709551616"},
             std::pair{"--offset", "1.5"                 },
             std::pair{"--offset", "+1"                  },
             std::pair{"--offset", "1x"                  },
             std::pair{"--limit",  "0"                   },
             std::pair{"--limit",  "65537"               },
             std::pair{"--limit",  "-1"                  },
             std::pair{"--limit",  "18446744073709551616"},
             std::pair{"--limit",  "1.5"                 }
    }) {
        auto invalid = parse({"run", "output", job_id, "--channel", "stdout", std::string{option} + "=" + value});
        CHECK_FALSE(invalid.command);
        CHECK(invalid.error == (std::string_view{option} == "--offset" ? "--offset must be a nonnegative integer"
                                                                       : "--limit must be from 1 through 65536"));
    }
    for (auto const* option : {"--offset", "--limit"}) {
        auto invalid = parse({"run", "output", job_id, "--channel", "stdout", option, "-1"});
        CHECK_FALSE(invalid.command);
        CHECK(invalid.error == std::string{option} + " requires a value");
    }

    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"run", "output"},
             {"run", "output", "invalid", "--channel", "stdout"},
             {"run", "output", "00112233-4455-6677-8899-AABBCCDDEEFF", "--channel", "stdout"},
             {"run", "output", job_id},
             {"run", "output", job_id, "--channel", ""},
             {"run", "output", job_id, "--channel", "STDOUT"},
             {"run", "output", job_id, "--channel", "unknown"},
             {"run", "output", job_id, "1", "--channel", "stdout"},
             {"run", "output", job_id, "--channel", "stdout", "--channel", "stderr"},
             {"run", "output", job_id, "--channel", "stdout", "--limit", "1", "--limit", "2"},
             {"run", "output", job_id, "--channel", "stdout", "--offset", ""},
             {"run", "output", job_id, "--channel", "stdout", "--offset", "1", "--offset", "2"},
             {"run", "output", job_id, "--channel", "stdout", "--wait"}
    }) {
        CAPTURE(arguments);
        CHECK_FALSE(parse(arguments).command);
    }
}

TEST_CASE("jobuctl run output rejects request files and conflicting delivery modes", "[jobuctl][parse]")
{
    for (auto const& arguments : std::vector<std::vector<std::string>>{
             {"run", "output", "--request-file", "-"},
             {"--request-file=missing.json", "run", "output"},
             {"run", "--request-file", "missing.json", "output", job_id, "--channel", "stdout"},
             {"run", "output", job_id, "--request-file", "missing.json", "--output-file", "out.bin"}
    }) {
        auto invalid = parse(arguments);
        CHECK_FALSE(invalid.command);
        CHECK(invalid.error.find("run output does not support --request-file") != std::string::npos);
        CHECK(invalid.error.find("attempt output --request-file FILE") != std::string::npos);
    }
    for (auto const& modes : std::vector<std::vector<std::string>>{
             {"--raw", "--json"},
             {"--raw", "--output-file", "out.bin"},
             {"--json", "--output-file", "out.bin"}
    }) {
        auto arguments = std::vector<std::string>{"run", "output", job_id, "--channel", "stdout"};
        arguments.insert(arguments.end(), modes.begin(), modes.end());
        auto invalid = parse(std::move(arguments));
        CHECK_FALSE(invalid.command);
        CHECK(invalid.error == "--raw, --output-file, and --json output modes are mutually exclusive");
    }
    auto file = parse({"run", "output", job_id, "--channel", "body", "--output-file", "out.bin"});
    REQUIRE(file.command);
    CHECK(file.command->output_file == "out.bin");
    CHECK_FALSE(file.command->request_file);
    CHECK_FALSE(parse({"run", "output", job_id, "--channel", "body", "--output-file", ""}).command);

    auto const* spec = find_command("run", "output");
    REQUIRE(spec);
    CHECK_FALSE(spec->supports_request_file);
    CHECK(spec->capability.empty());
    for (auto const& command : command_specs()) {
        if (command.kind != CommandKind::RunOutput) {
            CHECK(command.supports_request_file);
            CHECK_FALSE(command.capability.empty());
        }
    }
    StandardAttributeRegistry registry;
    CHECK_FALSE(decode_command_request(CommandKind::RunOutput, JsonValue{.data = JsonValue::Object{}}, registry));
}

TEST_CASE("jobuctl attempt commands keep output delivery separate from request params", "[jobuctl][parse]")
{
    auto listed = parse({"--socket", "fixture.sock", "attempt", "list", job_id, "--limit", "3"});
    REQUIRE(listed.command);
    auto const& query = std::get<AttemptQuery>(std::get<AttemptListRequest>(listed.command->request));
    CHECK(query.run_id == *Uuid::parse(job_id));
    CHECK(query.limit == 3);

    auto cursor = parse({"--socket", "fixture.sock", "attempt", "list", "--cursor", "next"});
    REQUIRE(cursor.command);
    CHECK(std::get<CursorRequest>(std::get<AttemptListRequest>(cursor.command->request)).cursor == "next");
    CHECK_FALSE(parse({"--socket", "fixture.sock", "attempt", "list", job_id, "--cursor", "next"}).command);

    auto output = parse({"--socket",
                         "fixture.sock",
                         "attempt",
                         "output",
                         job_id,
                         "2",
                         "--channel",
                         "stderr",
                         "--offset",
                         "4",
                         "--limit",
                         "16",
                         "--raw"});
    REQUIRE(output.command);
    CHECK(output.command->raw);
    auto const& request = std::get<AttemptOutputRequest>(output.command->request);
    CHECK(request.attempt.attempt_number == 2);
    CHECK(request.channel == OutputChannel::Stderr);
    CHECK(request.offset == 4);
    CHECK(request.limit == 16);

    CHECK_FALSE(parse({"--socket", "fixture.sock", "attempt", "output", job_id, "0", "--channel", "stdout"}).command);
    CHECK_FALSE(
        parse({"--socket", "fixture.sock", "attempt", "output", job_id, "1", "--channel", "stdout", "--raw", "--json"})
            .command);
    CHECK_FALSE(parse({"--socket",
                       "fixture.sock",
                       "attempt",
                       "output",
                       job_id,
                       "1",
                       "--channel",
                       "stdout",
                       "--raw",
                       "--output-file",
                       "out.bin"})
                    .command);

    auto file = parse(
        {"--socket", "fixture.sock", "attempt", "output", "--request-file", "params.json", "--output-file", "out.bin"});
    REQUIRE(file.command);
    CHECK(file.command->output_file == std::filesystem::path{"out.bin"});
}

TEST_CASE("jobuctl shared output parsing preserves channel defaults and numeric boundaries", "[jobuctl][parse]")
{
    for (auto const* channel : {"stdout", "stderr", "body", "headers"}) {
        auto output = parse({"attempt", "output", job_id, "1", "--channel", channel});
        REQUIRE(output.command);
        auto const& request = std::get<AttemptOutputRequest>(output.command->request);
        CHECK(request.offset == 0);
        CHECK(request.limit == 16384);
        auto encoded = attempt_output_request_to_json(request);
        REQUIRE(encoded);
        CHECK(encoded->as_object().at("channel").as_string() == channel);
    }

    for (auto const& [offset, limit] : std::array{
             std::pair{"0",                   "1"    },
             std::pair{"9223372036854775806", "65536"}
    }) {
        auto output =
            parse({"attempt", "output", job_id, "2", "--channel", "stderr", "--offset", offset, "--limit", limit});
        REQUIRE(output.command);
        auto const& request = std::get<AttemptOutputRequest>(output.command->request);
        CHECK(std::to_string(request.offset) == offset);
        CHECK(std::to_string(request.limit) == limit);
    }

    // The lexical parser accepts uint64 values, but the public codec preserves its stricter storage offset bound.
    for (auto const* offset : {"9223372036854775807", "18446744073709551615"}) {
        auto output = parse({"attempt", "output", job_id, "2", "--channel", "stderr", "--offset", offset});
        CHECK_FALSE(output.command);
        CHECK(output.error == "attempt output identity or channel is invalid");
    }

    for (auto const& option : std::array{
             std::pair{"--offset", "-1"                  },
             std::pair{"--offset", "18446744073709551616"},
             std::pair{"--offset", "1.5"                 },
             std::pair{"--offset", "+1"                  },
             std::pair{"--offset", "1x"                  },
             std::pair{"--limit",  "0"                   },
             std::pair{"--limit",  "65537"               },
             std::pair{"--limit",  "-1"                  },
             std::pair{"--limit",  "18446744073709551616"},
             std::pair{"--limit",  "1.5"                 }
    }) {
        // Attached values reach domain validation even when they start with a dash.
        auto output = parse(
            {"attempt", "output", job_id, "2", "--channel", "stderr", std::string{option.first} + "=" + option.second});
        CHECK_FALSE(output.command);
        CHECK(output.error == (std::string_view{option.first} == "--offset" ? "--offset must be a nonnegative integer"
                                                                            : "--limit must be from 1 through 65536"));
    }

    for (auto const* option : {"--offset", "--limit"}) {
        auto output = parse({"attempt", "output", job_id, "1", "--channel", "stdout", option, "-1"});
        CHECK_FALSE(output.command);
        CHECK(output.error == std::string{option} + " requires a value");
    }

    for (auto const* channel : {"", "STDOUT", "unknown"}) {
        CHECK_FALSE(parse({"attempt", "output", job_id, "1", "--channel", channel}).command);
    }
    CHECK_FALSE(parse({"attempt", "output", job_id, "1"}).command);
    CHECK_FALSE(parse({"attempt", "output", job_id, "1", "--channel", "stdout", "--offset", ""}).command);
    CHECK_FALSE(parse({"attempt", "output", job_id, "1", "--channel", "stdout", "--channel", "stderr"}).command);
    CHECK_FALSE(
        parse({"attempt", "output", job_id, "1", "--channel", "stdout", "--limit", "1", "--limit", "2"}).command);
}

TEST_CASE("jobuctl request files decode run and output methods with strict public codecs", "[jobuctl][input]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path = directory.path() / "history.json";
    {
        auto file = std::ofstream{path, std::ios::binary};
        REQUIRE(file);
        file << R"({"job_id":"00000000-0000-7000-8000-000000000001","limit":2})";
    }
    auto runs = parse({"--socket", "fixture.sock", "run", "list", "--request-file", path.string()});
    REQUIRE(runs.command);
    REQUIRE(load_request_file(*runs.command, registry));
    auto const& query = std::get<RunQuery>(std::get<RunListRequest>(runs.command->request));
    CHECK(query.limit == 2);
    CHECK(query.filters.job_id == *Uuid::parse(job_id));

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file
            << R"({"run_id":"00000000-0000-7000-8000-000000000001","attempt_number":1,"channel":"body","offset":4,"limit":8})";
    }
    auto output = parse({"--socket", "fixture.sock", "attempt", "output", "--request-file", path.string(), "--raw"});
    REQUIRE(output.command);
    REQUIRE(load_request_file(*output.command, registry));
    CHECK(output.command->raw);
    auto const& request = std::get<AttemptOutputRequest>(output.command->request);
    CHECK(request.channel == OutputChannel::Body);
    CHECK(request.offset == 4);
    CHECK(request.limit == 8);

    {
        auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(file);
        file << R"({"cursor":"token","limit":2})";
    }
    auto invalid = load_request_file(*runs.command, registry);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobuctl.input.invalid_params");
}

TEST_CASE("CLI cron flags and request files preserve timezone omission", "[jobuctl][parse][input][timezone]")
{
    jb::test::TemporaryDirectory directory;
    StandardAttributeRegistry    registry;
    auto const                   path   = directory.path() / "cron.json";
    auto const                   job_id = std::string{"00112233-4455-6677-8899-aabbccddeeff"};

    struct Case {
        std::vector<std::string> flags;
        std::vector<std::string> file_command;
        std::string              params;
    };

    auto const cases = std::vector<Case>{
        {.flags = {"job", "create", "--queue-name", "queue", "--type", "cli", "--cron", "@daily", "--command", "/true"},
         .file_command = {"job", "create"},
         .params       = R"({"queue_name":"queue","payload":{"command":"/true"},"schedule":SCHEDULE})"             },
        {.flags        = {"job", "update", job_id, "--revision", "1", "--cron", "@daily"},
         .file_command = {"job", "update"},
         .params = R"({"job_id":"00112233-4455-6677-8899-aabbccddeeff","expected_revision":1,"schedule":SCHEDULE})"},
        {.flags        = {"schedule", "validate", "@daily"},
         .file_command = {"schedule", "validate"},
         .params       = R"({"schedule":SCHEDULE})"                                                                },
        {.flags        = {"schedule", "next", "@daily", "--after", "2030-01-01T00:00:00Z"},
         .file_command = {"schedule", "next"},
         .params       = R"({"after":"2030-01-01T00:00:00Z","schedule":SCHEDULE})"                                 },
    };
    auto timezone_of = [](Command const& command) -> std::optional<std::string> {
        switch (command.kind) {
            case CommandKind::JobCreate:
                return std::get<CronScheduleInput>(std::get<CreateJobRequest>(command.request).schedule).timezone;
            case CommandKind::JobUpdate:
                return std::get<CronScheduleInput>(*std::get<UpdateJobRequest>(command.request).schedule).timezone;
            case CommandKind::ScheduleValidate:
                return std::get<CronScheduleInput>(command.request).timezone;
            case CommandKind::ScheduleNext:
                return std::get<ScheduleNextRequest>(command.request).schedule.timezone;
            default:
                FAIL("Unexpected command");
                return {};
        }
    };
    for (auto const& item : cases) {
        for (auto explicit_utc : {false, true}) {
            auto flags = std::vector<std::string>{"--socket", "fixture.sock"};
            flags.insert(flags.end(), item.flags.begin(), item.flags.end());
            if (explicit_utc) {
                flags.insert(flags.end(), {"--timezone", "UTC"});
            }
            auto parsed = parse(flags);
            REQUIRE(parsed.command);
            auto expected = explicit_utc ? std::optional<std::string>{"UTC"} : std::nullopt;
            CHECK(timezone_of(*parsed.command) == expected);

            auto        document = item.params;
            auto const* schedule = explicit_utc ? R"({"kind":"cron","expression":"@daily","timezone":"UTC"})"
                                                : R"({"kind":"cron","expression":"@daily"})";
            document.replace(document.find("SCHEDULE"), std::string{"SCHEDULE"}.size(), schedule);
            auto file = std::ofstream{path};
            file << document;
            file.close();
            REQUIRE(file);
            auto arguments = std::vector<std::string>{"--socket", "fixture.sock"};
            arguments.insert(arguments.end(), item.file_command.begin(), item.file_command.end());
            arguments.insert(arguments.end(), {"--request-file", path.string()});
            auto from_file = parse(arguments);
            REQUIRE(from_file.command);
            REQUIRE(load_request_file(*from_file.command, registry));
            CHECK(timezone_of(*from_file.command) == expected);
        }
    }
    auto rendered = render_help({.group = "schedule", .action = "validate"});
    CHECK(rendered.find("daemon default") != std::string::npos);
}

TEST_CASE("Explicit queue updates require a bounded revision with positional selectors",
          "[jobuctl][parse][queue][revision]")
{
    auto fields = std::vector<std::string>{"queue", "update", "--name", "protected", "--weight", "2"};
    CHECK(parse(fields).error == "queue update requires --revision with the inspected queue revision");
    for (auto const* revision : {"1", "9007199254740993", "9223372036854775807"}) {
        auto arguments = fields;
        arguments.insert(arguments.end(), {"--revision", revision});
        auto parsed = parse(std::move(arguments));
        REQUIRE(parsed.command);
        auto const& request = std::get<UpdateQueueRequest>(parsed.command->request);
        CHECK(request.expected_revision == std::stoull(revision));
        CHECK(std::get<std::string>(request.queue) == "protected");
    }
    for (auto const* revision : {"0", "-1", "1.0", "true", "9223372036854775808", "18446744073709551615"}) {
        auto arguments = fields;
        arguments.insert(arguments.end(), {"--revision", revision});
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
    CHECK_FALSE(parse({"queue", "update", "--name", "protected", "--revision", "1"}).command);
    CHECK_FALSE(parse({"queue", "update", "--name", "protected", "--revision", "1", "--revision", "1", "--weight", "2"})
                    .command);
    CHECK(parse({"queue", "update", "protected", "--revision", "1", "--weight", "2"}).command);
    CHECK_FALSE(parse({"queue", "update", "--request-file", "input.json", "--revision", "1"}).command);
    auto by_id = parse({"queue", "update", "--id", job_id, "--revision", "7", "--weight", "2"});
    REQUIRE(by_id.command);
    CHECK(std::get<UpdateQueueRequest>(by_id.command->request).expected_revision == 7);
}

TEST_CASE("Queue commands share owning positional and explicit selector intent", "[jobuctl][parse][queue][selector]")
{
    auto const lower = std::string{"abcdef01-abcd-7000-abcd-0123456789ab"};
    auto const upper = std::string{"ABCDEF01-ABCD-7000-ABCD-0123456789AB"};

    struct SelectorCase {
        std::vector<std::string> arguments;
        QueueSelectionForm       form;
        std::string              value;
    };

    auto const cases = std::vector<SelectorCase>{
        {.arguments = {"reports"},                              .form = QueueSelectionForm::ExactName,  .value = "reports"      },
        {.arguments = {lower},                                  .form = QueueSelectionForm::InferredId, .value = lower          },
        {.arguments = {upper},                                  .form = QueueSelectionForm::InferredId, .value = lower          },
        {.arguments = {"id:" + lower},                          .form = QueueSelectionForm::ExplicitId, .value = lower          },
        {.arguments = {"id:" + upper},                          .form = QueueSelectionForm::ExplicitId, .value = lower          },
        {.arguments = {"name:" + upper},                        .form = QueueSelectionForm::ExactName,  .value = upper          },
        {.arguments = {"name:id:reports"},                      .form = QueueSelectionForm::ExactName,  .value = "id:reports"   },
        {.arguments = {"name:name:reports"},                    .form = QueueSelectionForm::ExactName,  .value = "name:reports" },
        {.arguments = {"team:reports"},                         .form = QueueSelectionForm::ExactName,  .value = "team:reports" },
        {.arguments = {"ID:reports"},                           .form = QueueSelectionForm::ExactName,  .value = "ID:reports"   },
        {.arguments = {"Name:reports"},                         .form = QueueSelectionForm::ExactName,  .value = "Name:reports" },
        {.arguments = {"night reports"},                        .form = QueueSelectionForm::ExactName,  .value = "night reports"},
        {.arguments = {" reports "},                            .form = QueueSelectionForm::ExactName,  .value = " reports "    },
        {.arguments = {"name:-reports"},                        .form = QueueSelectionForm::ExactName,  .value = "-reports"     },
        {.arguments = {"name:--help"},                          .form = QueueSelectionForm::ExactName,  .value = "--help"       },
        {.arguments = {"--name", upper},                        .form = QueueSelectionForm::ExactName,  .value = upper          },
        {.arguments = {"--name", "id:reports"},                 .form = QueueSelectionForm::ExactName,  .value = "id:reports"   },
        {.arguments = {"--name", "name:reports"},               .form = QueueSelectionForm::ExactName,  .value = "name:reports" },
        {.arguments = {"--name=--help"},                        .form = QueueSelectionForm::ExactName,  .value = "--help"       },
        {.arguments = {"--id", upper},                          .form = QueueSelectionForm::ExplicitId, .value = lower          },
        {.arguments = {"abcdef01-abcd-7000-abcd-0123456789az"},
         .form      = QueueSelectionForm::ExactName,
         .value     = "abcdef01-abcd-7000-abcd-0123456789az"                                                                    },
    };

    for (auto const* action : {"get", "update", "suspend", "resume", "delete", "stats"}) {
        for (auto const& scenario : cases) {
            CAPTURE(action, scenario.arguments);
            auto arguments = std::vector<std::string>{"queue", action};
            arguments.insert(arguments.end(), scenario.arguments.begin(), scenario.arguments.end());
            if (std::string_view{action} == "update") {
                arguments.insert(arguments.end(), {"--revision", "6", "--new-name", "name:replacement"});
            }
            auto parsed = parse(std::move(arguments));
            REQUIRE(parsed.command);
            REQUIRE(parsed.command->queue_selector_intent);
            CHECK(parsed.command->queue_selector_intent->form == scenario.form);
            // parse() has already destroyed argv and lexer storage, including the original uppercase spelling.
            auto const original = scenario.arguments.front() == "--name=--help" ? "--help" : scenario.arguments.back();
            CHECK(parsed.command->queue_selector_intent->original_text == original);

            auto const* selector = std::get_if<QueueSelector>(&parsed.command->request);
            if (auto const* get = std::get_if<GetQueueRequest>(&parsed.command->request)) {
                selector = &get->queue;
                CHECK_FALSE(get->include_deleted);
            }
            else if (auto const* update = std::get_if<UpdateQueueRequest>(&parsed.command->request)) {
                selector = &update->queue;
                CHECK(update->name == "name:replacement");
                CHECK(update->expected_revision == 6);
            }
            else if (auto const* stats = std::get_if<QueueStatisticsListRequest>(&parsed.command->request)) {
                selector = &std::get<QueueStatisticsQuery>(*stats).selector;
            }
            REQUIRE(selector);
            if (scenario.form == QueueSelectionForm::ExactName) {
                CHECK(std::get<std::string>(*selector) == scenario.value);
            }
            else {
                CHECK(std::get<Uuid>(*selector).to_string() == scenario.value);
            }
        }

        // A terminator is still lexical, and must preserve help-looking operands as data.
        auto arguments = std::vector<std::string>{"queue", action};
        if (std::string_view{action} == "update") {
            arguments.insert(arguments.end(), {"--revision", "6", "--weight", "2"});
        }
        arguments.insert(arguments.end(), {"--", "--help"});
        auto literal = parse(std::move(arguments));
        REQUIRE(literal.command);
        REQUIRE(literal.command->queue_selector_intent);
        CHECK(literal.command->queue_selector_intent->form == QueueSelectionForm::ExactName);
        CHECK(literal.command->queue_selector_intent->original_text == "--help");
    }
}

TEST_CASE("Queue selectors reject empty malformed and conflicting forms", "[jobuctl][parse][queue][selector]")
{
    for (auto const* action : {"get", "update", "suspend", "resume", "delete", "stats"}) {
        for (auto const& selection : std::vector<std::vector<std::string>>{
                 {},
                 {""},
                 {"name:"},
                 {"id:"},
                 {"id:not-a-uuid"},
                 {"id:id:" + std::string{job_id}},
                 {"id:name:reports"},
                 {"--id", "id:" + std::string{job_id}},
                 {"--id", "not-a-uuid"},
                 {"--name", ""},
                 {"--id", ""},
                 {"reports", "reports"},
                 {job_id, "--id", job_id},
                 {"--id", job_id, job_id},
                 {"reports", "--name", "reports"},
                 {"--name", "reports", "reports"},
                 {"--id", job_id, "--name", job_id},
                 {"--name", "reports", "--name", "reports"}
        }) {
            CAPTURE(action, selection);
            auto arguments = std::vector<std::string>{"queue", action};
            arguments.insert(arguments.end(), selection.begin(), selection.end());
            if (std::string_view{action} == "update") {
                arguments.insert(arguments.end(), {"--revision", "6", "--weight", "2"});
            }
            CHECK_FALSE(parse(std::move(arguments)).command);
        }
    }
    for (auto const& selection : std::vector<std::vector<std::string>>{
             {"reports"},
             {"id:" + std::string{job_id}},
             {"--id", job_id},
             {"--name", "reports"}
    }) {
        auto arguments = std::vector<std::string>{"queue", "update"};
        arguments.insert(arguments.end(), selection.begin(), selection.end());
        auto interactive = parse(arguments);
        REQUIRE(interactive.command);
        CHECK(std::holds_alternative<InteractiveQueueUpdate>(interactive.command->request));
        arguments.insert(arguments.end(), {"--revision", "6"});
        CHECK(parse(std::move(arguments)).error == "queue update requires at least one mutable field");
    }
    CHECK_FALSE(parse({"queue", "list", "reports"}).command);
    auto created = parse({"queue", "create", "name:id:reports"});
    REQUIRE(created.command);
    CHECK(std::get<CreateQueueRequest>(created.command->request).name == "name:id:reports");
    CHECK_FALSE(created.command->queue_selector_intent);
}

TEST_CASE("Queue statistics keep cursor continuation free of selector intent", "[jobuctl][parse][queue][selector]")
{
    auto continuation = parse({"queue", "stats", "--cursor", "token"});
    REQUIRE(continuation.command);
    CHECK_FALSE(continuation.command->queue_selector_intent);
    for (auto const& selection : std::vector<std::vector<std::string>>{
             {"reports"},
             {"id:" + std::string{job_id}},
             {"--id", job_id},
             {"--name", "reports"},
             {"--limit", "2"},
             {"--group-by", "job"},
             {"--job-id", job_id},
             {"--type", "cli"},
             {"--origin", "manual"},
             {"--planned-from", "2030-01-01T00:00:00Z"},
             {"--planned-to", "2030-01-02T00:00:00Z"}
    }) {
        auto arguments = std::vector<std::string>{"queue", "stats", "--cursor", "token"};
        arguments.insert(arguments.end(), selection.begin(), selection.end());
        CHECK_FALSE(parse(std::move(arguments)).command);
    }
    auto filtered = parse({"queue", "stats", "reports", "--group-by", "state", "--limit", "2", "--job-id", job_id});
    REQUIRE(filtered.command);
    auto const& query = std::get<QueueStatisticsQuery>(std::get<QueueStatisticsListRequest>(filtered.command->request));
    CHECK(std::get<std::string>(query.selector) == "reports");
    CHECK(query.statistics.group_by == StatisticsGroupBy::State);
    CHECK(query.statistics.limit == 2);
    CHECK(query.statistics.job_id == Uuid::parse(job_id).value());
}

TEST_CASE("Queue get request files alone can opt into deleted visibility", "[jobuctl][input][queue][get]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "historical-get.json";
    StandardAttributeRegistry    registry;
    CHECK_FALSE(parse({"queue", "get", "archived", "--include-deleted"}).command);

    for (auto const include_deleted : {false, true}) {
        {
            std::ofstream file{path};
            file << R"({"queue_name":"name:archived","include_deleted":)" << (include_deleted ? "true" : "false")
                 << '}';
        }
        auto raw = parse({"queue", "get", "--request-file", path.string()});
        REQUIRE(raw.command);
        REQUIRE(load_request_file(*raw.command, registry));
        CHECK_FALSE(raw.command->queue_selector_intent);
        auto const& request = std::get<GetQueueRequest>(raw.command->request);
        CHECK(std::get<std::string>(request.queue) == "name:archived");
        CHECK(request.include_deleted == include_deleted);

        for (auto const* action : {"suspend", "resume", "delete"}) {
            auto mutation = parse({"queue", action, "--request-file", path.string()});
            REQUIRE(mutation.command);
            CHECK_FALSE(load_request_file(*mutation.command, registry));
        }
    }
    for (auto const* invalid : {R"({"queue_name":"archived","include_deleted":null})",
                                R"({"queue_name":"archived","include_deleted":1})",
                                R"({"queue_name":"archived","include_deleted":true,"unknown":true})"}) {
        auto value = parse_json(invalid);
        REQUIRE(value);
        CHECK_FALSE(decode_command_request(CommandKind::QueueGet, *value, registry));
    }
}

TEST_CASE("Queue request files bypass CLI selector interpretation and intent", "[jobuctl][input][queue][selector]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   path = directory.path() / "queue-request.json";
    StandardAttributeRegistry    registry;
    for (auto const* action : {"get", "update", "suspend", "resume", "delete", "stats"}) {
        for (auto const* name : {"id:reports", "name:reports", "ABCDEF01-ABCD-7000-ABCD-0123456789AB"}) {
            CAPTURE(action, name);
            {
                std::ofstream file{path};
                file << R"({"queue_name":")" << name << '"';
                if (std::string_view{action} == "update") {
                    file << R"(,"expected_revision":6,"weight":2)";
                }
                file << "}";
            }
            auto raw = parse({"queue", action, "--request-file", path.string()});
            REQUIRE(raw.command);
            CHECK_FALSE(raw.command->queue_selector_intent);
            REQUIRE(load_request_file(*raw.command, registry));
            CHECK_FALSE(raw.command->queue_selector_intent);
            auto const* selector = std::get_if<QueueSelector>(&raw.command->request);
            if (auto const* get = std::get_if<GetQueueRequest>(&raw.command->request)) {
                selector = &get->queue;
                CHECK_FALSE(get->include_deleted);
            }
            else if (auto const* update = std::get_if<UpdateQueueRequest>(&raw.command->request)) {
                selector = &update->queue;
            }
            else if (auto const* stats = std::get_if<QueueStatisticsListRequest>(&raw.command->request)) {
                selector = &std::get<QueueStatisticsQuery>(*stats).selector;
            }
            REQUIRE(selector);
            CHECK(std::get<std::string>(*selector) == name);
        }
        for (auto const& extra : std::vector<std::vector<std::string>>{
                 {"reports"},
                 {"--id", job_id},
                 {"--name", "reports"}
        }) {
            auto arguments = std::vector<std::string>{"queue", action, "--request-file", path.string()};
            arguments.insert(arguments.end(), extra.begin(), extra.end());
            CHECK_FALSE(parse(std::move(arguments)).command);
        }

        for (auto const* invalid : {R"({"queue_id":"id:00000000-0000-7000-8000-000000000001"})",
                                    R"({/* comment */"queue_name":"reports"})",
                                    R"({"queue_name":"reports","selector":"reports"})"}) {
            {
                std::ofstream file{path};
                auto          document = std::string{invalid};
                if (std::string_view{action} == "update") {
                    document.insert(document.size() - 1, R"(,"expected_revision":6,"weight":2)");
                }
                file << document;
            }
            auto raw = parse({"queue", action, "--request-file", path.string()});
            REQUIRE(raw.command);
            CHECK_FALSE(load_request_file(*raw.command, registry));
        }
    }
}
