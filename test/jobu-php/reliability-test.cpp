#include "reliability-test.hpp"

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
using Fault = RecoveryProxy::ReplyFault;

auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "reliability test");
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

auto wire_checks(RecoveryProxy& proxy) -> void
{
    for (auto fault : {Fault::TruncatedHeader,
                       Fault::TruncatedBody,
                       Fault::InvalidHeader,
                       Fault::InvalidJson,
                       Fault::CompleteThenEof}) {
        INFO(static_cast<int>(fault));
        bool const invalid  = fault == Fault::InvalidHeader || fault == Fault::InvalidJson;
        bool const complete = fault == Fault::CompleteThenEof;
        run(R"PHP(
            $c = new JobU\Client(RELIABILITY_SOCKET);
            $q = $c->createQueue(RELIABILITY_PREFIX . ++$GLOBALS['reliabilitySequence']);
            $GLOBALS['reliabilityClient'] = $c;
            $GLOBALS['reliabilityQueue'] = $q;
            return true;
        )PHP");

        // Reads can replay after incomplete EOF. A malformed frame must instead
        // surface its decoder error, rather than silently consume a retry.
        auto before        = proxy.requests["queue.get"].size();
        auto injected      = proxy.faults_injected;
        proxy.fault_method = "queue.get";
        proxy.reply_fault  = fault;
        run(invalid ? R"PHP(
            try { $GLOBALS['reliabilityClient']->getQueue($GLOBALS['reliabilityQueue']->getName()); return false; }
            catch (JobU\Exception $e) {
                $GLOBALS['wireError'] = $e->getErrorCode();
                return !($e instanceof JobU\OutcomeUnknownException) && !str_contains((string)$e, 'PRIVATE-wire-marker');
            }
        )PHP"
                    : R"PHP(
            return $GLOBALS['reliabilityClient']->getQueue($GLOBALS['reliabilityQueue']->getName())->getId()
                === $GLOBALS['reliabilityQueue']->getId();
        )PHP");
        CHECK(proxy.faults_injected == injected + 1);
        CHECK(proxy.requests["queue.get"].size() == before + (invalid || complete ? 1 : 2));
        if (invalid) {
            run(fault == Fault::InvalidHeader ? "return $GLOBALS['wireError'] === 'rpc.framing.invalid_content_length';"
                                              : "return $GLOBALS['wireError'] === 'core.json.syntax';");
        }

        // Reuse the same client after the failed read. The keyed create either
        // replays exactly, returns its complete reply, or exposes its original key.
        run(R"PHP(
            $GLOBALS['createdName'] = RELIABILITY_PREFIX . ++$GLOBALS['reliabilitySequence'];
            $GLOBALS['createdKey'] = $GLOBALS['createdName'] . '-key';
            return $GLOBALS['reliabilityClient']->getSystemInfo() instanceof JobU\SystemInfo;
        )PHP");
        before             = proxy.requests["queue.create"].size();
        proxy.fault_method = "queue.create";
        proxy.reply_fault  = fault;
        run(invalid ? R"PHP(
            try {
                $GLOBALS['reliabilityClient']->createQueue($GLOBALS['createdName'], idempotencyKey: $GLOBALS['createdKey']);
                return false;
            } catch (JobU\OutcomeUnknownException $e) {
                return $e->getOperation() === 'queue.create' && $e->getIdempotencyKey() === $GLOBALS['createdKey'];
            }
        )PHP"
                    : R"PHP(
            $q = $GLOBALS['reliabilityClient']->createQueue($GLOBALS['createdName'], idempotencyKey: $GLOBALS['createdKey']);
            return $q->getRevision() === 1;
        )PHP");
        REQUIRE(proxy.requests["queue.create"].size() == before + (invalid || complete ? 1 : 2));
        if (!invalid && !complete) {
            CHECK(proxy.requests["queue.create"][before] == proxy.requests["queue.create"][before + 1]);
        }
        run(R"PHP(
            $peer = new JobU\Client(RELIABILITY_DIRECT_SOCKET);
            $q = $peer->getQueue($GLOBALS['createdName']);
            $again = $GLOBALS['reliabilityClient']->createQueue($GLOBALS['createdName'], idempotencyKey: $GLOBALS['createdKey']);
            return $q->getId() === $again->getId() && $again->getRevision() === 1;
        )PHP");

        before             = proxy.requests["queue.update"].size();
        proxy.fault_method = "queue.update";
        proxy.reply_fault  = fault;
        run(complete ? R"PHP(
            $q = $GLOBALS['reliabilityQueue']; $q->setWeight(9); $q->save();
            return !$q->isDirty() && $q->getRevision() === 2;
        )PHP"
                     : R"PHP(
            $q = $GLOBALS['reliabilityQueue']; $q->setWeight(9);
            try { $q->save(); return false; }
            catch (JobU\OutcomeUnknownException $e) {
                return $e->getOperation() === 'queue.update' && $e->getIdempotencyKey() === null
                    && $q->isDirty() && $q->getRevision() === 1 && $q->getWeight() === 9;
            }
        )PHP");
        CHECK(proxy.requests["queue.update"].size() == before + 1);
        CHECK(proxy.faults_injected == injected + 3);
        run(R"PHP(
            $q = $GLOBALS['reliabilityQueue'];
            $peer = new JobU\Client(RELIABILITY_DIRECT_SOCKET);
            $stored = $peer->getQueueById($q->getId());
            $q->discardChanges(); $q->refresh();
            return $stored->getWeight() === 9 && $stored->getRevision() === 2 && $q->getRevision() === 2;
        )PHP");
    }
}

