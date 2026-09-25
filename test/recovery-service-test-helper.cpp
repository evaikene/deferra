#include "recovery_priv.hpp"
#include "support/fake_cron_engine.hpp"
#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"

#include "attribute_registry.hpp"
#include "database.hpp"
#include "run_repository_priv.hpp"
#include "sqlite/sqlite_driver.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <utility>

#include <unistd.h>

using namespace jb::core;
using namespace jb::db;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

int main(int argc, char* argv[])
{
    if (argc != 6) {
        return 2;
    }

    auto first_run       = Uuid::parse(argv[2]);
    auto successor_one   = Uuid::parse(argv[3]);
    auto successor_two   = Uuid::parse(argv[4]);
    auto successor_three = Uuid::parse(argv[5]);
    if (!first_run || !successor_one || !successor_two || !successor_three) {
        return 3;
    }

    // This process starts with no inherited SQLite or test-runner state.
    auto driver = std::make_unique<jb::db::sqlite::Driver>(
        jb::db::sqlite::Options{.database_file = std::filesystem::path{argv[1]},
                                .busy_timeout  = 1000ms,
                                .durability    = jb::db::sqlite::Durability::Normal});
    Database database{std::move(driver)};
    if (!database.open()) {
        return 10;
    }

    StandardAttributeRegistry registry;
    FakeCronEngine            cron;
    // Match the schedule and fixed clock used when the parent seeded both running jobs.
    cron.set_occurrences(CronSchedule{.expression = "* * * * *", .timezone = "UTC"},
                         {UtcTimePoint{120s}, UtcTimePoint{180s}, UtcTimePoint{300s}});
    FakeTimeSource time;
    time.set_utc(UtcTimePoint{120s});
    SequenceUuidGenerator generator{
        {*successor_one, *successor_two, *successor_three}
    };
    RunRepository runs{database, registry};

    bool observed_precommit = false;
    auto result             = recover_startup(database, registry, cron, generator, time, {.scan_batch_size = 1}, [&] {
        auto row = runs.find_by_id(*first_run);
        if (!row || !*row) {
            ::_exit(11);
        }
        if ((*row)->state == RunState::Interrupted && std::exchange(observed_precommit, true)) {
            // The next page boundary follows the first committed unit. Exit without
            // destructors to model an abrupt daemon death before recovery finishes.
            ::_exit(77);
        }
        return false;
    });

    return result ? 12 : 13;
}
