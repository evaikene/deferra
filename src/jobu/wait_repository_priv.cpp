#include "wait_repository_priv.hpp"

#include "domain_storage_priv.hpp"
#include "query.hpp"
#include "record.hpp"
#include "value.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

auto invalid_state(std::string_view reason, StorageFailureOrigin origin = StorageFailureOrigin::PersistedData)
    -> TelemetryFailure
{
    return {
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "jobu.telemetry.invalid_state",
                    .message  = "Runnable-wait accounting state is invalid",
                    .detail   = "reason=" + std::string{reason},
                    },
        .origin = origin,
    };
}

auto clock_regression() -> TelemetryFailure
{
    return {
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "jobu.telemetry.clock_regression",
                    .message  = "Runnable-wait monotonic time regressed",
                    },
    };
}

auto counter_overflow() -> TelemetryFailure
{
    return {
        .error =
            {
                    .category = jb::core::ErrorCategory::Internal,
                    .code     = "jobu.telemetry.counter_overflow",
                    .message  = "Runnable-wait duration cannot be represented",
                    },
    };
}

auto pending(RunState state) noexcept -> bool
{
    return state == RunState::Scheduled || state == RunState::RetryWait;
}

auto quality_text(WaitQuality quality) noexcept -> std::string_view
{
    switch (quality) {
        case WaitQuality::Unmeasured:
            return "unmeasured";
        case WaitQuality::Complete:
            return "complete";
        case WaitQuality::Partial:
            return "partial";
    }
    return {};
}

auto decode_timing(jb::db::Record const& row) -> TelemetryResult<WaitTiming>
{
    auto        state  = read_run_state(row, "state");
    auto        status = read_text(row, "measurement_status");
    auto        warned = read_boolean(row, "delay_warned");
    auto const* wait   = row.value("runnable_wait_us");
    auto const* epoch  = row.value("open_epoch");
    auto const* tick   = row.value("open_tick_us");
    auto const* count  = wait == nullptr ? nullptr : std::get_if<std::int64_t>(wait);
    if (!state || !status || !warned || count == nullptr || *count < 0 || epoch == nullptr || tick == nullptr) {
        return TelemetryResult<WaitTiming>::failure(invalid_state("malformed_row"));
    }

    WaitTiming timing{.state = *state, .runnable_wait_us = *count, .delay_warned = *warned};
    if (*status == "complete") {
        timing.quality = WaitQuality::Complete;
    }
    else if (*status == "partial") {
        timing.quality = WaitQuality::Partial;
    }
    else if (*status != "unmeasured") {
        return TelemetryResult<WaitTiming>::failure(invalid_state("unknown_quality"));
    }

    // The pair is decoded independently of backend constraints, including damaged storage.
    if (!std::holds_alternative<jb::db::Null>(*epoch)) {
        auto        value = read_uuid(row, "open_epoch");
        auto const* open  = std::get_if<std::int64_t>(tick);
        if (!value || value->is_nil() || open == nullptr || *open < 0 || !pending(timing.state)) {
            return TelemetryResult<WaitTiming>::failure(invalid_state("invalid_open_interval"));
        }
        timing.open_epoch   = *value;
        timing.open_tick_us = *open;
    }
    else if (!std::holds_alternative<jb::db::Null>(*tick)) {
        return TelemetryResult<WaitTiming>::failure(invalid_state("unpaired_open_interval"));
    }

    if (timing.quality == WaitQuality::Unmeasured &&
        (timing.runnable_wait_us != 0 || timing.open_epoch || timing.delay_warned)) {
        return TelemetryResult<WaitTiming>::failure(invalid_state("invalid_unmeasured_row"));
    }
    return TelemetryResult<WaitTiming>::success(timing);
}

