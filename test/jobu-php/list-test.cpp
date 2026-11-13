#include "list-test.hpp"

#include "recovery-proxy.hpp"
#include "request.hpp"

#include <phpcxx/zend.hpp> // IWYU pragma: keep Zend declarations must precede zend_exceptions.h

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <cstdint>
#include <string_view>
#include <utility>

namespace jobu_php_test {
namespace {
auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "public list test");
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

auto invalid_arguments(RecoveryProxy& proxy) -> void
{
    auto const before = proxy.requests;
    run(R"PHP(
        $c = $GLOBALS['listClient']; $q = $GLOBALS['listQueue'];
        foreach ([0, -1, 201, PHP_INT_MAX] as $limit) {
            listThrows(ValueError::class, fn() => $c->listQueues(limit: $limit));
            listThrows(ValueError::class, fn() => $c->listJobs(limit: $limit));
            listThrows(ValueError::class, fn() => $q->listJobs(limit: $limit));
        }
        foreach (['', 'bad', '019A0000-0000-7000-8000-000000000001'] as $id) {
            listThrows(ValueError::class, fn() => $c->listJobs(queueId: $id));
        }
        foreach (['', str_repeat('x', 129), "bad\0name", "bad\nname", "\xff", 'name-deleted#' . $q->getId()] as $name) {
            listThrows(ValueError::class, fn() => $c->listJobs(queueName: $name));
        }
        listThrows(ValueError::class, fn() => $c->listJobs(queueName: $q->getName(), queueId: $q->getId()));
        foreach ([
            fn() => $c->listQueues(state: JobU\JobState::Active),
            fn() => $c->listJobs(state: JobU\QueueState::Active),
            fn() => $q->listJobs(type: 'cli'),
            fn() => $c->listJobs(queueName: []),
            fn() => $c->listJobs(queueId: []),
            fn() => $c->listQueues(includeDeleted: []),
            fn() => $q->listJobs(limit: []),
            fn() => $q->listJobs(timeout: 1),
        ] as $call) {
            listThrows(TypeError::class, $call);
        }
        foreach ([JobU\Duration::milliseconds(0), JobU\Duration::seconds(-1)] as $timeout) {
            listThrows(ValueError::class, fn() => $c->listQueues(timeout: $timeout));
            listThrows(ValueError::class, fn() => $c->listJobs(timeout: $timeout));
            listThrows(ValueError::class, fn() => $q->listJobs(timeout: $timeout));
        }
        return true;
    )PHP");
    CHECK(proxy.requests == before);
}

auto prefetch_and_filters(RecoveryProxy& proxy) -> void
{
    // Observations and first next() consume the eager buffer without RPC. Each
    // public entry point also proves there is no preliminary/detail lookup.
    for (auto const& [expression, method] : {
             std::pair{"$GLOBALS['listClient']->listQueues(state: JobU\\QueueState::Active, limit: 1)",                            "queue.list"},
             std::pair{"$GLOBALS['listClient']->listJobs(queueId: $GLOBALS['listQueueId'], state: "
                       "JobU\\JobState::Active, type: JobU\\JobType::Cli, includeDeleted: true, limit: 1)", "job.list"  },
             std::pair{"$GLOBALS['listQueue']->listJobs(state: JobU\\JobState::Active, type: JobU\\JobType::Cli, "
                       "includeDeleted: true, limit: 1)",                                                   "job.list"  }
    }) {
        auto queue_get = proxy.requests["queue.get"].size();
        auto job_get   = proxy.requests["job.get"].size();
        auto before    = proxy.requests[method].size();
        run("$GLOBALS['listing'] = " + std::string(expression) + "; return true;");
        CHECK(proxy.requests[method].size() == before + 1);
        run("$p = $GLOBALS['listing']; listCheck($p->items() === [] && $p->hasNext(), 'before-first'); "
            "$p->next(); return count($p->items()) === 1 && $p->hasNext();");
        CHECK(proxy.requests[method].size() == before + 1);
        auto initial = proxy.requests[method].back().as_object();
        run("$GLOBALS['listing']->next(); return count($GLOBALS['listing']->items()) === 1;");
        auto continuation = proxy.requests[method].back().as_object();
        CHECK(continuation.erase("after_id") == 1);
        CHECK(continuation == initial);
        CHECK(proxy.requests["queue.get"].size() == queue_get);
        CHECK(proxy.requests["job.get"].size() == job_get);
    }

    run(R"PHP(
        $q = $GLOBALS['listQueue'];
        $a = $q->listJobs(limit: 1); $b = $q->listJobs(limit: 2);
        $a->next(); $b->next(); $first = $a->items()[0]->getId();
        $a->next();
        return $b->items()[0]->getId() === $first && $a->items()[0]->getId() !== $first;
    )PHP");
}

auto continuation_failures(RecoveryProxy& proxy) -> void
{
    run("$GLOBALS['listing'] = $GLOBALS['listQueue']->listJobs(limit: 1); $GLOBALS['listing']->next(); return true;");
    proxy.error_method = "job.list";
    auto error         = jb::core::parse_json(
        R"({"code":-32000,"message":"Read failed","data":{"category":"not_found","code":"jobu.queue.not_found"}})");
    REQUIRE(error);
    proxy.response_error = error.value();
    auto before          = proxy.requests["job.list"].size();
    run(R"PHP(
        $p = $GLOBALS['listing']; $old = $p->items();
        listThrows(JobU\NotFoundException::class, fn() => $p->next());
        listThrows(JobU\NotFoundException::class, fn() => $GLOBALS['listQueue']->listJobs());
        return $p->items() === $old && $p->hasNext();
    )PHP");
    CHECK(proxy.requests["job.list"].size() == before + 2);
    auto boundary = proxy.requests["job.list"][before].as_object().at("after_id");
    proxy.response_error.reset();

    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "job.list") {
            result = jb::core::JsonValue{.data = jb::core::JsonValue::Object{}};
        }
    };
    run(R"PHP(
        $p = $GLOBALS['listing']; $old = $p->items();
        listThrows(JobU\Exception::class, fn() => $p->next());
        return $p->items() === $old && $p->hasNext();
    )PHP");
    proxy.transform_result = {};
    run("$GLOBALS['listing']->next(); return true;");
    CHECK(proxy.requests["job.list"].back().as_object().at("after_id") == boundary);

    // Model the wire budget returning fewer items than requested, with the
    // matching exclusive boundary. The next fetch must not skip the removed item.
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "job.list") {
            auto& fields = std::get<jb::core::JsonValue::Object>(result.data);
            auto& items  = std::get<jb::core::JsonValue::Array>(fields.at("items").data);
            if (items.size() > 1) {
                items.resize(1);
                fields.at("next_after_id") = items.front().as_object().at("id");
            }
        }
    };
    run("$GLOBALS['listing'] = $GLOBALS['listQueue']->listJobs(limit: 2); $GLOBALS['listing']->next(); "
        "return count($GLOBALS['listing']->items()) === 1 && $GLOBALS['listing']->hasNext();");
    proxy.transform_result = {};
    run(R"PHP(
        $p = $GLOBALS['listing']; $first = $p->items()[0]->getId();
        $rest = listIds($p);
        return count($rest) === 3 && !in_array($first, $rest, true);
    )PHP");

    // A retryable lost read response keeps the exact keyset request.
    run("$GLOBALS['listing'] = $GLOBALS['listQueue']->listJobs(limit: 1); $GLOBALS['listing']->next(); return true;");
    proxy.drop_method = "job.list";
    proxy.drops       = 1;
    before            = proxy.requests["job.list"].size();
    run("$GLOBALS['listing']->next(); return count($GLOBALS['listing']->items()) === 1;");
    REQUIRE(proxy.requests["job.list"].size() == before + 2);
    CHECK(proxy.requests["job.list"][before] == proxy.requests["job.list"][before + 1]);

    run("$GLOBALS['timedList'] = (new JobU\\Client(LIST_SOCKET))->listJobs(queueId: $GLOBALS['listQueueId'], limit: "
        "1); "
        "$GLOBALS['timedList']->next(); return true;");
    proxy.silent = true;
    run(R"PHP(
        $p = $GLOBALS['timedList']; $old = $p->items();
        listThrows(JobU\TimeoutException::class, fn() => $p->next(JobU\Duration::milliseconds(25)));
        return $p->items() === $old && $p->hasNext();
    )PHP");
    proxy.silent = false;
}
} // namespace

auto test_lists(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy        proxy(proxy_endpoint, endpoint);
    static std::uint32_t sequence = 0;
    auto                 prefix   = "lists-" + std::to_string(++sequence) + ":";
    zend_register_string_constant("LIST_PREFIX", sizeof("LIST_PREFIX") - 1, prefix.c_str(), CONST_CS, 0);
    zend_register_string_constant("LIST_SOCKET", sizeof("LIST_SOCKET") - 1, proxy_endpoint.c_str(), CONST_CS, 0);
    run("return require '" JOBU_PHP_LIST_SCRIPT "';");
    invalid_arguments(proxy);
    prefetch_and_filters(proxy);
    continuation_failures(proxy);
}
} // namespace jobu_php_test
