#include "history-test.hpp"

#include "recovery-proxy.hpp"
#include "request.hpp"

#include <phpcxx/zend.hpp> // IWYU pragma: keep Zend declarations precede zend_exceptions.h

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <cstdint>
#include <string_view>

namespace jobu_php_test {
namespace {
auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "public history test");
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

auto argument_checks(RecoveryProxy& proxy) -> void
{
    auto before = proxy.requests;
    run(R"PHP(
        $c = $GLOBALS['historyClient']; $r = $GLOBALS['historyRun']; $j = $GLOBALS['historyJob']; $q = $GLOBALS['historyQueue'];
        foreach (['', 'bad', '019A0000-0000-7000-8000-000000000001', '00000000-0000-0000-0000-000000000000'] as $id) {
            historyThrows(ValueError::class, fn() => $j->getRun($id));
            historyThrows(ValueError::class, fn() => $q->getJob($id));
        }
        foreach ([0, -1, PHP_INT_MIN] as $number) {
            historyThrows(ValueError::class, fn() => $r->getAttempt($number));
        }
        foreach ([0, -1, 201, PHP_INT_MAX] as $limit) {
            historyThrows(ValueError::class, fn() => $r->listAttempts($limit));
        }
        historyThrows(ValueError::class, fn() => $j->listRuns(new JobU\RunQuery(jobId: $r->getJobId())));
        foreach ([fn() => $j->getRun([]), fn() => $r->getAttempt([]),
                  fn() => $j->listRuns([]), fn() => $r->listAttempts([])] as $call) {
            historyThrows(TypeError::class, $call);
        }
        historyThrows(ValueError::class, fn() => $q->listRuns(new JobU\RunQuery(queueId: $GLOBALS['historyOldQueue']->getId())));
        historyThrows(TypeError::class, fn() => $q->getJob([]));
        $bad = JobU\Duration::milliseconds(0);
        foreach ([fn() => $j->getRun($r->getId(), $bad), fn() => $c->listRuns(timeout: $bad),
                  fn() => $r->getAttempt(1, $bad), fn() => $q->getJob($j->getId(), $bad),
                  fn() => $q->listRuns(timeout: $bad), fn() => $j->listRuns(timeout: $bad), fn() => $r->refresh($bad), fn() => $r->cancel($bad),
                  fn() => $r->listAttempts(timeout: $bad), fn() => $GLOBALS['historySummary']->load($bad),
                  fn() => $GLOBALS['historyAttempt']->refresh($bad)] as $call) {
            historyThrows(ValueError::class, $call);
        }
        return true;
    )PHP");
    CHECK(proxy.requests == before);
    run(R"PHP(
        historyThrows(JobU\NotFoundException::class, fn() => $GLOBALS['historyRun']->getAttempt(PHP_INT_MAX));
        return true;
    )PHP");
    CHECK(proxy.requests["attempt.get"].back().as_object().at("attempt_number").as_uint() == INT64_MAX);
}

auto scoped_reads(RecoveryProxy& proxy) -> void
{
    auto queue_gets = proxy.requests["queue.get"].size();
    auto job_gets   = proxy.requests["job.get"].size();
    auto run_gets   = proxy.requests["run.get"].size();
    run(R"PHP(
        $q = $GLOBALS['historyQueue']; $j = $GLOBALS['historyRetainedJob']; $r = $GLOBALS['historyRun'];
        historyCheck($q->getJob($GLOBALS['historyJob']->getId())->getId() === $GLOBALS['historyJob']->getId(), 'scoped job lookup');
        historyCheck($j->getRun($r->getId())->getId() === $r->getId(), 'retained scoped run');
        historyThrows(JobU\NotFoundException::class, fn() => $GLOBALS['historyOldQueue']->getJob($j->getId()));
        historyThrows(JobU\NotFoundException::class, fn() => $GLOBALS['historyJob']->getRun($r->getId()));
        return true;
    )PHP");
    CHECK(proxy.requests["job.get"].size() == job_gets + 2);
    CHECK(proxy.requests["run.get"].size() == run_gets + 2);
    CHECK(proxy.requests["queue.get"].size() == queue_gets);

    // A queue binds only its own identity; the caller's other filters and limit
    // survive unchanged, and continuation carries only the server cursor.
    auto lists = proxy.requests["run.list"].size();
    run(R"PHP(
        $q = $GLOBALS['historyOldQueue'];
        $query = new JobU\RunQuery(queueId: $q->getId(), jobId: $GLOBALS['historyRun']->getJobId(),
            type: JobU\JobType::Cli, states: [JobU\RunState::Succeeded], limit: 1);
        $GLOBALS['queueHistoryPage'] = $q->listRuns($query);
        return $GLOBALS['queueHistoryPage']->items() === [] && $query->getQueueId() === $q->getId();
    )PHP");
    CHECK(proxy.requests["run.list"].size() == lists + 1);
    auto initial = proxy.requests["run.list"].back().as_object();
    CHECK(initial.contains("queue_id"));
    CHECK(initial.contains("job_id"));
    CHECK(initial.contains("type"));
    CHECK(initial.contains("states"));
    CHECK(initial.at("limit").as_uint() == 1);
    run("$GLOBALS['queueHistoryPage']->next(); return true;");
    CHECK(proxy.requests["run.list"].size() == lists + 1);
    run("$GLOBALS['queueHistoryPage']->next(); return true;");
    CHECK(proxy.requests["run.list"].back().as_object().size() == 1);
    CHECK(proxy.requests["run.list"].back().as_object().contains("cursor"));
    CHECK(proxy.requests["queue.get"].size() == queue_gets);
    CHECK(proxy.requests["job.get"].size() == job_gets + 2);
    CHECK(proxy.requests["run.get"].size() == run_gets + 2);
}

auto reads_and_paging(RecoveryProxy& proxy) -> void
{
    auto attempts = proxy.requests["attempt.get"].size();
    auto runs     = proxy.requests["run.get"].size();
    auto lists    = proxy.requests["attempt.list"].size();
    run(R"PHP(
        $p = $GLOBALS['historyRun']->listAttempts();
        historyCheck($p->items() === [] && $p->hasNext(), 'eager before-first attempts');
        $p->next(); $s = $p->items()[0];
        historyCheck($s->getNumber() === 1 && $s->getOutcome() === JobU\AttemptOutcome::Succeeded, 'cached getters');
        $GLOBALS['scopedAttemptSummary'] = $s;
        return true;
    )PHP");
    CHECK(proxy.requests["attempt.list"].size() == lists + 1);
    CHECK(proxy.requests["attempt.get"].size() == attempts);
    CHECK(proxy.requests["run.get"].size() == runs);
    run("$GLOBALS['scopedAttempt'] = $GLOBALS['scopedAttemptSummary']->load(); return true;");
    CHECK(proxy.requests["attempt.get"].size() == attempts + 1);
    CHECK(proxy.requests["run.get"].size() == runs);
    run("$GLOBALS['scopedAttempt']->refresh(); return $GLOBALS['scopedAttempt']->getResult()->getExitCode() === 0;");
    CHECK(proxy.requests["attempt.get"].size() == attempts + 2);
    CHECK(proxy.requests["run.get"].size() == runs);
    run("$GLOBALS['historyRun']->getAttempt(number: 1); return true;");
    CHECK(proxy.requests["attempt.get"].size() == attempts + 3);
    CHECK(proxy.requests["run.get"].size() == runs);

    // Initial filters and page limit stay inside the daemon cursor. A failed
    // continuation must leave the same current objects and boundary available.
    run(R"PHP(
        $r = $GLOBALS['historyRun'];
        $GLOBALS['historyPage'] = $GLOBALS['historyClient']->listRuns(new JobU\RunQuery(
            jobId: $r->getJobId(), states: [JobU\RunState::Succeeded, JobU\RunState::Cancelled],
            type: JobU\JobType::Cli, limit: 1));
        $GLOBALS['historyPage']->next(); return true;
    )PHP");
    auto initial = proxy.requests["run.list"].back().as_object();
    CHECK(initial.contains("states"));
    CHECK(initial.at("limit").as_uint() == 1);
    proxy.error_method = "run.list";
    auto error         = jb::core::parse_json(
        R"({"code":-32000,"message":"Expired","data":{"category":"invalid_argument","code":"jobu.history.invalid_cursor"}})");
    REQUIRE(error);
    proxy.response_error = error.value();
    run(R"PHP(
        $p = $GLOBALS['historyPage']; $old = $p->items();
        historyThrows(JobU\InvalidCursorException::class, fn() => $p->next());
        return $p->hasNext() && $p->items() === $old;
    )PHP");
    auto continuation = proxy.requests["run.list"].back();
    CHECK(continuation.as_object().size() == 1);
    CHECK(continuation.as_object().contains("cursor"));
    proxy.response_error.reset();
    run("$GLOBALS['historyPage']->next(); return true;");
    CHECK(proxy.requests["run.list"].back() == continuation);

    // Simulate a short wire-budget page without changing its valid cursor.
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "run.list") {
            auto& items = std::get<jb::core::JsonValue::Array>(
                std::get<jb::core::JsonValue::Object>(result.data).at("items").data);
            if (items.size() > 1) {
                items.resize(1);
            }
        }
    };
    run(R"PHP(
        $p = $GLOBALS['historyClient']->listRuns(new JobU\RunQuery(jobId: $GLOBALS['historyRun']->getJobId(), limit: 2));
        $p->next(); return count($p->items()) === 1 && $p->hasNext();
    )PHP");
    proxy.transform_result = {};
}

