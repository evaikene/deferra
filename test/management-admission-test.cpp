#include "management.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_database_driver.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::test;

namespace {

auto test_id() -> Uuid
{
    auto parsed = Uuid::parse("00000000-0000-7000-8000-000000000001");
    REQUIRE(parsed);
    return *parsed;
}

auto storage_error(std::string code) -> Error
{
    return {.category = ErrorCategory::Internal,
            .code     = std::move(code),
            .message  = "sensitive SQL and payload",
            .detail   = "sensitive backend diagnostics"};
}

auto require_error(auto const& result, std::string_view code) -> Error
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    return result.error();
}

void check_safe(Error const& error)
{
    CHECK(error.message.find("sensitive") == std::string::npos);
    CHECK(error.detail.find("sensitive") == std::string::npos);
}

struct Fixture {
    Fixture()
    {
        REQUIRE(database.open());
        service.failed.connect(&service, [this](Error const& error) {
            calls_at_failure = state->calls;
            failures.push_back(error);
            // A runtime observer may repeat the non-destructive shutdown latch.
            service.stop_mutations();
        });
        service.mutation_committed.connect(&service, [this] { ++committed; });
    }

    std::shared_ptr<FakeDatabaseDriverState> state{std::make_shared<FakeDatabaseDriverState>()};
    Database                                 database{std::make_unique<FakeDatabaseDriver>(state)};
    StandardAttributeRegistry                attributes;
    FakeCronEngine                           cron;
    SequenceUuidGenerator                    generator{
        {test_id(), test_id(), test_id()}
    };
    FakeTimeSource           time;
    std::vector<Error>       failures;
    std::vector<std::string> calls_at_failure;
    std::size_t              committed{0};
    ManagementService        service{database, attributes, cron, generator, time};
};

// Exercise every public mutation, including the C++-only Run Now entry point.
// Invalid arguments intentionally prove that admission precedes validation.
void check_all_rejected(Fixture& fixture)
{
    auto const before = fixture.state->calls;
    auto const id     = test_id();
    auto const check  = [](auto const& result) {
        auto error = require_error(result, "jobu.service.stopping");
        CHECK(error.category == ErrorCategory::Unavailable);
    };
    check(fixture.service.create_queue({}));
    check(fixture.service.update_queue({}));
    check(fixture.service.suspend_queue(std::string{}));
    check(fixture.service.resume_queue(std::string{}));
    check(fixture.service.delete_queue(std::string{}));
    check(fixture.service.create_job({}));
    check(fixture.service.update_job({}));
    check(fixture.service.suspend_job(id));
    check(fixture.service.resume_job(id));
    check(fixture.service.move_job({}));
    check(fixture.service.delete_job({}));
    check(fixture.service.run_now({.job_id = id, .idempotency_key = ""}));
    CHECK(fixture.state->calls == before);
    CHECK(fixture.committed == 0);
}

constexpr std::array mutation_names{"create_queue",
                                    "update_queue",
                                    "suspend_queue",
                                    "resume_queue",
                                    "delete_queue",
                                    "create_job",
                                    "update_job",
                                    "suspend_job",
                                    "resume_job",
                                    "move_job",
                                    "delete_job",
                                    "run_now"};

auto mutation_error(ManagementService& service, std::size_t index, std::string_view code) -> Error
{
    auto const id = test_id();
    switch (index) {
        case 0:
            return require_error(service.create_queue({.name = "queue"}), code);
        case 1:
            return require_error(service.update_queue({.queue = id, .weight = 2}), code);
        case 2:
            return require_error(service.suspend_queue(id), code);
        case 3:
            return require_error(service.resume_queue(id), code);
        case 4:
            return require_error(service.delete_queue(id), code);
        case 5: {
            JsonValue payload{.data = JsonValue::Object{{"command", JsonValue{.data = std::string{"/bin/true"}}}}};
            return require_error(service.create_job({.queue = id, .payload = std::move(payload)}), code);
        }
        case 6:
            return require_error(service.update_job({.job_id = id, .expected_revision = 1, .priority = 1}), code);
        case 7:
            return require_error(service.suspend_job(id), code);
        case 8:
            return require_error(service.resume_job(id), code);
        case 9:
            return require_error(service.move_job({.job_id = id, .expected_revision = 1, .target_queue = id}), code);
        case 10:
            return require_error(service.delete_job({.job_id = id, .expected_revision = 1}), code);
        default:
            return require_error(service.run_now({.job_id = id}), code);
    }
}

