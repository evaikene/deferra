#include "statistics-test.hpp"

#include "recovery-proxy.hpp"
#include "request.hpp"

#include <phpcxx/zend.hpp> // IWYU pragma: keep Zend declarations precede zend_exceptions.h

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <jb/jobu/statistics.hpp>
#include <jb/jobu/statistics_json.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace jobu_php_test {
namespace {
using jb::core::JsonValue;

auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "public statistics test");
    auto result = php::Value::adopt(slot);
    INFO(body);
    if (php::has_exception()) {
        zend_exception_error(EG(exception), E_WARNING);
        zend_clear_exception();
        FAIL("Unexpected PHP exception");
    }
    REQUIRE(status == SUCCESS);
    REQUIRE(result.view().is_true());
}

auto prepare() -> void
{
    run(R"PHP(
        $c = $GLOBALS['statsClient'] = new JobU\Client(STATISTICS_SOCKET);
        $q = $GLOBALS['statsQueue'] = $c->createQueue('stats-proxy-' . bin2hex(random_bytes(6)));
        $w = $GLOBALS['statsWindow'] = new JobU\TimeRange(new DateTimeImmutable('2100-01-01Z'), new DateTimeImmutable('2100-01-02Z'));
        for ($i = 0; $i < 3; ++$i) {
            $q->createCliJob(new JobU\CliPayload('/bin/true'), new JobU\OnceSchedule($w->getFrom()));
        }
        $GLOBALS['statsQuery'] = new JobU\StatisticsQuery(queueId: $q->getId(), planned: $w, groupBy: JobU\StatisticsGroupBy::Job, limit: 1);
        return true;
    )PHP");
}

auto argument_checks(RecoveryProxy& proxy) -> void
{
    auto before = proxy.requests;
    run(R"PHP(
        $c = $GLOBALS['statsClient']; $q = $GLOBALS['statsQueue'];
        foreach ([JobU\Duration::milliseconds(0), JobU\Duration::seconds(-1)] as $timeout) {
            statisticsThrows(ValueError::class, fn() => $c->getSystemInfo($timeout));
            statisticsThrows(ValueError::class, fn() => $c->systemStatistics(timeout: $timeout));
            statisticsThrows(ValueError::class, fn() => $q->statistics(timeout: $timeout));
        }
        statisticsThrows(TypeError::class, fn() => $c->systemStatistics([]));
        statisticsThrows(TypeError::class, fn() => $q->statistics(new JobU\RunQuery()));
        statisticsThrows(TypeError::class, fn() => $c->getSystemInfo('invalid'));
        statisticsThrows(ValueError::class, fn() => $q->statistics(new JobU\StatisticsQuery(queueId: '12345678-1234-1234-1234-123456789abc')));
        return true;
    )PHP");
    CHECK(proxy.requests == before);

    auto reads             = proxy.requests["system.info"].size();
    proxy.transform_result = [](std::string const& method, JsonValue& result) {
        if (method == "system.info") {
            std::get<JsonValue::Object>(result.data)["daemon_version"] =
                JsonValue{.data = std::string("explicit-read")};
        }
    };
    run("return $GLOBALS['statsClient']->getSystemInfo()->getDaemonVersion() === 'explicit-read';");
    CHECK(proxy.requests["system.info"].size() == reads + 1);
    proxy.transform_result = {};
}