auto validate_sample(WaitTiming const& timing, WaitSample const& sample) -> TelemetryResult<void>
{
    if (sample.epoch.is_nil()) {
        return TelemetryResult<void>::failure(invalid_state("nil_sample_epoch", StorageFailureOrigin::Operation));
    }
    if (sample.tick_us < 0) {
        return TelemetryResult<void>::failure(clock_regression());
    }
    if (timing.open_epoch) {
        // Foreign ticks have no relationship to this origin. Recovery, not arithmetic, repairs them.
        if (*timing.open_epoch != sample.epoch) {
            return TelemetryResult<void>::failure(invalid_state("foreign_open_epoch"));
        }
        if (sample.tick_us < *timing.open_tick_us) {
            return TelemetryResult<void>::failure(clock_regression());
        }
    }
    return TelemetryResult<void>::success();
}

auto known_wait(WaitTiming const& timing, WaitSample const& sample) -> TelemetryResult<std::int64_t>
{
    auto valid = validate_sample(timing, sample);
    if (!valid) {
        return TelemetryResult<std::int64_t>::failure(std::move(valid).error());
    }
    auto const delta = timing.open_tick_us ? sample.tick_us - *timing.open_tick_us : 0;
    if (timing.runnable_wait_us > std::numeric_limits<std::int64_t>::max() - delta) {
        return TelemetryResult<std::int64_t>::failure(counter_overflow());
    }
    return TelemetryResult<std::int64_t>::success(timing.runnable_wait_us + delta);
}

} // namespace

WaitRepository::WaitRepository(jb::db::Database& database) noexcept
    : _database{database}
{}

auto WaitRepository::list_open(std::size_t limit, std::optional<jb::core::Uuid> after)
    -> TelemetryResult<std::vector<jb::core::Uuid>>
{
    using PageResult = TelemetryResult<std::vector<jb::core::Uuid>>;
    if (limit < 1 || limit > 4096) {
        return PageResult::failure(invalid_state("invalid_page_limit", StorageFailureOrigin::Operation));
    }

    jb::db::Query query{_database};
    auto          sql = std::string{"SELECT run_id FROM jobu_run_timing WHERE "
                                    "(open_epoch IS NOT NULL OR open_tick_us IS NOT NULL)"};
    if (after) {
        sql += " AND run_id > :after";
    }
    sql         += " ORDER BY run_id LIMIT :limit";
    auto result  = query.prepare(sql);
    if (result && after) {
        result = query.bind_value(":after", uuid_to_storage(*after));
    }
    if (result) {
        result = query.bind_value(":limit", static_cast<std::int64_t>(limit));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return PageResult::failure({.error = std::move(result).error()});
    }

    std::vector<jb::core::Uuid> ids;
    for (;;) {
        auto next = query.next();
        if (!next) {
            return PageResult::failure({.error = std::move(next).error()});
        }
        if (!*next) {
            break;
        }
        auto id = read_uuid(query.record(), "run_id");
        if (!id || id->is_nil() || (after && *id <= *after) || (!ids.empty() && *id <= ids.back())) {
            return PageResult::failure(invalid_state("invalid_page_key"));
        }
        ids.push_back(*id);
    }
    auto finished = query.finish();
    if (!finished) {
        return PageResult::failure({.error = std::move(finished).error()});
    }
    return PageResult::success(std::move(ids));
}

auto WaitRepository::read(jb::core::Uuid const& run_id) -> TelemetryResult<WaitTiming>
{
    jb::db::Query query{_database};
    auto          result = query.prepare("SELECT r.state, t.runnable_wait_us, t.measurement_status, "
                                         "t.open_epoch, t.open_tick_us, t.delay_warned "
                                         "FROM jobu_runs r JOIN jobu_run_timing t ON t.run_id = r.id WHERE r.id = :id");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(run_id));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return TelemetryResult<WaitTiming>::failure({.error = std::move(result).error()});
    }

    auto next = query.next();
    if (!next) {
        return TelemetryResult<WaitTiming>::failure({.error = std::move(next).error()});
    }
    if (!*next) {
        return TelemetryResult<WaitTiming>::failure(invalid_state("missing_timing_row"));
    }
    auto timing = decode_timing(query.record());
    if (!timing) {
        return timing;
    }

    next = query.next();
    if (!next) {
        return TelemetryResult<WaitTiming>::failure({.error = std::move(next).error()});
    }
    if (*next) {
        return TelemetryResult<WaitTiming>::failure(invalid_state("duplicate_timing_row"));
    }
    auto finished = query.finish();
    if (!finished) {
        return TelemetryResult<WaitTiming>::failure({.error = std::move(finished).error()});
    }
    return timing;
}

