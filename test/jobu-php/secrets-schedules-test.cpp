#include "secrets-schedules-test.hpp"

#include "recovery-proxy.hpp"
#include "request.hpp"

#include <phpcxx/zend.hpp> // IWYU pragma: keep Zend declarations precede zend_exceptions.h

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <chrono>
#include <string_view>

namespace jobu_php_test {
namespace {
auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "public secret/schedule test");
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
        $c = $GLOBALS['adminClient']; $s = new JobU\CronSchedule('@daily'); $a = new DateTimeImmutable('2030-01-01Z');
        foreach (['', 'Bad', 'a..b', 'a.1b', str_repeat('a', 129), "a\0b", "\xff"] as $name) {
            adminThrows(ValueError::class, fn() => $c->setSecret($name, 'private'));
            adminThrows(ValueError::class, fn() => $c->deleteSecret($name));
        }
        adminThrows(ValueError::class, fn() => $c->setSecret('admin.large', str_repeat('x', 65537)));
        foreach ([0, -1, 201, PHP_INT_MAX] as $n) {
            adminThrows(ValueError::class, fn() => $c->listSecrets($n));
            adminThrows(ValueError::class, fn() => $c->nextScheduleOccurrences($s, $a, $n));
        }
        foreach ([fn() => $c->setSecret([], 'x'), fn() => $c->setSecret('admin.x', []),
                  fn() => $c->deleteSecret([]), fn() => $c->listSecrets([]),
                  fn() => $c->validateSchedule(JobU\OnceSchedule::now()),
                  fn() => $c->nextScheduleOccurrences(JobU\OnceSchedule::now(), $a),
                  fn() => $c->nextScheduleOccurrences($s, 'date'),
                  fn() => $c->nextScheduleOccurrences($s, $a, [])] as $call) {
            adminThrows(TypeError::class, $call);
        }
        foreach ([JobU\Duration::milliseconds(0), JobU\Duration::seconds(-1)] as $timeout) {
            foreach ([fn() => $c->setSecret('admin.x', 'x', $timeout), fn() => $c->listSecrets(timeout: $timeout),
                      fn() => $c->deleteSecret('admin.x', $timeout), fn() => $c->validateSchedule($s, $timeout),
                      fn() => $c->nextScheduleOccurrences($s, $a, timeout: $timeout)] as $call) {
                adminThrows(ValueError::class, $call);
            }
        }
        $badDate = new class extends DateTimeImmutable { public function __construct() {} };
        adminThrows(ValueError::class, fn() => $c->nextScheduleOccurrences($s, $badDate));
        adminThrows(ValueError::class, fn() => $c->nextScheduleOccurrences($s, new DateTimeImmutable('9999-01-01Z')));
        return true;
    )PHP");
    CHECK(proxy.requests == before);
}

auto wire_checks(RecoveryProxy& proxy) -> void
{
    // These are synthetic fixture bytes. Only the request codec sees them; all
    // daemon results remain metadata, including the binary and empty cases.
    auto const& sets = proxy.requests["secret.set"];
    REQUIRE(sets.size() >= 4);
    CHECK(sets[0].as_object().at("value").as_object().at("encoding").as_string() == "base64");
    CHECK(sets[2].as_object().at("value").as_object().at("data").as_string().empty());
    auto const& previews = proxy.requests["schedule.next"];
    REQUIRE_FALSE(previews.empty());
    CHECK(previews.front().as_object().at("after").as_string() == "2029-12-31T22:00:00.123456Z");
    CHECK(previews.front().as_object().at("count").as_uint() == 5);
    CHECK(previews.front().as_object().at("schedule").as_object().at("timezone").as_string() == "UTC");
    bool omitted = false;
    for (auto const& preview : previews) {
        omitted = omitted || !preview.as_object().at("schedule").as_object().contains("timezone");
    }
    CHECK(omitted);

    run(R"PHP(
        $c = $GLOBALS['adminClient'];
        $name = 'proxy.original';
        $value = new class($name) {
            public function __construct(public mixed &$name) {}
            public function __toString(): string { $this->name = 'proxy.changed'; return "binary\0value"; }
        };
        $m = $c->setSecret($name, $value);
        adminCheck($name === 'proxy.changed' && $m->getName() === 'proxy.original', 'owned name across coercion');
        $c->deleteSecret('proxy.original');
        return true;
    )PHP");
}