// Use a codec-valid native specimen to exercise nonzero counters and fractional
// samples independently of real wall-clock timing and external job execution.
auto specimen() -> JsonValue
{
    jb::jobu::StatisticsGroup group;
    group.runs                       = {.total            = 4,
                                        .scheduled        = 1,
                                        .succeeded        = 1,
                                        .failed           = 1,
                                        .cancelled        = 1,
                                        .cli              = 2,
                                        .http             = 2,
                                        .scheduled_origin = 3,
                                        .manual_origin    = 1};
    group.attempts                   = {.total       = 5,
                                        .pending     = 1,
                                        .running     = 1,
                                        .completed   = 3,
                                        .succeeded   = 1,
                                        .failed      = 1,
                                        .interrupted = 1,
                                        .retries     = 2};
    group.capture                    = {.truncated_attempts = 3, .lost_attempts = 2};
    group.schedule_lateness_ms       = {.samples = 2, .average = 1.25, .maximum = 2.5};
    group.execution_wall_duration_ms = {.samples = 1, .average = 0.0, .maximum = 0.0};
    group.runnable_wait_ms           = jb::jobu::StatisticsDuration{.samples = 1, .average = 0.0, .maximum = 0.0};
    group.runnable_wait_coverage     = {.complete = 1, .partial = 1, .unmeasured = 1, .unfinished = 1};
    auto                     now     = jb::core::UtcClock::now();
    jb::jobu::StatisticsPage page{
        .window      = {.from = now, .to = now + std::chrono::hours(1)},
        .groups      = {group},
        .measurement = {.runnable_wait = "monotonic_observed"}
    };
    auto encoded = jb::jobu::statistics_page_to_json(page);
    REQUIRE(encoded);
    REQUIRE(jb::jobu::statistics_page_from_json(encoded.value()));
    return encoded.value().as_object().at("groups").as_array()[0];
}

auto projection_checks(RecoveryProxy& proxy) -> void
{
    auto group             = specimen();
    proxy.transform_result = [group](std::string const& method, JsonValue& result) {
        if (method == "system.stats") {
            std::get<JsonValue::Object>(result.data)["groups"] = JsonValue{.data = JsonValue::Array{group}};
        }
    };
    run(R"PHP(
        $p = $GLOBALS['statsClient']->systemStatistics(); $p->next();
        $g = $p->items()[0];
        statisticsCheck($g->getRuns()->getTotal() === 4 && $g->getRuns()->getStateCount(JobU\RunState::Cancelled) === 1, 'run counters');
        statisticsCheck($g->getRuns()->getTypeCount(JobU\JobType::Http) === 2 && $g->getRuns()->getOriginCount(JobU\RunOrigin::Manual) === 1, 'breakdowns');
        statisticsCheck($g->getAttempts()->getTotal() === 5 && $g->getAttempts()->getRetries() === 2, 'attempt and retry counts');
        statisticsCheck($g->getAttempts()->getStateCount(JobU\AttemptState::Running) === 1 && $g->getAttempts()->getOutcomeCount(JobU\AttemptOutcome::Interrupted) === 1, 'attempt states/outcomes');
        statisticsCheck($g->getCapture()->getTruncatedAttempts() === 3 && $g->getCapture()->getLostAttempts() === 2, 'capture counts');
        statisticsCheck($g->getScheduleLateness()->getSamples() === 2 && $g->getScheduleLateness()->getAverageMilliseconds() === 1.25 && $g->getScheduleLateness()->getMaximumMilliseconds() === 2.5, 'fractional milliseconds');
        statisticsCheck($g->getExecutionWallDuration()->getAverageMilliseconds() === 0.0 && $g->getRunnableWait()->getMaximumMilliseconds() === 0.0, 'measured zeros');
        $coverage = $g->getRunnableWaitCoverage();
        statisticsCheck([$coverage->getComplete(), $coverage->getPartial(), $coverage->getUnmeasured(), $coverage->getUnfinished()] === [1, 1, 1, 1], 'coverage distinctions');
        statisticsCheck($g == $p->items()[0] && $g->getRuns() == $g->getRuns(), 'snapshot equality');
        statisticsThrows(TypeError::class, fn() => $g->getRuns()->getStateCount(JobU\AttemptState::Pending));
        return true;
    )PHP");
    proxy.transform_result = [group](std::string const& method, JsonValue& result) {
        if (method == "system.stats") {
            auto unmeasured                = group.as_object();
            unmeasured["runnable_wait_ms"] = JsonValue{};
            auto& fields                   = std::get<JsonValue::Object>(result.data);
            fields["groups"] = JsonValue{.data = JsonValue::Array{JsonValue{.data = std::move(unmeasured)}}};
            std::get<JsonValue::Object>(fields.at("measurement").data)["runnable_wait"] =
                JsonValue{.data = std::string("unavailable")};
        }
    };
    run(R"PHP(
        $p = $GLOBALS['statsClient']->systemStatistics(); $p->next();
        return $p->items()[0]->getRunnableWait() === null && $p->getStatisticsMetadata()->getMeasurement()->getRunnableWait() === 'unavailable';
    )PHP");
    proxy.transform_result = {};

    // A legal 64-bit count remains an integer; the next value must fail eagerly,
    // rather than becoming a float/string or failing later in a typed getter.
    for (auto overflow : {false, true}) {
        auto number = static_cast<std::uint64_t>(std::numeric_limits<zend_long>::max()) + (overflow ? 1U : 0U);
        proxy.transform_result = [number](std::string const& method, JsonValue& result) {
            if (method == "system.stats") {
                auto& group_fields = std::get<JsonValue::Object>(
                    std::get<JsonValue::Array>(std::get<JsonValue::Object>(result.data).at("groups").data)[0].data);
                auto& runs    = std::get<JsonValue::Object>(group_fields.at("runs").data);
                runs["total"] = JsonValue{.data = number};
                std::get<JsonValue::Object>(runs.at("states").data)["scheduled"] = JsonValue{.data = number};
                std::get<JsonValue::Object>(group_fields.at("runnable_wait_coverage").data)["unfinished"] =
                    JsonValue{.data = number};
            }
        };
        run(overflow ? "statisticsThrows(ValueError::class, fn() => $GLOBALS['statsClient']->systemStatistics()); "
                       "return true;"
                     : "$p = $GLOBALS['statsClient']->systemStatistics(); $p->next(); return "
                       "$p->items()[0]->getRuns()->getTotal() === PHP_INT_MAX;");
    }
    proxy.transform_result = {};
}

