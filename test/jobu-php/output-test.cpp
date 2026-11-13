#include "output-test.hpp"

#include "recovery-proxy.hpp"
#include "request.hpp"

#include <phpcxx/zend.hpp> // IWYU pragma: keep Zend declarations precede zend_exceptions.h

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace jobu_php_test {
namespace {
auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "public output test");
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
        $a = $GLOBALS['outputAttempt']; $r = $GLOBALS['outputRun']; $h = $GLOBALS['outputHttp'];
        foreach ([0, -1, 65537, PHP_INT_MAX] as $n) {
            outputThrows(ValueError::class, fn() => $a->getOutput($n));
            outputThrows(ValueError::class, fn() => $r->getOutput($n));
            outputThrows(ValueError::class, fn() => $a->readOutput(JobU\OutputChannel::Stdout, length: $n));
        }
        foreach ([-1, PHP_INT_MIN, PHP_INT_MAX] as $n) {
            outputThrows(ValueError::class, fn() => $a->readOutput(JobU\OutputChannel::Stdout, offset: $n));
        }
        foreach ([JobU\OutputChannel::Body, JobU\OutputChannel::Headers] as $ch) {
            outputThrows(ValueError::class, fn() => $a->readOutput($ch));
        }
        foreach ([JobU\OutputChannel::Stdout, JobU\OutputChannel::Stderr] as $ch) {
            outputThrows(ValueError::class, fn() => $h->readOutput($ch));
        }
        foreach ([fn() => $a->getOutput([]), fn() => $r->getOutput([]), fn() => $a->readOutput('stdout'),
                  fn() => $a->readOutput(JobU\OutputChannel::Stdout, []),
                  fn() => $a->readOutput(JobU\OutputChannel::Stdout, length: [])] as $call) {
            outputThrows(TypeError::class, $call);
        }
        $bad = JobU\Duration::milliseconds(0);
        foreach ([fn() => $a->getOutput(timeout: $bad), fn() => $r->getOutput(timeout: $bad),
                  fn() => $a->readOutput(JobU\OutputChannel::Stdout, timeout: $bad)] as $call) {
            outputThrows(ValueError::class, $call);
        }
        return true;
    )PHP");
    CHECK(proxy.requests == before);
    run(R"PHP(
        outputThrows(JobU\Exception::class, fn() => $GLOBALS['outputAttempt']->readOutput(JobU\OutputChannel::Stdout, PHP_INT_MAX - 1));
        return true;
    )PHP");
    CHECK(proxy.requests["attempt.output"].back().as_object().at("offset").as_uint() == INT64_MAX - 1);
}

auto request_checks(RecoveryProxy& proxy) -> void
{
    auto details = proxy.requests["attempt.get"].size();
    auto runs    = proxy.requests["run.get"].size();
    auto lists   = proxy.requests["attempt.list"].size();
    auto reads   = proxy.requests["attempt.output"].size();
    run(R"PHP(
        return $GLOBALS['outputAttempt']->readOutput(JobU\OutputChannel::Stderr)->getData() === "err\0é";
    )PHP");
    REQUIRE(proxy.requests["attempt.output"].size() == reads + 1);
    auto request = proxy.requests["attempt.output"].back().as_object();
    CHECK(request.at("offset").as_uint() == 0);
    CHECK(request.at("limit").as_uint() == 16384);

    reads = proxy.requests["attempt.output"].size();
    run("return $GLOBALS['outputAttempt']->getOutput(2) === 'é';");
    REQUIRE(proxy.requests["attempt.output"].size() == reads + 2);
    CHECK(proxy.requests["attempt.output"][reads].as_object().at("limit").as_uint() == 1);
    CHECK(proxy.requests["attempt.output"].back().as_object().at("offset").as_uint() == 4);
    CHECK(proxy.requests["attempt.output"].back().as_object().at("limit").as_uint() == 2);
    CHECK(proxy.requests["attempt.list"].size() == lists);

    reads = proxy.requests["attempt.output"].size();
    run("return $GLOBALS['outputRetry']->getOutput() === 'retry';");
    CHECK(proxy.requests["attempt.list"].size() == lists + 1);
    CHECK(proxy.requests["attempt.list"].back().as_object().at("limit").as_uint() == 1);
    REQUIRE(proxy.requests["attempt.output"].size() == reads + 3);
    for (auto i = reads; i < reads + 3; ++i) {
        CHECK(proxy.requests["attempt.output"][i].as_object().at("attempt_number").as_uint() == 3);
    }
    reads = proxy.requests["attempt.output"].size();
    run("return $GLOBALS['outputZero']->getOutput() === null;");
    CHECK(proxy.requests["attempt.list"].size() == lists + 2);
    CHECK(proxy.requests["attempt.output"].size() == reads);
    CHECK(proxy.requests["attempt.get"].size() == details);
    CHECK(proxy.requests["run.get"].size() == runs);
}