auto WaitRepository::write(jb::core::Uuid const& run_id, WaitTiming const& timing) -> TelemetryResult<void>
{
    // Read and update are serialized inside the caller's transaction. Only accounting columns
    // change: domain state, warning claims and the run's immutable snapshot stay with their owners.
    jb::db::Query query{_database};
    auto result = query.prepare("UPDATE jobu_run_timing SET runnable_wait_us = :wait, measurement_status = :status, "
                                "open_epoch = :epoch, open_tick_us = :tick WHERE run_id = :id");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(run_id));
    }
    if (result) {
        result = query.bind_value(":wait", timing.runnable_wait_us);
    }
    if (result) {
        result = query.bind_value(":status", jb::db::make_text(quality_text(timing.quality)));
    }
    if (result) {
        result = query.bind_value(":epoch", timing.open_epoch ? uuid_to_storage(*timing.open_epoch) : jb::db::Null{});
    }
    if (result) {
        result = query.bind_value(":tick", timing.open_tick_us ? jb::db::Value{*timing.open_tick_us} : jb::db::Null{});
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return TelemetryResult<void>::failure({.error = std::move(result).error()});
    }
    if (query.num_rows_affected() != 1) {
        return TelemetryResult<void>::failure(invalid_state("timing_update_count"));
    }
    auto finished = query.finish();
    if (!finished) {
        return TelemetryResult<void>::failure({.error = std::move(finished).error()});
    }
    return TelemetryResult<void>::success();
}

auto WaitRepository::repair_abandoned(jb::core::Uuid const& run_id) -> TelemetryResult<bool>
{
    auto timing = read(run_id);
    if (!timing) {
        return TelemetryResult<bool>::failure(std::move(timing).error());
    }
    if (!timing->open_epoch) {
        return TelemetryResult<bool>::success(false);
    }

    // There is no live epoch during startup recovery. Neither the old monotonic origin nor
    // the uncheckpointed tail can be reconstructed from UTC or from a new activation's tick.
    timing->quality = WaitQuality::Partial;
    timing->open_epoch.reset();
    timing->open_tick_us.reset();
    auto written = write(run_id, *timing);
    if (!written) {
        return TelemetryResult<bool>::failure(std::move(written).error());
    }
    return TelemetryResult<bool>::success(true);
}

auto WaitRepository::open_interval(jb::core::Uuid const& run_id, WaitSample const& sample)
    -> TelemetryResult<WaitTiming>
{
    auto timing = read(run_id);
    if (!timing) {
        return timing;
    }
    auto valid = validate_sample(*timing, sample);
    if (!valid) {
        return TelemetryResult<WaitTiming>::failure(std::move(valid).error());
    }
    if (!pending(timing->state)) {
        return TelemetryResult<WaitTiming>::failure(invalid_state("opening_nonpending_run"));
    }
    if (timing->open_epoch) {
        return timing;
    }

    // A late first observation cannot certify earlier unknown wait as an exact zero sample.
    if (timing->quality == WaitQuality::Unmeasured) {
        timing->quality = WaitQuality::Partial;
    }
    timing->open_epoch   = sample.epoch;
    timing->open_tick_us = sample.tick_us;
    auto written         = write(run_id, *timing);
    if (!written) {
        return TelemetryResult<WaitTiming>::failure(std::move(written).error());
    }
    return timing;
}