auto cursor_only(RecoveryProxy const& proxy, std::string const& method) -> void
{
    auto const& fields = proxy.requests.at(method).back().as_object();
    REQUIRE(fields.size() == 1);
    CHECK(fields.contains("cursor"));
}

auto pagination_checks(RecoveryProxy& proxy) -> void
{
    for (auto const* method : {"system.stats", "queue.stats"}) {
        auto const* call = method == std::string_view("system.stats")
                             ? "$GLOBALS['statsClient']->systemStatistics($GLOBALS['statsQuery'])"
                             : "$GLOBALS['statsQueue']->statistics($GLOBALS['statsQuery'])";
        run("$GLOBALS['statsPage'] = " + std::string(call) + "; return true;");
        auto        before  = proxy.requests[method].size();
        auto const& initial = proxy.requests[method].back().as_object();
        CHECK(initial.contains("queue_id"));
        CHECK(initial.at("limit").as_uint() == 1);
        run(R"PHP(
            $p = $GLOBALS['statsPage']; $m = $p->getStatisticsMetadata();
            statisticsCheck($p->items() === [] && $p->hasNext(), 'prefetch');
            $p->next();
            return $p->getStatisticsMetadata() === $m;
        )PHP");
        CHECK(proxy.requests[method].size() == before);

        auto error = jb::core::parse_json(
            R"({"code":-32000,"message":"private diagnostic","data":{"category":"invalid_argument","code":"jobu.statistics.invalid_cursor"}})");
        REQUIRE(error);
        proxy.error_method   = method;
        proxy.response_error = error.value();
        run(R"PHP(
            $p = $GLOBALS['statsPage']; $old = $p->items(); $m = $p->getStatisticsMetadata();
            $e = statisticsThrows(JobU\InvalidCursorException::class, fn() => $p->next());
            return $p->items() === $old && $p->getStatisticsMetadata() === $m && $p->hasNext()
                && $e->getErrorCode() === 'jobu.statistics.invalid_cursor' && !str_contains($e->getMessage(), 'private diagnostic');
        )PHP");
        CHECK(proxy.requests[method].size() == before + 1);
        cursor_only(proxy, method);
        proxy.response_error.reset();

        // An overflow after valid native decoding must leave the whole page intact.
        proxy.transform_result = [method](std::string const& name, JsonValue& result) {
            if (name == method) {
                auto& fields = std::get<JsonValue::Object>(result.data);
                auto& group = std::get<JsonValue::Object>(std::get<JsonValue::Array>(fields.at("groups").data)[0].data);
                std::get<JsonValue::Object>(group.at("attempts").data)["retries"] = JsonValue{.data = UINT64_MAX};
            }
        };
        run(R"PHP(
            $p = $GLOBALS['statsPage']; $old = $p->items(); $m = $p->getStatisticsMetadata();
            statisticsThrows(ValueError::class, fn() => $p->next());
            return $p->items() === $old && $p->getStatisticsMetadata() === $m && $p->hasNext();
        )PHP");
        proxy.transform_result = {};
        proxy.drop_method      = method;
        proxy.drops            = 1;
        before                 = proxy.requests[method].size();
        run(R"PHP(
            $p = $GLOBALS['statsPage']; $old = $p->items(); $m = $p->getStatisticsMetadata();
            $p->next();
            return $p->items() !== $old && $p->getStatisticsMetadata() !== $m && $p->getStatisticsMetadata() == $m;
        )PHP");
        REQUIRE(proxy.requests[method].size() == before + 2);
        CHECK(proxy.requests[method][before] == proxy.requests[method][before + 1]);
        cursor_only(proxy, method);
    }

    // Force a legal empty final page: metadata must still advance with the batch.
    proxy.transform_result = [](std::string const& method, JsonValue& result) {
        if (method == "queue.stats") {
            auto& fields          = std::get<JsonValue::Object>(result.data);
            fields["groups"]      = JsonValue{.data = JsonValue::Array{}};
            fields["next_cursor"] = JsonValue{};
        }
    };
    run(R"PHP(
        $p = $GLOBALS['statsPage']; $m = $p->getStatisticsMetadata(); $p->next();
        return !$p->hasNext() && $p->items() === [] && $p->getStatisticsMetadata() !== $m && $p->getStatisticsMetadata() == $m;
    )PHP");
    proxy.transform_result = {};

    // Substitute a real queue cursor into a system page; the daemon owns scope checks.
    std::string queue_cursor;
    proxy.transform_result = [&queue_cursor](std::string const& method, JsonValue& result) {
        auto& fields = std::get<JsonValue::Object>(result.data);
        if (method == "queue.stats") {
            queue_cursor = fields.at("next_cursor").as_string();
        }
        if (method == "system.stats") {
            fields["next_cursor"] = JsonValue{.data = queue_cursor};
        }
    };
    run(R"PHP(
        $GLOBALS['statsQueue']->statistics($GLOBALS['statsQuery']);
        $p = $GLOBALS['statsClient']->systemStatistics($GLOBALS['statsQuery']); $p->next();
        $m = $p->getStatisticsMetadata();
        statisticsThrows(JobU\InvalidCursorException::class, fn() => $p->next());
        return $p->getStatisticsMetadata() === $m && $p->hasNext();
    )PHP");
    proxy.transform_result = {};
}