auto attempt_cursors_and_deadlines(RecoveryProxy& proxy) -> void
{
    auto runs = proxy.requests["run.get"].size();
    auto gets = proxy.requests["attempt.get"].size();
    run(R"PHP(
        $p = $GLOBALS['historyRetryRun']->listAttempts(limit: 1);
        $p->next(); historyCheck($p->items()[0]->getNumber() === 3, 'largest number first');
        $p->next(); historyCheck($p->items()[0]->getNumber() === 2, 'next attempt number');
        $GLOBALS['retryAttemptSummary'] = $p->items()[0];
        return true;
    )PHP");
    CHECK(proxy.requests["attempt.list"].back().as_object().size() == 1);
    CHECK(proxy.requests["attempt.list"].back().as_object().contains("cursor"));
    CHECK(proxy.requests["run.get"].size() == runs);
    CHECK(proxy.requests["attempt.get"].size() == gets);
    run("$GLOBALS['retryAttemptSummary']->load(); return true;");
    CHECK(proxy.requests["attempt.get"].size() == gets + 1);
    CHECK(proxy.requests["run.get"].size() == runs);

    // The lookup uses the Run's context and observes the override without any
    // preliminary parent request or subsequent metadata read.
    run(R"PHP(
        $c = new JobU\Client(HISTORY_SOCKET);
        $p = $c->listRuns(new JobU\RunQuery(jobId: $GLOBALS['historyRun']->getJobId(), states: [JobU\RunState::Succeeded]));
        $p->next(); $GLOBALS['deadlineRun'] = $p->items()[0]->load(); return true;
    )PHP");
    runs                      = proxy.requests["run.get"].size();
    proxy.history_reply_delay = std::chrono::milliseconds(200);
    run(R"PHP(
        historyThrows(JobU\TimeoutException::class, fn() => $GLOBALS['deadlineRun']->getAttempt(1, JobU\Duration::milliseconds(50)));
        return true;
    )PHP");
    proxy.history_reply_delay = std::chrono::milliseconds(0);
    CHECK(proxy.requests["attempt.get"].size() == gets + 2);
    CHECK(proxy.requests["run.get"].size() == runs);
}

