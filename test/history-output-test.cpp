#include "history_service.hpp"

#include "support/fake_time_source.hpp"
#include "support/fault_database_driver.hpp"
#include "support/recovery_fixture.hpp"
#include "support/sequence_uuid_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::test;

namespace {

auto bytes(std::string_view text) -> ByteBuffer
{
    auto view = as_bytes(text);
    return {view.begin(), view.end()};
}

auto json(auto value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto capture_result(std::string_view type,
                    std::string_view channel,
                    std::uint64_t    captured,
                    std::uint64_t    total,
                    bool             truncated,
                    bool             lost = false) -> JsonValue
{
    return json(JsonValue::Object{
        {"type",               json(std::string{type})},
        {"capture_lost",       json(lost)             },
        {std::string{channel},
         json(JsonValue::Object{
             {"captured_bytes", json(captured)},
             {"total_bytes", json(total)},
             {"truncated", json(truncated)},
         })                                           },
    });
}

struct Fixture {
    RecoveryFixture       storage;
    FakeTimeSource        time;
    SequenceUuidGenerator tokens{{}};
    HistoryService        history{storage.database, storage.registry, tokens, time};
    std::vector<Error>    failures;

    Fixture()
    {
        history.failed.connect(&history, [this](Error const& error) { failures.push_back(error); });
    }

    auto insert(JobType                                        type,
                RunState                                       state,
                std::optional<jb::jobu::detail::AttemptOutput> output = std::nullopt,
                std::optional<JsonValue>                       result = std::nullopt) -> AttemptKey
    {
        auto queue = recovery_queue(recovery_id(1));
        storage.insert_queue(queue);
        auto job = storage.make_job(recovery_id(2), queue.id, type);
        storage.insert_job(job);
        auto run = storage.make_run(recovery_id(3), job, state);
        REQUIRE_FALSE(run.attempts.empty());
        if (result) {
            run.attempts.back().attempt.result = std::move(result);
        }
        run.attempts.back().output = std::move(output);
        auto key = AttemptKey{.run_id = run.run.id, .attempt_number = run.attempts.back().attempt.attempt_number};
        storage.insert_run(run);
        return key;
    }
};

auto request(AttemptKey key, OutputChannel channel, std::uint64_t offset = 0, std::size_t limit = 16'384)
    -> AttemptOutputRequest
{
    return {.attempt = key, .channel = channel, .offset = offset, .limit = limit};
}

} // namespace

TEST_CASE("Output reads one bounded slice from a large truncated BLOB", "[jobu][history][output][sqlite]")
{
    auto selected    = std::vector<std::string>{};
    auto faults      = std::make_shared<DatabaseFaultState>();
    faults->classify = [&selected](std::string_view sql) -> std::string {
        if (sql.find("jobu_attempt_output") != std::string_view::npos && sql.starts_with("SELECT")) {
            selected.emplace_back(sql);
        }
        return "other";
    };
    RecoveryFixture storage{[faults](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    auto            queue = recovery_queue(recovery_id(1));
    storage.insert_queue(queue);
    auto job = storage.make_job(recovery_id(2), queue.id);
    storage.insert_job(job);

    auto captured              = ByteBuffer(std::size_t{2} * 1024U * 1024U, std::byte{'x'});
    captured[1]                = std::byte{0xff};
    auto run                   = storage.make_run(recovery_id(3), job, RunState::Succeeded);
    run.attempts.back().output = jb::jobu::detail::AttemptOutput{
        .stdout_bytes     = captured,
        .stdout_truncated = true,
    };
    run.attempts.back().attempt.result = capture_result("cli", "stdout", captured.size(), captured.size() + 25U, true);
    storage.insert_run(run);

    FakeTimeSource        time;
    SequenceUuidGenerator tokens{{}};
    HistoryService        history{storage.database, storage.registry, tokens, time};
    auto                  key = AttemptKey{.run_id = run.run.id, .attempt_number = 1};
    selected.clear();
    auto chunk = history.read_output(request(key, OutputChannel::Stdout, 0, 3));
    REQUIRE(chunk);
    CHECK(chunk->status == OutputStatus::Available);
    CHECK(chunk->data == ByteBuffer{std::byte{'x'}, std::byte{0xff}, std::byte{'x'}});
    CHECK(chunk->encoding == OutputEncoding::Base64);
    CHECK(chunk->bytes_returned == 3);
    CHECK(chunk->next_offset == 3);
    CHECK(chunk->retained_bytes == captured.size());
    CHECK(chunk->total_bytes == captured.size() + 25U);
    CHECK(chunk->omitted_bytes == 25);
    CHECK(chunk->truncated);
    REQUIRE(selected.size() == 2);
    CHECK(selected[0].find("length(o.stdout_blob)") != std::string::npos);
    CHECK(selected[0].find("stdout_blob AS") == std::string::npos);
    CHECK(selected[1].find("substr(stdout_blob, :start, :limit)") != std::string::npos);

    auto eof = history.read_output(request(key, OutputChannel::Stdout, captured.size(), 3));
    REQUIRE(eof);
    CHECK(eof->data.empty());
    CHECK_FALSE(eof->next_offset);
    CHECK(history.read_output(request(key, OutputChannel::Stdout, captured.size() + 1U, 3)).error().code ==
          "jobu.history.invalid_request");
}

TEST_CASE("Output preserves split UTF-8 and both HTTP channels", "[jobu][history][output][sqlite]")
{
    Fixture fixture;
    auto    key  = fixture.insert(JobType::Http,
                                  RunState::Succeeded,
                                  jb::jobu::detail::AttemptOutput{
                                      .stdout_bytes = bytes("A\xc3\xa9"
                                                            "B"),
                                      .stderr_bytes = bytes("HTTP/1.1 200\r\n"),
                                  },
                                  capture_result("http", "body", 4, 4, false));
    auto    full = fixture.history.read_output(request(key, OutputChannel::Body, 0, 4));
    REQUIRE(full);
    CHECK(full->encoding == OutputEncoding::Utf8);
    CHECK(full->data == bytes("A\xc3\xa9"
                              "B"));
    CHECK(full->total_bytes == 4);
    CHECK(full->omitted_bytes == 0);

    auto split = fixture.history.read_output(request(key, OutputChannel::Body, 1, 1));
    REQUIRE(split);
    CHECK(split->encoding == OutputEncoding::Base64);
    CHECK(split->data == ByteBuffer{std::byte{0xc3}});
    CHECK(split->next_offset == 2);
    auto suffix = fixture.history.read_output(request(key, OutputChannel::Body, 2, 2));
    REQUIRE(suffix);
    CHECK(suffix->data == ByteBuffer{std::byte{0xa9}, std::byte{'B'}});
    CHECK_FALSE(suffix->next_offset);

    auto headers = fixture.history.read_output(request(key, OutputChannel::Headers));
    REQUIRE(headers);
    CHECK(headers->status == OutputStatus::Available);
    CHECK(headers->data == bytes("HTTP/1.1 200\r\n"));
    CHECK(fixture.history.read_output(request(key, OutputChannel::Stdout)).error().code ==
          "jobu.history.invalid_request");
}

TEST_CASE("Output distinguishes pending, empty, absent, and lost capture", "[jobu][history][output][sqlite]")
{
    SECTION("Incomplete attempt")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli, RunState::Running);
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stdout));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::Pending);
        CHECK(chunk->retained_bytes == 0);
        CHECK_FALSE(chunk->total_bytes);
        CHECK_FALSE(chunk->next_offset);
    }
    SECTION("Present empty BLOB")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli,
                                       RunState::Succeeded,
                                       jb::jobu::detail::AttemptOutput{.stdout_bytes = ByteBuffer{}});
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stdout));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::Available);
        CHECK(chunk->data.empty());
        CHECK_FALSE(chunk->total_bytes);
    }
    SECTION("Absent output row")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli, RunState::Succeeded);
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stdout));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::NotCaptured);
        CHECK_FALSE(chunk->capture_lost);
        CHECK_FALSE(chunk->total_bytes);
    }
    SECTION("Capture policy can omit a channel despite known observation counts")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli,
                                       RunState::Succeeded,
                                       std::nullopt,
                                       capture_result("cli", "stdout", 0, 5, true));
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stdout));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::NotCaptured);
        CHECK(chunk->retained_bytes == 0);
        CHECK(chunk->total_bytes == 5);
        CHECK(chunk->omitted_bytes == 5);
        CHECK(chunk->truncated);
    }
    SECTION("Null channel within a persisted output row")
    {
        Fixture fixture;
        auto    key     = fixture.insert(JobType::Cli,
                                         RunState::Succeeded,
                                         jb::jobu::detail::AttemptOutput{.stderr_bytes = bytes("diagnostic")});
        auto    missing = fixture.history.read_output(request(key, OutputChannel::Stdout));
        REQUIRE(missing);
        CHECK(missing->status == OutputStatus::NotCaptured);
        auto retained = fixture.history.read_output(request(key, OutputChannel::Stderr));
        REQUIRE(retained);
        CHECK(retained->status == OutputStatus::Available);
        CHECK(retained->data == bytes("diagnostic"));
    }
    SECTION("Explicit loss without persisted bytes")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli,
                                       RunState::Succeeded,
                                       std::nullopt,
                                       capture_result("cli", "stdout", 0, 0, false, true));
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stdout));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::Lost);
        CHECK(chunk->capture_lost);
        CHECK(chunk->total_bytes == 0);
        CHECK(chunk->omitted_bytes == 0);
    }
    SECTION("Interrupted recovery records empty lost BLOBs without observed totals")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli,
                                       RunState::Interrupted,
                                       jb::jobu::detail::AttemptOutput{
                                           .stdout_bytes = ByteBuffer{},
                                           .stderr_bytes = ByteBuffer{},
                                           .capture_lost = true,
                                       });
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stderr));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::Lost);
        CHECK(chunk->data.empty());
        CHECK_FALSE(chunk->total_bytes);
        CHECK_FALSE(chunk->omitted_bytes);
    }
    SECTION("Lost capture still returns its retained prefix")
    {
        Fixture fixture;
        auto    key   = fixture.insert(JobType::Cli,
                                       RunState::Succeeded,
                                       jb::jobu::detail::AttemptOutput{
                                           .stdout_bytes = bytes("prefix"),
                                           .capture_lost = true,
                                       });
        auto    chunk = fixture.history.read_output(request(key, OutputChannel::Stdout, 0, 3));
        REQUIRE(chunk);
        CHECK(chunk->status == OutputStatus::Lost);
        CHECK(chunk->data == bytes("pre"));
        CHECK(chunk->next_offset == 3);
        CHECK_FALSE(chunk->total_bytes);
    }
}