auto read_error(ManagementService& service, std::size_t index, std::string_view code) -> Error
{
    switch (index) {
        case 0:
            return require_error(service.get_queue(test_id()), code);
        case 1:
            return require_error(service.list_queues({}), code);
        case 2:
            return require_error(service.get_job(test_id()), code);
        default:
            return require_error(service.list_jobs({}), code);
    }
}

} // namespace

TEST_CASE("Management stopping is irreversible and precedes every mutation", "[jobu][management][admission]")
{
    Fixture fixture;
    fixture.service.stop_mutations();
    fixture.service.stop_mutations();
    check_all_rejected(fixture);
    CHECK(fixture.failures.empty());
    REQUIRE(fixture.generator.generate());

    // A healthy read still reaches storage after an explicit mutation stop.
    fixture.state->parameter_names                 = {":limit"};
    fixture.state->execution_info.produces_records = true;
    REQUIRE(fixture.service.list_queues({}));
    check_all_rejected(fixture);
}

TEST_CASE("Every management mutation reports a fatal begin failure", "[jobu][management][failure]")
{
    for (std::size_t index = 0; index < mutation_names.size(); ++index) {
        DYNAMIC_SECTION(mutation_names[index])
        {
            Fixture fixture;
            fixture.state->begin_error = storage_error("db.io");
            check_safe(mutation_error(fixture.service, index, "db.io"));
            REQUIRE(fixture.failures.size() == 1);
            CHECK(fixture.failures.front().code == "db.io");
            check_safe(fixture.failures.front());
            check_all_rejected(fixture);
        }
    }
}

TEST_CASE("Management fatal delivery follows transaction rollback", "[jobu][management][failure]")
{
    Fixture fixture;
    fixture.state->prepare_error = storage_error("db.io");
    require_error(fixture.service.create_queue({.name = "queue", .idempotency_key = "replay"}), "db.io");
    REQUIRE(fixture.failures.size() == 1);
    REQUIRE_FALSE(fixture.calls_at_failure.empty());
    CHECK(fixture.calls_at_failure.back() == "driver.rollback");
    CHECK(std::ranges::count(fixture.calls_at_failure, "driver.rollback") == 1);
    check_all_rejected(fixture);

    // Later read failures cannot replace or emit the first fatal error again.
    fixture.state->prepare_error = storage_error("db.corrupt");
    require_error(fixture.service.get_queue(test_id()), "db.corrupt");
    REQUIRE(fixture.failures.size() == 1);
    CHECK(fixture.failures.front().code == "db.io");
}

TEST_CASE("All management reads distinguish ordinary I/O from corruption", "[jobu][management][failure]")
{
    for (std::size_t index = 0; index < 4; ++index) {
        for (auto const corrupt : {false, true}) {
            DYNAMIC_SECTION("read " << index << " corrupt=" << corrupt)
            {
                Fixture     fixture;
                auto const* code             = corrupt ? "db.corrupt" : "db.io";
                fixture.state->prepare_error = storage_error(code);
                check_safe(read_error(fixture.service, index, code));
                CHECK(fixture.failures.size() == (corrupt ? 1U : 0U));
                CHECK(fixture.committed == 0);
                if (corrupt) {
                    check_all_rejected(fixture);
                }
                else {
                    // Reaching begin proves that an ordinary read did not latch admission.
                    fixture.state->begin_error = storage_error("db.busy");
                    require_error(fixture.service.create_queue({.name = "still-admitted"}), "db.busy");
                    REQUIRE(fixture.failures.size() == 1);
                    CHECK(fixture.failures.front().code == "db.busy");
                }
            }
        }
    }
}

TEST_CASE("Invalid management input remains nonfatal", "[jobu][management][admission]")
{
    Fixture    fixture;
    auto const before = fixture.state->calls;
    require_error(fixture.service.create_queue({.name = ""}), "jobu.queue.invalid_name");
    CHECK(fixture.failures.empty());
    CHECK(fixture.state->calls == before);
    fixture.state->begin_error = storage_error("db.locked");
    require_error(fixture.service.create_queue({.name = "valid"}), "db.locked");
    REQUIRE(fixture.failures.size() == 1);
}