auto boundary_checks(RecoveryProxy& proxy) -> void
{
    proxy.transform_result = [](std::string const& method, JsonValue& result) {
        if (method == "system.stats") {
            auto& groups = std::get<JsonValue::Array>(std::get<JsonValue::Object>(result.data).at("groups").data);
            if (groups.size() == 2) {
                groups.pop_back();
            }
        }
    };
    run(R"PHP(
        $q = new JobU\StatisticsQuery(queueId: $GLOBALS['statsQueue']->getId(), planned: $GLOBALS['statsWindow'], groupBy: JobU\StatisticsGroupBy::Job, limit: 2);
        $GLOBALS['statsPage'] = $GLOBALS['statsClient']->systemStatistics($q);
        $p = $GLOBALS['statsPage']; $p->next();
        return count($p->items()) === 1 && $p->hasNext();
    )PHP");
    proxy.transform_result = {};
    run("$p = $GLOBALS['statsPage']; $p->next(); return count($p->items()) === 1 && !$p->hasNext();");

    for (auto const* code : {"jobu.response.too_large",
                             "jobu.statistics.invalid_request",
                             "jobu.statistics.cursor_unavailable",
                             "jobu.queue.not_found"}) {
        auto error = jb::core::parse_json(
            R"({"code":-32000,"message":"private diagnostic","data":{"category":"resource_exhausted","code":"placeholder"}})");
        REQUIRE(error);
        auto& data   = std::get<JsonValue::Object>(std::get<JsonValue::Object>(error.value().data).at("data").data);
        data["code"] = JsonValue{.data = std::string(code)};
        proxy.error_method   = "queue.stats";
        proxy.response_error = error.value();
        auto before          = proxy.requests["queue.stats"].size();
        run("$e = statisticsThrows(JobU\\Exception::class, fn() => $GLOBALS['statsQueue']->statistics()); "
            "return $e->getErrorCode() === '" +
            std::string(code) + "' && !str_contains($e->getMessage(), 'private diagnostic');");
        CHECK(proxy.requests["queue.stats"].size() == before + 1);
    }
    proxy.response_error.reset();

    // Cache eviction uses real daemon capacity, with no timer sleeps. The oldest
    // untouched token must be invalid after 1024 subsequent cursor-producing reads.
    run(R"PHP(
        $c = $GLOBALS['statsClient']; $q = $GLOBALS['statsQuery'];
        $p = $c->systemStatistics($q); $p->next(); $old = $p->items(); $m = $p->getStatisticsMetadata();
        for ($i = 0; $i < 1024; ++$i) { $c->systemStatistics($q); }
        statisticsThrows(JobU\InvalidCursorException::class, fn() => $p->next());
        return $p->items() === $old && $p->getStatisticsMetadata() === $m && $p->hasNext();
    )PHP");
}

