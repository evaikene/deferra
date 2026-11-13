#include "page-bindings.hpp"

#include "client_binding.hpp"
#include "operation.hpp"
#include "recovery-proxy.hpp"
#include "request.hpp"
#include "value_binding.hpp"

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <jb/jobu/history_json.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <string_view>
#include <utility>

namespace jobu_php_test {
namespace {
auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "Page test");
    auto result = php::Value::adopt(slot);
    if (php::has_exception()) {
        zend_exception_error(EG(exception), E_WARNING);
        zend_clear_exception();
        FAIL("Unexpected PHP exception");
    }
    INFO(body);
    REQUIRE(status == SUCCESS);
    REQUIRE(result.view().is_true());
}

auto global(char const* name) -> php::ValueView
{
    return php::ValueView(zend_hash_str_find(&EG(symbol_table), name, std::char_traits<char>::length(name)));
}

auto cursor_only(RecoveryProxy const& proxy, std::string const& method) -> void
{
    auto const& fields = proxy.requests.at(method).back().as_object();
    REQUIRE(fields.size() == 1);
    CHECK(fields.contains("cursor"));
}

auto set_secret(std::string name) -> void
{
    auto client = jb::php::native_client(const_cast<zend_object*>(global("pageClient").get_object()));
    REQUIRE(client);
    auto operation =
        jb::php::Operation::bind("secret.set",
                                 &jb::jobu::ControlClient::set_secret,
                                 jb::jobu::SetSecretRequest{.name = std::move(name), .value = jb::core::ByteBuffer{}});
    REQUIRE(operation);
    auto deadline = client.value()->deadline(jb::core::Clock::now());
    REQUIRE(deadline);
    REQUIRE(client.value()->call(deadline.value(), operation.value()));
}
} // namespace