auto disclosure_checks(RecoveryProxy& proxy) -> void
{
    // Only direct extension frames promise SensitiveParameter redaction. Do not
    // wrap setSecret in a PHP callable that independently retains its arguments.
    for (auto fault : {Fault::TruncatedBody, Fault::InvalidHeader, Fault::InvalidJson}) {
        proxy.fault_method = "secret.set";
        proxy.reply_fault  = fault;
        auto before        = proxy.requests["secret.set"].size();
        run(R"PHP(
            ini_set('zend.exception_ignore_args', '0');
            $c = $GLOBALS['reliabilityClient'];
            try { $c->setSecret('reliability.secret', 'PRIVATE-secret-marker'); return false; }
            catch (JobU\OutcomeUnknownException $e) {
                if ($e->getOperation() !== 'secret.set' || $e->getIdempotencyKey() !== null
                    || !($e->getTrace()[0]['args'][1] instanceof SensitiveParameterValue)) return false;
                $p = $c->listSecrets(); $p->next();
                ob_start(); var_dump($e, $c, $p->items()); $dump = ob_get_clean();
                return !str_contains((string)$e . $dump, 'PRIVATE-secret-marker')
                    && !str_contains((string)$e . $dump, 'PRIVATE-wire-marker');
            }
        )PHP");
        CHECK(proxy.requests["secret.set"].size() == before + 1);
    }

    proxy.error_method = "secret.set";
    auto error         = jb::core::parse_json(
        R"({"code":-32000,"message":"PRIVATE-backend-marker","data":{"code":"jobu.test.denied","category":"internal","detail":"PRIVATE-backend-marker"}})");
    REQUIRE(error);
    proxy.response_error = std::move(error).value();
    auto before          = proxy.requests["secret.set"].size();
    run(R"PHP(
        try { $GLOBALS['reliabilityClient']->setSecret('reliability.secret', 'PRIVATE-secret-marker'); return false; }
        catch (JobU\Exception $e) {
            ob_start(); var_dump($e); $dump = ob_get_clean();
            return $e->getErrorCode() === 'jobu.test.denied' && $e->getCategory() === 'internal'
                && $e->getCode() === -32000 && !str_contains((string)$e . $dump, 'PRIVATE-secret-marker')
                && !str_contains((string)$e . $dump, 'PRIVATE-backend-marker');
        }
    )PHP");
    CHECK(proxy.requests["secret.set"].size() == before + 1);
    proxy.response_error.reset();
}