auto capture_checks(RecoveryProxy& proxy) -> void
{
    // Explicitly injected loss models recovery evidence; the stock daemon's
    // ordinary completed fixture still supplies the actual binary slices.
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "attempt.output") {
            auto& fields            = std::get<jb::core::JsonValue::Object>(result.data);
            fields["status"]        = {.data = std::string("lost")};
            fields["capture_lost"]  = {.data = true};
            fields["total_bytes"]   = {};
            fields["omitted_bytes"] = {};
        }
    };
    run(R"PHP(
        $a = $GLOBALS['outputAttempt']; $chunk = $a->readOutput(JobU\OutputChannel::Stderr);
        outputCheck($chunk->getStatus() === JobU\OutputStatus::Lost && $chunk->isCaptureLost(), 'loss evidence');
        outputCheck($chunk->getTotalBytes() === null && $chunk->getOmittedBytes() === null, 'unknown counts');
        outputCheck($a->getOutput() === "err\0é", 'lost bytes remain useful');
        return $GLOBALS['outputEmpty']->getOutput() === null;
    )PHP");
    proxy.transform_result = [](std::string const& method, jb::core::JsonValue& result) {
        if (method == "attempt.output") {
            auto& fields = std::get<jb::core::JsonValue::Object>(result.data);
            if (fields.at("channel").as_string() == "stdout") {
                fields["status"] = {.data = std::string("not_captured")};
            }
        }
    };
    run("return $GLOBALS['outputEmpty']->getOutput() === '';");
    proxy.transform_result = {};
}

auto failure_checks(RecoveryProxy& proxy) -> void
{
    // Lose the tail reply after its probe succeeded. Only that frozen read is
    // replayed; the resource cache and attempt selection are left untouched.
    proxy.drop_method      = "attempt.output";
    proxy.transform_result = [&proxy](std::string const& method, jb::core::JsonValue& result) {
        if (method == "attempt.output" && result.as_object().at("offset").as_uint() == 0) {
            proxy.drops = 1;
        }
    };
    auto reads       = proxy.requests["attempt.output"].size();
    auto connections = proxy.connections;
    run(R"PHP(
        $a = $GLOBALS['outputAttempt']; $old = $a->getResult();
        return $a->getOutput(2) === 'é' && $a->getResult() == $old;
    )PHP");
    REQUIRE(proxy.requests["attempt.output"].size() == reads + 3);
    CHECK(proxy.requests["attempt.output"][reads + 1] == proxy.requests["attempt.output"][reads + 2]);
    CHECK(proxy.connections == connections + 1);
    proxy.transform_result = {};

    // Retention can delete the selected attempt between probe and tail. Do not
    // hide its not-found error, restart enumeration, or return probe bytes.
    auto gone = jb::core::parse_json(
        R"({"code":-32000,"message":"Gone","data":{"category":"not_found","code":"jobu.attempt.not_found"}})");
    REQUIRE(gone);
    proxy.error_method     = "attempt.output";
    proxy.transform_result = [&proxy, &gone](std::string const& method, jb::core::JsonValue&) {
        if (method == "attempt.output") {
            proxy.response_error = gone.value();
        }
    };
    reads = proxy.requests["attempt.output"].size();
    run(R"PHP(
        $a = $GLOBALS['outputAttempt']; $state = $a->getState();
        outputThrows(JobU\NotFoundException::class, fn() => $a->getOutput());
        return $a->getState() === $state;
    )PHP");
    CHECK(proxy.requests["attempt.output"].size() == reads + 2);
    proxy.response_error.reset();
    proxy.transform_result = {};

    for (auto const& field : {"attempt_number", "channel", "offset"}) {
        proxy.transform_result = [field](std::string const& method, jb::core::JsonValue& result) {
            if (method == "attempt.output") {
                auto& fields = std::get<jb::core::JsonValue::Object>(result.data);
                if (field == std::string_view("channel")) {
                    fields[field] = {.data = std::string("stdout")};
                }
                else {
                    fields[field] = {.data = std::uint64_t{2}};
                    if (field == std::string_view("offset")) {
                        fields["next_offset"] = {.data = std::uint64_t{3}};
                    }
                }
            }
        };
        run(R"PHP(
            outputThrows(ValueError::class, fn() => $GLOBALS['outputAttempt']->readOutput(JobU\OutputChannel::Stderr, length: 1));
            return true;
        )PHP");
    }
    proxy.transform_result = {};
}