auto test_pages(std::string const&           endpoint,
                std::string const&           proxy_endpoint,
                std::function<void()> const& restart_daemon) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy proxy(proxy_endpoint, endpoint);
    zend_register_string_constant("PAGE_SOCKET", sizeof("PAGE_SOCKET") - 1, proxy_endpoint.c_str(), CONST_CS, 0);
    run("return require '" JOBU_PHP_PAGE_SCRIPT "';");

    // One exact-limit result is terminal; the largest accepted request is also
    // exercised without synthesizing a full 200-item fixture.
    run(R"PHP(
        foreach ([3, 200] as $limit) {
            $p = JobUTestPages::create($GLOBALS['pageClient'], 'jobs', $limit, $GLOBALS['pageQueueId']);
            $p->next();
            pageCheck(count($p->items()) === 3 && !$p->hasNext(), 'exact or below-limit terminal page');
        }
        return true;
    )PHP");

    // First request is eager; repeated observation and first advancement are local.
    auto before = proxy.requests["queue.list"].size();
    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'queues', 2); return true;");
    CHECK(proxy.requests["queue.list"].size() == before + 1);
    run("$p = $GLOBALS['p']; pageCheck($p->hasNext() && $p->hasNext() && $p->items() === [], 'prefetch'); $p->next(); "
        "return $p->items()[0] instanceof JobU\\Queue;");
    CHECK(proxy.requests["queue.list"].size() == before + 1);
    run("$GLOBALS['p']->next(); return !$GLOBALS['p']->hasNext();");
    auto const& queue_request = proxy.requests["queue.list"].back().as_object();
    CHECK(queue_request.at("limit").as_uint() == 2);
    CHECK(queue_request.at("include_deleted").as_bool());
    CHECK(queue_request.contains("after_id"));
    auto const& job_request = proxy.requests["job.list"][2].as_object();
    CHECK(job_request.at("limit").as_uint() == 2);
    CHECK(job_request.at("type").as_string() == "cli");
    CHECK(job_request.contains("queue_id"));
    CHECK(job_request.contains("after_id"));

    // Secret name keysets retain the requested limit and publish metadata only.
    for (auto const* name : {"page.a", "page.b", "page.c"}) {
        set_secret(name);
    }
    run(R"PHP(
        $p = JobUTestPages::create($GLOBALS['pageClient'], 'secrets', 2);
        $p->next();
        pageCheck($p->items()[0] instanceof JobU\SecretMetadata, 'typed secret');
        $p->next();
        return count($p->items()) === 1 && !$p->hasNext();
    )PHP");
    auto const& secret_request = proxy.requests["secret.list"].back().as_object();
    CHECK(secret_request.at("limit").as_uint() == 2);
    CHECK(secret_request.at("after_name").as_string() == "page.b");

    for (auto const& [kind, method] : {
             std::pair{"runs",        "run.list"    },
             std::pair{"system",      "system.stats"},
             std::pair{"queue-stats", "queue.stats" }
    }) {
        run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], '" + std::string(kind) +
            "', 1, $GLOBALS['pageQueueId']); $GLOBALS['p']->next(); return $GLOBALS['p']->hasNext();");
        run("$GLOBALS['p']->next(); return count($GLOBALS['p']->items()) === 1;");
        cursor_only(proxy, method);
    }

    // GC must see Page's buffered array even before first advancement.
    run(R"PHP(
        foreach ([false, true] as $advance) {
            $client = new JobU\Client(PAGE_SOCKET);
            $p = JobUTestPages::create($client, 'cycle-runs', 1, $GLOBALS['pageQueueId']);
            if ($advance) {
                $p->next();
                $p->items()[0]->page = $p;
            }
            $weakClient = WeakReference::create($client);
            $weakPage = WeakReference::create($p);
            unset($client, $p);
            gc_collect_cycles();
            pageCheck($weakClient->get() === null && $weakPage->get() === null, 'GC ownership');
        }
        return true;
    )PHP");

    // The same opaque cursor is retried after a fully projected reply is rejected.
    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'reject-runs', 1, $GLOBALS['pageQueueId']); "
        "$GLOBALS['p']->next(); return true;");
    run(R"PHP(
        $p = $GLOBALS['p']; $old = $p->items();
        pageThrows(ValueError::class, fn() => $p->next());
        pageCheck($p->items() === $old && $p->hasNext(), 'projection atomicity');
        $p->next(); return $p->items() !== $old;
    )PHP");
    auto const& run_requests = proxy.requests["run.list"];
    CHECK(run_requests[run_requests.size() - 1] == run_requests[run_requests.size() - 2]);

    // A lost read reply reconnects and retries the exact boundary within one call.
    proxy.drop_method = "run.list";
    proxy.drops       = 1;
    before            = proxy.requests["run.list"].size();
    run("$GLOBALS['p']->next(); return !$GLOBALS['p']->hasNext();");
    CHECK(proxy.requests["run.list"].size() == before + 2);
    CHECK(proxy.requests["run.list"][before] == proxy.requests["run.list"][before + 1]);

    // Expiry is injected as a stable daemon error, without a five-minute sleep.
    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'runs', 1, $GLOBALS['pageQueueId']); "
        "$GLOBALS['p']->next(); return true;");
    proxy.error_method = "run.list";
    auto error         = jb::core::parse_json(
        R"({"code":-32000,"message":"Expired cursor","data":{"category":"invalid_argument","code":"jobu.history.invalid_cursor"}})");
    REQUIRE(error);
    proxy.response_error = error.value();
    before               = proxy.requests["run.list"].size();
    run(R"PHP(
        $p = $GLOBALS['p']; $old = $p->items();
        $error = pageThrows(JobU\InvalidCursorException::class, fn() => $p->next());
        return $p->items() === $old && $p->hasNext() && $error->getErrorCode() === 'jobu.history.invalid_cursor';
    )PHP");
    CHECK(proxy.requests["run.list"].size() == before + 1);
    cursor_only(proxy, "run.list");
    proxy.response_error.reset();

    // Decode errors and empty continuation responses are both atomic transitions.
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "run.list") {
            result = jb::core::JsonValue(jb::core::JsonValue::Object{});
        }
    };
    run(R"PHP(
        $p = $GLOBALS['p']; $old = $p->items();
        pageThrows(JobU\Exception::class, fn() => $p->next());
        return $p->items() === $old && $p->hasNext();
    )PHP");
    proxy.transform_result = {};

    // A response budget may produce a short page with a valid continuation.
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "run.list") {
            auto& fields = std::get<jb::core::JsonValue::Object>(result.data);
            auto& items  = std::get<jb::core::JsonValue::Array>(fields.at("items").data);
            if (items.size() == 2) {
                items.pop_back();
            }
        }
    };
    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'runs', 2, $GLOBALS['pageQueueId']); "
        "$GLOBALS['p']->next(); return count($GLOBALS['p']->items()) === 1 && $GLOBALS['p']->hasNext();");
    proxy.transform_result = {};
    run("$GLOBALS['p']->next(); return count($GLOBALS['p']->items()) === 1 && !$GLOBALS['p']->hasNext();");
    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'runs', 1, $GLOBALS['pageQueueId']); "
        "$GLOBALS['p']->next(); return true;");
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "run.list") {
            result = jb::core::JsonValue(jb::core::JsonValue::Object{
                {"items",       jb::core::JsonValue(jb::core::JsonValue::Array{})},
                {"next_cursor", jb::core::JsonValue{}                            }
            });
        }
    };
    run("$GLOBALS['p']->next(); return $GLOBALS['p']->items() === [] && !$GLOBALS['p']->hasNext();");
    proxy.transform_result = {};

    // A separate client's hard timeout must not lose the current page.
    run("$GLOBALS['timed'] = JobUTestPages::create(new JobU\\Client(PAGE_SOCKET), 'runs', 1, $GLOBALS['pageQueueId']); "
        "$GLOBALS['timed']->next(); return true;");
    proxy.silent = true;
    run(R"PHP(
        $p = $GLOBALS['timed']; $old = $p->items();
        pageThrows(JobU\TimeoutException::class, fn() => $p->next(JobU\Duration::milliseconds(25)));
        return $p->items() === $old && $p->hasNext();
    )PHP");
    proxy.silent = false;

    // Failed eager loading never publishes a partially initialized Page.
    proxy.response_error = error.value();
    before               = proxy.requests["run.list"].size();
    run(R"PHP(
        pageThrows(JobU\InvalidCursorException::class, fn() => JobUTestPages::create($GLOBALS['pageClient'], 'runs'));
        return true;
    )PHP");
    CHECK(proxy.requests["run.list"].size() == before + 1);
    proxy.response_error.reset();

    // Attempt continuations use only a cursor, even though the initial request
    // requires run_id. Construct typed fixture replies without executing work.
    run("$p = JobUTestPages::create($GLOBALS['pageClient'], 'runs', 1, $GLOBALS['pageQueueId']); $p->next(); "
        "$GLOBALS['pageRunId'] = $p->items()[0]; return true;");
    auto run_id = jb::php::uuid_from_php(global("pageRunId"));
    REQUIRE(run_id);
    jb::jobu::AttemptPage attempt_page{
        .items       = {{.run_id = run_id.value(), .attempt_number = 2, .due_at = jb::core::UtcClock::now()}},
        .next_cursor = "fixture-attempt"};
    auto attempt_json = jb::jobu::attempt_page_to_json(attempt_page);
    REQUIRE(attempt_json);
    proxy.transform_result = [value = attempt_json.value()](std::string const& method, jb::core::JsonValue& result) {
        if (method == "attempt.list") {
            result = value;
        }
    };
    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'attempts', 1, $GLOBALS['pageRunId']); "
        "$GLOBALS['p']->next(); return $GLOBALS['p']->items() === ['2'];");
    proxy.transform_result = {};
    // The daemon rejects the fixture token; that still proves cursor-only encoding.
    run(R"PHP(
        $p = $GLOBALS['p'];
        pageThrows(JobU\InvalidCursorException::class, fn() => $p->next());
        return $p->items() === ['2'] && $p->hasNext();
    )PHP");
    cursor_only(proxy, "attempt.list");

    run("$GLOBALS['p'] = JobUTestPages::create($GLOBALS['pageClient'], 'runs', 1, $GLOBALS['pageQueueId']); "
        "$GLOBALS['p']->next(); return true;");
    CHECK(proxy.requests["queue.get"].empty());
    CHECK(proxy.requests["job.get"].empty());
    CHECK(proxy.requests["run.get"].empty());
    CHECK(proxy.requests["attempt.get"].empty());

    // Real restart invalidates the server-owned cursor; Page cannot restart it.
    restart_daemon();
    before = proxy.requests["run.list"].size();
    run(R"PHP(
        $p = $GLOBALS['p']; $old = $p->items();
        pageThrows(JobU\InvalidCursorException::class, fn() => $p->next());
        return $p->items() === $old && $p->hasNext();
    )PHP");
    REQUIRE(proxy.requests["run.list"].size() > before);
    for (auto i = before; i < proxy.requests["run.list"].size(); ++i) {
        CHECK(proxy.requests["run.list"][i].as_object().size() == 1);
    }
}
} // namespace jobu_php_test