auto page_deadline_checks(RecoveryProxy& proxy) -> void
{
    run(R"PHP(
        $c = new JobU\Client(RELIABILITY_SOCKET);
        foreach (['reliability.a', 'reliability.b', 'reliability.c'] as $name) $c->setSecret($name, 'x');
        $p = $c->listSecrets(limit: 1); $p->next();
        $GLOBALS['deadlineClient'] = $c; $GLOBALS['deadlinePage'] = $p;
        $GLOBALS['deadlineItems'] = $p->items();
        return $p->hasNext();
    )PHP");
    proxy.disconnect();
    proxy.handshake_reply_delay      = std::chrono::milliseconds(180);
    proxy.administration_reply_delay = std::chrono::milliseconds(180);
    auto before                      = proxy.requests["secret.list"].size();
    auto retired                     = proxy.delayed_replies_retired;
    run(R"PHP(
        $p = $GLOBALS['deadlinePage'];
        try { $p->next(JobU\Duration::milliseconds(300)); return false; }
        catch (JobU\TimeoutException $e) {
            return $p->items() === $GLOBALS['deadlineItems'] && $p->hasNext();
        }
    )PHP");
    CHECK(proxy.requests["secret.list"].size() == before + 1);
    proxy.handshake_reply_delay      = std::chrono::milliseconds(0);
    proxy.administration_reply_delay = std::chrono::milliseconds(0);

    // Drain the already-scheduled late response while the failed Page survives.
    // Its retired observer must neither publish items nor make the Client usable.
    REQUIRE(jb::php::current_request()->loop()->run_until(jb::core::Clock::now() + std::chrono::seconds(2), [&] {
        return proxy.delayed_replies_retired == retired + 2;
    }));
    run(R"PHP(
        $p = $GLOBALS['deadlinePage'];
        try { $p->next(); return false; }
        catch (JobU\ConnectionException $e) {
            if ($e->getErrorCode() !== 'php.client.unusable') return false;
        }
        $GLOBALS['deadlineClient']->close();
        try { $p->next(); return false; }
        catch (JobU\ConnectionException $e) {
            return $e->getErrorCode() === 'php.client.closed'
                && $p->items() === $GLOBALS['deadlineItems'] && $p->hasNext();
        }
    )PHP");
    CHECK(proxy.requests["secret.list"].size() == before + 1);
}

auto shutdown_checks(RecoveryProxy& proxy) -> void
{
    proxy.fault_method = "system.info";
    proxy.reply_fault  = Fault::InvalidHeader;
    run(R"PHP(
        try { new JobU\Client(RELIABILITY_SOCKET); return false; }
        catch (JobU\Exception $e) {}
        $c = new JobU\Client(RELIABILITY_SOCKET);
        $q = $c->getQueue($GLOBALS['reliabilityQueue']->getName());
        $view = $q->defaults()->cli();
        $weak = WeakReference::create($c);
        unset($c, $q); gc_collect_cycles();
        if ($weak->get() === null) return false;
        unset($view); gc_collect_cycles();
        if ($weak->get() !== null) return false;
        // Keep a fresh peer and the failed Page/resources alive until RSHUTDOWN.
        $GLOBALS['shutdownPeer'] = new JobU\Client(RELIABILITY_DIRECT_SOCKET);
        return $GLOBALS['shutdownPeer']->getSystemInfo() instanceof JobU\SystemInfo;
    )PHP");
}
} // namespace

auto test_reliability(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy   proxy(proxy_endpoint, endpoint);
    static unsigned sequence = 0;
    auto            prefix   = "reliability-" + std::to_string(++sequence) + "-";
    zend_register_string_constant("RELIABILITY_SOCKET",
                                  sizeof("RELIABILITY_SOCKET") - 1,
                                  proxy_endpoint.c_str(),
                                  CONST_CS,
                                  0);
    zend_register_string_constant("RELIABILITY_DIRECT_SOCKET",
                                  sizeof("RELIABILITY_DIRECT_SOCKET") - 1,
                                  endpoint.c_str(),
                                  CONST_CS,
                                  0);
    zend_register_string_constant("RELIABILITY_PREFIX", sizeof("RELIABILITY_PREFIX") - 1, prefix.c_str(), CONST_CS, 0);
    run("$GLOBALS['reliabilitySequence'] = 0; return true;");
    wire_checks(proxy);
    disclosure_checks(proxy);
    page_deadline_checks(proxy);
    shutdown_checks(proxy);
}
} // namespace jobu_php_test
