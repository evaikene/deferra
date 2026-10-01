#pragma once

#include "catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "fault_database_driver.hpp"
#include "recovery_fixture.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "run.hpp"
#include "uuid.hpp"
#include "wait_repository_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace jb::test {

/// Real timing storage with named fault boundaries and a single inert run; no management or runner hooks.
struct TelemetryStorageFixture {
    explicit TelemetryStorageFixture(jobu::RunState state = jobu::RunState::Scheduled)
        : storage{[this](std::unique_ptr<db::Driver> driver) {
            return std::make_unique<FaultDatabaseDriver>(std::move(driver), faults);
        }}
        , repository{storage.database}
    {
        faults->classify = [](std::string_view sql) {
            if (sql.starts_with("UPDATE jobu_run_timing")) {
                return std::string{"timing.write"};
            }
            return std::string{"timing.read"};
        };
        auto queue = recovery_queue(queue_id);
        auto job   = storage.make_job(job_id, queue_id);
        storage.insert_queue(queue);
        storage.insert_job(job);
        storage.insert_run(storage.make_run(run_id, job, state, state == jobu::RunState::RetryWait ? 1 : 0));
        faults->calls.clear();
    }

    /// Establishes an explicit quality/counter fixture without pretending observation happened.
    void seed(std::string_view quality, std::int64_t wait = 0, bool warned = false)
    {
        db::Query query{storage.database};
        REQUIRE(query.prepare("UPDATE jobu_run_timing SET measurement_status = :quality, "
                              "runnable_wait_us = :wait, delay_warned = :warned WHERE run_id = :id"));
        REQUIRE(query.bind_value(":quality", db::make_text(quality)));
        REQUIRE(query.bind_value(":wait", wait));
        REQUIRE(query.bind_value(":warned", jobu::detail::boolean_to_storage(warned)));
        REQUIRE(query.bind_value(":id", jobu::detail::uuid_to_storage(run_id)));
        REQUIRE(query.exec());
    }

    /// Installs an open tail deliberately, including a foreign activation for rejection tests.
    void seed_open(core::Uuid epoch, std::int64_t tick)
    {
        db::Query query{storage.database};
        REQUIRE(
            query.prepare("UPDATE jobu_run_timing SET open_epoch = :epoch, open_tick_us = :tick WHERE run_id = :id"));
        REQUIRE(query.bind_value(":epoch", jobu::detail::uuid_to_storage(epoch)));
        REQUIRE(query.bind_value(":tick", tick));
        REQUIRE(query.bind_value(":id", jobu::detail::uuid_to_storage(run_id)));
        REQUIRE(query.exec());
    }

    std::shared_ptr<DatabaseFaultState> faults{std::make_shared<DatabaseFaultState>()};
    RecoveryFixture                     storage;
    jobu::detail::WaitRepository        repository;
    core::Uuid                          queue_id{recovery_id(1)};
    core::Uuid                          job_id{recovery_id(2)};
    core::Uuid                          run_id{recovery_id(3)};
    core::Uuid                          epoch{recovery_id(100)};
};

} // namespace jb::test