TEST_CASE("Output rejects invalid requests and closes on inconsistent stored evidence",
          "[jobu][history][output][sqlite]")
{
    Fixture fixture;
    auto    key = fixture.insert(JobType::Cli,
                                 RunState::Succeeded,
                                 jb::jobu::detail::AttemptOutput{.stdout_bytes = bytes("abc")},
                                 capture_result("cli", "stdout", 3, 2, false));
    CHECK(fixture.history.read_output(request(key, OutputChannel::Stdout, 0, 0)).error().code ==
          "jobu.history.invalid_request");
    CHECK(fixture.history.read_output(request(key, OutputChannel::Stdout, 0, 65'537)).error().code ==
          "jobu.history.invalid_request");
    CHECK(fixture.history.read_output(request(key, OutputChannel::Stdout, UINT64_MAX)).error().code ==
          "jobu.history.invalid_request");
    CHECK(fixture.history.read_output(request({.run_id = key.run_id, .attempt_number = 99}, OutputChannel::Stdout))
              .error()
              .code == "jobu.attempt.not_found");
    CHECK(fixture.failures.empty());

    auto damaged = fixture.history.read_output(request(key, OutputChannel::Stdout));
    REQUIRE_FALSE(damaged);
    CHECK(damaged.error().code == "jobu.storage.invariant");
    REQUIRE(fixture.failures.size() == 1);
    CHECK(fixture.history.read_output(request(key, OutputChannel::Stdout)).error().code ==
          "jobu.service.stopping");
}

TEST_CASE("Output slice storage failure closes history admission after the query", "[jobu][history][output][failure]")
{
    auto faults      = std::make_shared<DatabaseFaultState>();
    faults->classify = [](std::string_view sql) -> std::string {
        return sql.find("substr(stdout_blob") != std::string_view::npos ? "slice" : "other";
    };
    RecoveryFixture storage{[faults](std::unique_ptr<Driver> driver) {
        return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
    }};
    auto            queue = recovery_queue(recovery_id(1));
    storage.insert_queue(queue);
    auto job = storage.make_job(recovery_id(2), queue.id);
    storage.insert_job(job);
    auto run                   = storage.make_run(recovery_id(3), job, RunState::Succeeded);
    run.attempts.back().output = jb::jobu::detail::AttemptOutput{.stdout_bytes = bytes("captured")};
    storage.insert_run(run);

    FakeTimeSource        time;
    SequenceUuidGenerator tokens{{}};
    HistoryService        history{storage.database, storage.registry, tokens, time};
    auto                  failures = std::vector<Error>{};
    history.failed.connect(&history, [&failures](Error const& error) { failures.push_back(error); });
    faults->faults.push_back({
        .at    = {.boundary = "slice", .operation = DatabaseOperation::Execute},
        .error = {.category = ErrorCategory::Internal, .code = "db.corrupt", .message = "backend detail"},
    });

    auto key    = AttemptKey{.run_id = run.run.id, .attempt_number = 1};
    auto failed = history.read_output(request(key, OutputChannel::Stdout, 0, 2));
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "db.corrupt");
    REQUIRE(failures.size() == 1);
    CHECK(failures[0].message.find("backend detail") == std::string::npos);
    CHECK(history.read_output(request(key, OutputChannel::Stdout)).error().code == "jobu.service.stopping");
}