auto WaitRepository::accumulate(jb::core::Uuid const& run_id, WaitSample const& sample, bool close)
    -> TelemetryResult<WaitTiming>
{
    auto timing = read(run_id);
    if (!timing) {
        return timing;
    }
    auto valid = validate_sample(*timing, sample);
    if (!valid) {
        return TelemetryResult<WaitTiming>::failure(std::move(valid).error());
    }
    if (!timing->open_epoch) {
        return timing;
    }

    // Both ticks are nonnegative and ordered. Check addition before modifying any durable value.
    auto const delta = sample.tick_us - *timing->open_tick_us;
    if (timing->runnable_wait_us > std::numeric_limits<std::int64_t>::max() - delta) {
        return TelemetryResult<WaitTiming>::failure(counter_overflow());
    }
    timing->runnable_wait_us += delta;
    if (close) {
        timing->open_epoch.reset();
        timing->open_tick_us.reset();
    }
    else {
        timing->open_tick_us = sample.tick_us;
    }

    auto written = write(run_id, *timing);
    if (!written) {
        return TelemetryResult<WaitTiming>::failure(std::move(written).error());
    }
    return timing;
}

auto WaitRepository::settle(jb::core::Uuid const& run_id, WaitSample const& sample) -> TelemetryResult<WaitTiming>
{
    return accumulate(run_id, sample, true);
}

auto WaitRepository::rebase(jb::core::Uuid const& run_id, WaitSample const& sample) -> TelemetryResult<WaitTiming>
{
    return accumulate(run_id, sample, false);
}

auto WaitRepository::claim_warning(jb::core::Uuid const&     run_id,
                                   WaitSample const&         sample,
                                   std::chrono::milliseconds threshold) -> TelemetryResult<WaitWarning>
{
    auto timing = read(run_id);
    if (!timing) {
        return TelemetryResult<WaitWarning>::failure(std::move(timing).error());
    }
    auto total = known_wait(*timing, sample);
    if (!total) {
        return TelemetryResult<WaitWarning>::failure(std::move(total).error());
    }
    if (!pending(timing->state) || timing->quality == WaitQuality::Unmeasured || threshold.count() < 0) {
        return TelemetryResult<WaitWarning>::failure(invalid_state("invalid_warning_context"));
    }

    WaitWarning effect{.known_wait = std::chrono::microseconds{*total}};
    // Comparing policy to the counter's range before conversion prevents a huge valid threshold
    // from wrapping into an immediate warning. Such a threshold can never be exceeded.
    if (timing->delay_warned || threshold.count() == 0 ||
        threshold.count() > std::numeric_limits<std::int64_t>::max() / 1000) {
        return TelemetryResult<WaitWarning>::success(effect);
    }
    auto const threshold_us = threshold.count() * 1000;
    if (*total <= threshold_us) {
        effect.until_warning = std::chrono::microseconds{threshold_us - *total + 1};
        return TelemetryResult<WaitWarning>::success(effect);
    }

    // Only the successful compare-and-update claimant owns delivery. The enclosing transaction
    // makes this flag atomic with observation or the Running claim; rollback emits nothing.
    jb::db::Query query{_database};
    auto result = query.prepare("UPDATE jobu_run_timing SET delay_warned = 1 WHERE run_id = :id AND delay_warned = 0");
    if (result) {
        result = query.bind_value(":id", uuid_to_storage(run_id));
    }
    if (result) {
        result = query.exec();
    }
    if (!result) {
        return TelemetryResult<WaitWarning>::failure({.error = std::move(result).error()});
    }
    auto const affected = query.num_rows_affected();
    if (affected != 0 && affected != 1) {
        return TelemetryResult<WaitWarning>::failure(invalid_state("warning_update_count"));
    }
    auto finished = query.finish();
    if (!finished) {
        return TelemetryResult<WaitWarning>::failure({.error = std::move(finished).error()});
    }
    effect.claimed = affected == 1;
    return TelemetryResult<WaitWarning>::success(effect);
}

} // namespace jb::jobu::detail