auto recovery_checks(RecoveryProxy& proxy) -> void
{
    for (auto const* method : {"system.stats", "queue.stats", "system.info"}) {
        proxy.drop_method  = method;
        proxy.drops        = 1;
        auto        before = proxy.requests[method].size();
        auto const* call   = "$GLOBALS['statsClient']->getSystemInfo()";
        if (method == std::string_view("system.stats")) {
            call = "$GLOBALS['statsClient']->systemStatistics()";
        }
        else if (method == std::string_view("queue.stats")) {
            call = "$GLOBALS['statsQueue']->statistics()";
        }
        run(std::string(call) + "; return true;");
        // An explicit system.info retry also needs a reconnect handshake.
        REQUIRE(proxy.requests[method].size() == before + (method == std::string_view("system.info") ? 3 : 2));
        CHECK(proxy.requests[method][before] == proxy.requests[method].back());
    }
    for (auto const* expression : {"$c->getSystemInfo($t)",
                                   "$c->systemStatistics(timeout: $t)",
                                   "$q->statistics(timeout: $t)",
                                   "$p->next($t)"}) {
        run(R"PHP(
            $c = $GLOBALS['statsTimed'] = new JobU\Client(STATISTICS_SOCKET);
            $q = $GLOBALS['statsTimedQueue'] = $c->getQueueById($GLOBALS['statsQueue']->getId());
            $p = $GLOBALS['statsTimedPage'] = $q->statistics($GLOBALS['statsQuery']); $p->next();
            return true;
        )PHP");
        proxy.disconnect();
        proxy.handshake_reply_delay  = std::chrono::milliseconds(180);
        proxy.statistics_reply_delay = std::chrono::milliseconds(180);
        run("$c = $GLOBALS['statsTimed']; $q = $GLOBALS['statsTimedQueue']; $p = $GLOBALS['statsTimedPage']; "
            "$m = $p->getStatisticsMetadata(); $old = $p->items(); $t = JobU\\Duration::milliseconds(300); "
            "statisticsThrows(JobU\\TimeoutException::class, fn() => " +
            std::string(expression) + "); return $p->getStatisticsMetadata() === $m && $p->items() === $old;");
        proxy.handshake_reply_delay  = std::chrono::milliseconds(0);
        proxy.statistics_reply_delay = std::chrono::milliseconds(0);
    }
    run(R"PHP(
        $c = new JobU\Client(STATISTICS_SOCKET);
        $p = $c->systemStatistics($GLOBALS['statsQuery']); $m = $p->getStatisticsMetadata();
        $c->close(); $p->next(); $old = $p->items();
        statisticsThrows(JobU\ConnectionException::class, fn() => $p->next());
        return $p->items() === $old && $p->getStatisticsMetadata() === $m;
    )PHP");
}
} // namespace