auto result_and_failure_checks(RecoveryProxy& proxy) -> void
{
    // Runner selection comes from trusted run metadata, even when safe result
    // fields are unknown, misleading, or entirely absent.
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "attempt.get") {
            auto extra = jb::core::parse_json(
                R"({"type":"http","exit_code":0,"future":{"zero":0,"false":false,"text":"a\u0000b"}})");
            REQUIRE(extra);
            std::get<jb::core::JsonValue::Object>(result.data).at("result") = extra.value();
        }
    };
    run(R"PHP(
        $a = $GLOBALS['scopedAttempt']; $a->refresh(); $r = $a->getResult();
        return $r instanceof JobU\CliExecutionResult && $r->getExitCode() === 0 &&
            $r->get('future')->text === "a\0b" && $r->get('future')->false === false;
    )PHP");
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "attempt.get") {
            std::get<jb::core::JsonValue::Object>(result.data).at("result") = jb::core::JsonValue{};
        }
    };
    run("$GLOBALS['scopedAttempt']->refresh(); return $GLOBALS['scopedAttempt']->getResult() === null;");
    proxy.transform_result = {};
    run("$GLOBALS['scopedAttempt']->refresh(); return true;");

    for (auto const& method : {"run.get", "attempt.get"}) {
        proxy.transform_result = [method](std::string const& name, jb::core::JsonValue& result) {
            if (name == method) {
                std::get<jb::core::JsonValue::Object>(result.data).erase("state");
            }
        };
        run(method == std::string_view("run.get") ? R"PHP(
            $r = $GLOBALS['historyRun']; $old = $r->getState();
            historyThrows(JobU\Exception::class, fn() => $r->refresh());
            historyThrows(JobU\Exception::class, fn() => $GLOBALS['historySummary']->load());
            return $r->getState() === $old && $r->getResult()->getExitCode() === 0;
        )PHP"
                                                  : R"PHP(
            $a = $GLOBALS['scopedAttempt']; $old = $a->getState();
            historyThrows(JobU\Exception::class, fn() => $a->refresh());
            return $a->getState() === $old && $a->getResult()->getExitCode() === 0;
        )PHP");
    }
    proxy.transform_result = {};

    // A failed detail read must not alter its originating summary.
    proxy.error_method = "attempt.get";
    auto error         = jb::core::parse_json(
        R"({"code":-32000,"message":"Gone","data":{"category":"not_found","code":"jobu.attempt.not_found"}})");
    REQUIRE(error);
    proxy.response_error = error.value();
    run(R"PHP(
        $s = $GLOBALS['scopedAttemptSummary'];
        historyThrows(JobU\NotFoundException::class, fn() => $s->load());
        return $s->getNumber() === 1;
    )PHP");
    proxy.response_error.reset();

    run(R"PHP(
        $q = $GLOBALS['historyQueue'];
        $j = $q->createCliJob(new JobU\CliPayload('/bin/true'), new JobU\OnceSchedule(new DateTimeImmutable('2040-01-01Z')));
        $GLOBALS['cancelRun'] = historyItems($j->listRuns())[0]->load(); return true;
    )PHP");
    proxy.drop_method = "run.cancel";
    proxy.drops       = 1;
    auto before       = proxy.requests["run.cancel"].size();
    run(R"PHP(
        $r = $GLOBALS['cancelRun'];
        $e = historyThrows(JobU\OutcomeUnknownException::class, fn() => $r->cancel());
        historyCheck($e->getOperation() === 'run.cancel' && $r->getState() === JobU\RunState::Scheduled, 'uncertain cache');
        $r->refresh(); return $r->getState() === JobU\RunState::Cancelled;
    )PHP");
    CHECK(proxy.requests["run.cancel"].size() == before + 1);
}
} // namespace

auto test_history(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy        proxy(proxy_endpoint, endpoint);
    static std::uint32_t sequence = 0;
    auto                 prefix   = "history-" + std::to_string(++sequence) + ":";
    zend_register_string_constant("HISTORY_PREFIX", sizeof("HISTORY_PREFIX") - 1, prefix.c_str(), CONST_CS, 0);
    zend_register_string_constant("HISTORY_SOCKET", sizeof("HISTORY_SOCKET") - 1, proxy_endpoint.c_str(), CONST_CS, 0);
    run("return require '" JOBU_PHP_HISTORY_SCRIPT "';");
    argument_checks(proxy);
    scoped_reads(proxy);
    reads_and_paging(proxy);
    result_and_failure_checks(proxy);
    attempt_cursors_and_deadlines(proxy);
}
} // namespace jobu_php_test