auto deadline_checks(RecoveryProxy& proxy) -> void
{
    // Isolate each timeout on a fresh Client because deadline exhaustion makes
    // that Client terminal. Delayed replies use event-loop timers, not sleeps.
    for (auto const& target : {"run", "attempt"}) {
        run(R"PHP(
            $c = new JobU\Client(OUTPUT_SOCKET);
            $p = $c->listRuns(new JobU\RunQuery(jobId: $GLOBALS['outputRun']->getJobId(), states: [JobU\RunState::Succeeded])); $p->next();
            $GLOBALS['outputDeadlineRun'] = $p->items()[0]->load();
            $GLOBALS['outputDeadlineAttempt'] = $GLOBALS['outputDeadlineRun']->getAttempt(1);
            return true;
        )PHP");
        auto reads               = proxy.requests["attempt.output"].size();
        proxy.output_reply_delay = std::chrono::milliseconds(200);
        run(target == std::string_view("run") ? R"PHP(
            outputThrows(JobU\TimeoutException::class, fn() => $GLOBALS['outputDeadlineRun']->getOutput(timeout: JobU\Duration::milliseconds(300)));
            return true;
        )PHP"
                                              : R"PHP(
            outputThrows(JobU\TimeoutException::class, fn() => $GLOBALS['outputDeadlineAttempt']->getOutput(timeout: JobU\Duration::milliseconds(300)));
            return true;
        )PHP");
        CHECK(proxy.requests["attempt.output"].size() == reads + (target == std::string_view("run") ? 1 : 2));
        proxy.output_reply_delay = std::chrono::milliseconds(0);
    }
}
} // namespace

auto test_output(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy        proxy(proxy_endpoint, endpoint);
    static std::uint32_t sequence = 0;
    auto                 prefix   = "output-" + std::to_string(++sequence) + ":";
    zend_register_string_constant("OUTPUT_PREFIX", sizeof("OUTPUT_PREFIX") - 1, prefix.c_str(), CONST_CS, 0);
    zend_register_string_constant("OUTPUT_SOCKET", sizeof("OUTPUT_SOCKET") - 1, proxy_endpoint.c_str(), CONST_CS, 0);
    zend_register_string_constant("OUTPUT_PHP_BINARY", sizeof("OUTPUT_PHP_BINARY") - 1, JOBU_PHP_BINARY, CONST_CS, 0);
    run("return require '" JOBU_PHP_OUTPUT_SCRIPT "';");
    argument_checks(proxy);
    request_checks(proxy);
    capture_checks(proxy);
    failure_checks(proxy);
    deadline_checks(proxy);
}
} // namespace jobu_php_test