auto pagination_checks(RecoveryProxy& proxy) -> void
{
    run(R"PHP(
        $c = $GLOBALS['adminClient'];
        $c->setSecret('proxy.a', 'a'); $c->setSecret('proxy.b', 'b');
        $GLOBALS['adminPage'] = $c->listSecrets(limit: 1);
        return true;
    )PHP");
    auto before = proxy.requests["secret.list"].size();
    run("$p = $GLOBALS['adminPage']; adminCheck($p->hasNext() && $p->items() === [], 'prefetch'); "
        "$p->next(); return $p->items()[0]->getName() === 'proxy.a';");
    CHECK(proxy.requests["secret.list"].size() == before);
    CHECK_FALSE(proxy.requests["secret.list"].back().as_object().contains("after_name"));

    auto error = jb::core::parse_json(
        R"({"code":-32000,"message":"private backend detail","data":{"category":"internal","code":"jobu.secret.read_failed"}})");
    REQUIRE(error);
    proxy.error_method   = "secret.list";
    proxy.response_error = error.value();
    run(R"PHP(
        $p = $GLOBALS['adminPage']; $old = $p->items();
        $error = adminThrows(JobU\Exception::class, fn() => $p->next());
        adminCheck(!str_contains($error->getMessage(), 'private backend detail'), 'safe remote diagnostics');
        return $p->items() === $old && $p->hasNext();
    )PHP");
    proxy.response_error.reset();
    run("$GLOBALS['adminPage']->next(); return $GLOBALS['adminPage']->items()[0]->getName() === 'proxy.b';");
    auto fields = proxy.requests["secret.list"].back().as_object();
    CHECK(fields.at("after_name").as_string() == "proxy.a");
    CHECK(fields.at("limit").as_uint() == 1);
    run("$GLOBALS['adminClient']->deleteSecret('proxy.a'); $GLOBALS['adminClient']->deleteSecret('proxy.b'); return "
        "true;");
}

auto recovery_checks(RecoveryProxy& proxy) -> void
{
    for (auto const* method : {"secret.set", "secret.delete"}) {
        proxy.drop_method = method;
        proxy.drops       = 1;
        auto before       = proxy.requests[method].size();
        auto connections  = proxy.connections;
        run(method == std::string_view("secret.set") ? R"PHP(
            $e = adminThrows(JobU\OutcomeUnknownException::class, fn() => $GLOBALS['adminClient']->setSecret('proxy.uncertain', 'private-marker'));
            return $e->getOperation() === 'secret.set' && !str_contains($e->getMessage(), 'private-marker');
        )PHP"
                                                     : R"PHP(
            $e = adminThrows(JobU\OutcomeUnknownException::class, fn() => $GLOBALS['adminClient']->deleteSecret('proxy.uncertain'));
            return $e->getOperation() === 'secret.delete';
        )PHP");
        CHECK(proxy.requests[method].size() == before + 1);
        CHECK(proxy.connections == connections);
        // An independent metadata read proves the mutation committed and permits
        // reconnect; the mutation itself never reconciles or replays.
        run(method == std::string_view("secret.set") ? "$p = $GLOBALS['adminClient']->listSecrets(); $p->next(); "
                                                       "return $p->items()[0]->getName() === 'proxy.uncertain';"
                                                     : "return !$GLOBALS['adminClient']->listSecrets()->hasNext();");
        CHECK(proxy.connections == connections + 1);
    }
    for (auto const* method : {"secret.list", "schedule.validate", "schedule.next"}) {
        proxy.drop_method = method;
        proxy.drops       = 1;
        auto before       = proxy.requests[method].size();
        if (method == std::string_view("secret.list")) {
            run("return !$GLOBALS['adminClient']->listSecrets()->hasNext();");
        }
        else if (method == std::string_view("schedule.validate")) {
            run("$GLOBALS['adminClient']->validateSchedule(new JobU\\CronSchedule('@daily')); return true;");
        }
        else {
            run("return count($GLOBALS['adminClient']->nextScheduleOccurrences(new JobU\\CronSchedule('@daily'), "
                "new DateTimeImmutable('2030-01-01Z'))) === 5;");
        }
        REQUIRE(proxy.requests[method].size() == before + 2);
        CHECK(proxy.requests[method][before] == proxy.requests[method][before + 1]);
    }
}

auto deadline_checks(RecoveryProxy& proxy) -> void
{
    // Reconnect handshake and read each fit the timeout separately, but together
    // exceed it. Timers exercise the shared budget without blocking the loop.
    for (auto const* expression : {"$c->listSecrets(timeout: $t)",
                                   "$c->validateSchedule(new JobU\\CronSchedule('@daily'), $t)",
                                   "$c->nextScheduleOccurrences(new JobU\\CronSchedule('@daily'), new "
                                   "DateTimeImmutable('2030-01-01Z'), timeout: $t)"}) {
        run("$GLOBALS['adminTimed'] = new JobU\\Client(ADMIN_SOCKET); return true;");
        proxy.disconnect();
        proxy.handshake_reply_delay      = std::chrono::milliseconds(180);
        proxy.administration_reply_delay = std::chrono::milliseconds(180);
        run("$c = $GLOBALS['adminTimed']; $t = JobU\\Duration::milliseconds(300); "
            "adminThrows(JobU\\TimeoutException::class, fn() => " +
            std::string(expression) + "); return true;");
        proxy.handshake_reply_delay      = std::chrono::milliseconds(0);
        proxy.administration_reply_delay = std::chrono::milliseconds(0);
    }
}
} // namespace

auto test_secrets_schedules(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy proxy(proxy_endpoint, endpoint);
    zend_register_string_constant("ADMIN_SOCKET", sizeof("ADMIN_SOCKET") - 1, proxy_endpoint.c_str(), CONST_CS, 0);
    run("return require '" JOBU_PHP_ADMIN_SCRIPT "';");
    run("$GLOBALS['adminClient'] = new JobU\\Client(ADMIN_SOCKET); return true;");
    argument_checks(proxy);
    wire_checks(proxy);
    pagination_checks(proxy);
    recovery_checks(proxy);
    deadline_checks(proxy);
}
} // namespace jobu_php_test