auto test_statistics(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy proxy(proxy_endpoint, endpoint);
    zend_register_string_constant("STATISTICS_SOCKET",
                                  sizeof("STATISTICS_SOCKET") - 1,
                                  proxy_endpoint.c_str(),
                                  CONST_CS,
                                  0);
    run("return require '" JOBU_PHP_STATISTICS_SCRIPT "';");
    prepare();
    argument_checks(proxy);
    projection_checks(proxy);
    pagination_checks(proxy);
    boundary_checks(proxy);
    recovery_checks(proxy);
}

auto test_statistics_restart(std::string const&           endpoint,
                             std::string const&           proxy_endpoint,
                             std::function<void()> const& restart) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy proxy(proxy_endpoint, endpoint);
    zend_register_string_constant("STATISTICS_SOCKET",
                                  sizeof("STATISTICS_SOCKET") - 1,
                                  proxy_endpoint.c_str(),
                                  CONST_CS,
                                  0);
    run("return require '" JOBU_PHP_STATISTICS_SCRIPT "';");
    prepare();
    run(R"PHP(
        $GLOBALS['systemPage'] = $GLOBALS['statsClient']->systemStatistics($GLOBALS['statsQuery']);
        $GLOBALS['queuePage'] = $GLOBALS['statsQueue']->statistics($GLOBALS['statsQuery']);
        $GLOBALS['systemPage']->next(); $GLOBALS['queuePage']->next();
        return true;
    )PHP");
    restart();
    auto system_before = proxy.requests["system.stats"].size();
    auto queue_before  = proxy.requests["queue.stats"].size();
    run(R"PHP(
        foreach ([$GLOBALS['systemPage'], $GLOBALS['queuePage']] as $p) {
            $old = $p->items(); $m = $p->getStatisticsMetadata();
            statisticsThrows(JobU\InvalidCursorException::class, fn() => $p->next());
            statisticsCheck($p->items() === $old && $p->getStatisticsMetadata() === $m && $p->hasNext(), 'restart atomicity');
        }
        return true;
    )PHP");
    for (auto const& [method, before] : {
             std::pair{"system.stats", system_before},
             std::pair{"queue.stats",  queue_before }
    }) {
        REQUIRE(proxy.requests[method].size() > before);
        for (auto index = before; index < proxy.requests[method].size(); ++index) {
            CHECK(proxy.requests[method][index].as_object().size() == 1);
            CHECK(proxy.requests[method][index].as_object().contains("cursor"));
        }
    }
}
} // namespace jobu_php_test
