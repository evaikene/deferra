#include "resource-bindings.hpp"

#include "attribute_binding.hpp"
#include "cli_payload_binding.hpp"
#include "client_binding.hpp"
#include "http_payload_binding.hpp"
#include "recovery-proxy.hpp"
#include "request.hpp"
#include "resource_state.hpp"
#include "value_binding.hpp"

#include <Zend/zend_exceptions.h>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jobu_php_test {
namespace {

auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "resource state test");
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

auto test_job_snapshot() -> void
{
    using Job   = jb::jobu::JobDefinition;
    auto native = jb::php::native_client(Z_OBJ_P(global("c").get()));
    REQUIRE(native);
    auto queue_id = jb::php::uuid_from_php(global("id"));
    REQUIRE(queue_id);
    auto payload = jb::core::parse_json(R"({"command":"/bin/true"})");
    REQUIRE(payload);
    auto operation = jb::php::Operation::bind(
        "job.create",
        &jb::jobu::ControlClient::create_job,
        jb::jobu::CreateJobRequest{
            .queue    = queue_id.value(),
            .schedule = jb::jobu::OnceSchedule{.planned_at = jb::core::UtcClock::now() + std::chrono::hours(1)},
            .payload  = payload.value()});
    REQUIRE(operation);
    auto deadline = native.value()->deadline(jb::core::Clock::now());
    REQUIRE(deadline);
    auto reply = native.value()->call(deadline.value(), operation.value());
    REQUIRE(reply);
    auto* job = std::get_if<Job>(&reply.value());
    REQUIRE(job);
    jb::php::ResourceState<Job> state(php::Value::retain(global("c")), *job);
    REQUIRE(state.set("priority", &Job::priority, std::int32_t{7}));

    // Supplied-key merges retain omitted attributes; only net changed keys enter
    // the update patch, including after a user changes one key back to baseline.
    auto attributes                   = state.current().attributes;
    attributes.at("job.timeout").data = std::chrono::duration_cast<jb::core::Duration>(std::chrono::seconds(8));
    REQUIRE(jb::php::update_job_attributes(state, attributes));
    CHECK(state.edits().contains("attributes.job.timeout"));
    REQUIRE(jb::php::update_job_attributes(state, state.baseline().attributes));
    CHECK_FALSE(state.edits().contains("attributes.job.timeout"));
    CHECK(state.edits().contains("priority"));
    REQUIRE(jb::php::update_job_attributes(state, attributes));
    attributes.erase("job.timeout");
    REQUIRE_FALSE(jb::php::update_job_attributes(state, attributes));
    CHECK(state.edits().contains("attributes.job.timeout"));

    auto make = [](Job const& current, Job const& baseline, auto const& edits) -> php::Result<jb::php::Operation> {
        jb::jobu::UpdateJobRequest request{.job_id = baseline.id, .expected_revision = baseline.revision};
        if (edits.contains("priority")) {
            request.priority = current.priority;
        }
        auto changes = jb::php::changed_attributes(baseline.attributes, current.attributes);
        REQUIRE(changes);
        CHECK(changes.value().size() == 1);
        request.attribute_changes = std::move(changes).value();
        auto result = jb::php::Operation::bind("job.update", &jb::jobu::ControlClient::update_job, std::move(request));
        REQUIRE(result);
        return std::move(result).value();
    };
    auto decode = [](jb::jobu::ControlReply const& result) -> php::Result<Job> {
        auto const* snapshot = std::get_if<Job>(&result);
        REQUIRE(snapshot);
        return *snapshot;
    };
    auto prepare = [](Job const&) -> php::Result<jb::php::ConfigurationRestore> {
        return jb::php::ConfigurationRestore{};
    };
    REQUIRE(state.save(deadline.value(), make, decode, prepare));
    CHECK_FALSE(state.dirty());
    CHECK(state.baseline().revision == job->revision + 1);
    CHECK(state.current().priority == 7);
    auto accepted = state.baseline();
    auto invalid  = accepted;
    invalid.id    = {};
    REQUIRE_FALSE(state.accept(invalid, prepare));
    invalid          = accepted;
    invalid.revision = 0;
    REQUIRE_FALSE(state.accept(invalid, prepare));
    invalid      = accepted;
    invalid.type = jb::jobu::JobType::Http;
    REQUIRE_FALSE(state.accept(invalid, prepare));
    CHECK(state.baseline().revision == accepted.revision);
    CHECK(state.baseline().id == accepted.id);
    REQUIRE(state.set("priority", &Job::priority, std::int32_t{-2}));
    REQUIRE(state.discard(prepare));
    CHECK(state.current().priority == 7);
    CHECK_FALSE(state.dirty());
}

auto test_payload_restoration() -> void
{
    run(R"PHP(
        $GLOBALS['cli'] = new JobU\CliPayload('/bin/echo original');
        $GLOBALS['cliSource'] = new JobU\CliPayload('/bin/true replacement');
        $GLOBALS['http'] = new JobU\HttpPayload('https://example.test/old');
        $GLOBALS['httpSource'] = new JobU\HttpPayload('https://example.test/new', 'POST');
        $GLOBALS['httpSource']->setBody("a\0b");
        $GLOBALS['badSource'] = new JobU\CliPayload('echo');
        return true;
    )PHP");
    auto owner         = global("r");
    int  notifications = 0;
    bool deleted       = false;
    auto check         = [&] { return jb::php::resource_guard(false, deleted || global("blockEdits").is_true()); };
    REQUIRE(jb::php::attach_cli_payload(Z_OBJ_P(global("cli").get()), owner, [&] { ++notifications; }, check));
    REQUIRE(jb::php::attach_http_payload(Z_OBJ_P(global("http").get()), owner, [&] { ++notifications; }, check));
    auto* cli   = Z_OBJ_P(global("cli").get());
    auto* http  = Z_OBJ_P(global("http").get());
    auto  first = jb::php::prepare_cli_payload_restore(cli, global("cliSource"));
    REQUIRE(first);
    auto bad = jb::php::prepare_cli_payload_restore(cli, global("badSource"));
    REQUIRE_FALSE(bad); // Bare executable has no explicit PATH; conversion must fail atomically.
    auto second = jb::php::prepare_http_payload_restore(http, global("httpSource"));
    REQUIRE(second);
    first.value().append(std::move(second).value());
    auto cli_before  = jb::php::cli_payload_from_php(global("cli"));
    auto http_before = jb::php::http_payload_from_php(global("http"));
    REQUIRE(cli_before);
    REQUIRE(http_before);
    CHECK(cli_before.value().as_object().at("command").as_string() == "/bin/echo");
    CHECK(http_before.value().as_object().at("url").as_string() == "https://example.test/old");
    std::move(first).value().commit();
    CHECK(notifications == 0);
    CHECK(Z_OBJ_P(global("cli").get()) == cli);
    CHECK(Z_OBJ_P(global("http").get()) == http);
    run(R"PHP(
        if ($GLOBALS['cli']->getExecutable() !== '/bin/true'
            || $GLOBALS['http']->getBody() !== "a\0b") return false;
        $GLOBALS['cli']->setWorkingDirectory('/tmp');
        $GLOBALS['http']->setUrl('https://example.test/edited');
        return true;
    )PHP");
    CHECK(notifications == 2);
    run(R"PHP(
        $GLOBALS['blockEdits'] = false;
        $text = new class {
            public function __toString(): string {
                $GLOBALS['blockEdits'] = true;
                return '/bin/false';
            }
        };
        try { $GLOBALS['cli']->setExecutable($text); return false; }
        catch (JobU\NotFoundException $e) {}
        $GLOBALS['blockEdits'] = false;
        $body = new class implements JsonSerializable {
            public function jsonSerialize(): mixed {
                $GLOBALS['blockEdits'] = true;
                return ['ok' => true];
            }
        };
        try { $GLOBALS['http']->setJsonBody($body); return false; }
        catch (JobU\NotFoundException $e) {}
        $GLOBALS['blockEdits'] = false;
        return $GLOBALS['cli']->getExecutable() === '/bin/true'
            && $GLOBALS['http']->getBody() === "a\0b";
    )PHP");
    CHECK(notifications == 2);
    deleted = true;
    run(R"PHP(
        foreach ([$GLOBALS['cli'], $GLOBALS['http']] as $payload) {
            try {
                if ($payload instanceof JobU\CliPayload) $payload->setExecutable('/bin/false');
                else $payload->setUrl('https://example.test/deleted');
                return false;
            } catch (JobU\NotFoundException $e) {}
        }
        $copy = clone $GLOBALS['cli'];
        $copy->setExecutable('/bin/false');
        return $GLOBALS['cli']->getExecutable() === '/bin/true';
    )PHP");
    CHECK(notifications == 2);
    // Release every closure borrowing these stack counters before returning.
    run(R"PHP(unset($GLOBALS['cli'], $GLOBALS['cliSource'], $GLOBALS['http'],
        $GLOBALS['httpSource'], $GLOBALS['badSource']); gc_collect_cycles(); return true;)PHP");
}

} // namespace

auto test_resource_state(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy        proxy(proxy_endpoint, endpoint);
    static std::uint32_t sequence = 0;
    auto                 prefix   = std::to_string(++sequence) + ":";
    zend_register_string_constant("RESOURCE_PREFIX", sizeof("RESOURCE_PREFIX") - 1, prefix.c_str(), CONST_CS, 0);
    zend_register_string_constant("RESOURCE_SOCKET",
                                  sizeof("RESOURCE_SOCKET") - 1,
                                  proxy_endpoint.c_str(),
                                  CONST_CS,
                                  0);
    run(R"PHP(
        $GLOBALS['c'] = new JobU\Client(RESOURCE_SOCKET);
        $GLOBALS['id'] = JobUTestBridge::createQueue($GLOBALS['c'], RESOURCE_PREFIX . 'resource-initial');
        $GLOBALS['r'] = new JobUTestResource($GLOBALS['c'], $GLOBALS['id']);
        $GLOBALS['peer'] = new JobUTestResource($GLOBALS['c'], $GLOBALS['id']);
        $GLOBALS['defaults'] = $GLOBALS['r']->defaults();
        $GLOBALS['view'] = $GLOBALS['defaults']->cli();
        $GLOBALS['httpView'] = $GLOBALS['defaults']->http();
        return !$GLOBALS['r']->dirty() && $GLOBALS['r']->revision() === 1;
    )PHP");
    auto reads = proxy.requests["queue.get"].size();
    run(R"PHP(
        $r = $GLOBALS['r'];
        $r->setName('changed');
        if (!$r->dirty()) return false;
        foreach (['refresh', 'lifecycle'] as $method) {
            try { $r->$method(); return false; } catch (JobU\UnsavedChangesException $e) {}
        }
        $r->setName(RESOURCE_PREFIX . 'resource-initial');
        if ($r->dirty()) return false;
        $r->save();
        $GLOBALS['view']->setTimeout(JobU\Duration::seconds(2));
        if (!$r->dirty()) return false;
        $GLOBALS['defaults']->clear('job.timeout');
        if ($r->dirty()) return false;
        $GLOBALS['view']->setTimeout(JobU\Duration::seconds(3));
        $r->discardChanges();
        return !$r->dirty() && !$GLOBALS['defaults']->has('job.timeout')
            && $r->defaults() === $GLOBALS['defaults'] && $r->defaults()->cli() === $GLOBALS['view']
            && $r->defaults()->http() === $GLOBALS['httpView'] && $r->notifications() === 3;
    )PHP");
    CHECK(proxy.requests["queue.get"].size() == reads);
    CHECK(proxy.requests["queue.update"].empty());

    run(R"PHP(
        $r = $GLOBALS['r'];
        $GLOBALS['view']->setTimeout(JobU\Duration::seconds(4));
        $r->setName(RESOURCE_PREFIX . 'resource-saved');
        $r->save();
        return !$r->dirty() && $r->revision() === 2 && $r->notifications() === 4
            && $r->defaults() === $GLOBALS['defaults'] && $r->defaults()->cli() === $GLOBALS['view'];
    )PHP");
    REQUIRE(proxy.requests["queue.update"].size() == 1);
    auto const& saved = proxy.requests["queue.update"].back().as_object();
    CHECK(saved.contains("name"));
    CHECK(saved.contains("defaults"));
    CHECK_FALSE(saved.contains("weight"));
    CHECK_FALSE(saved.contains("concurrency_limit"));
    run(R"PHP(
        $peer = $GLOBALS['peer'];
        $peer->setName('conflicting-local');
        try { $peer->save(); return false; } catch (JobU\RevisionConflictException $e) {}
        if (!$peer->dirty() || $peer->revision() !== 1 || $peer->name() !== 'conflicting-local') return false;
        $peer->discardChanges();
        if ($peer->name() !== RESOURCE_PREFIX . 'resource-initial') return false;
        $peer->refresh();
        return $peer->name() === RESOURCE_PREFIX . 'resource-saved' && $peer->revision() === 2;
    )PHP");

    // A successful server commit followed by local conversion failure cannot partially replace the cache.
    run(R"PHP(
        $r = $GLOBALS['r'];
        $r->setName(RESOURCE_PREFIX . 'conversion-committed');
        $r->rejectNextSnapshot();
        try { $r->save(); return false; } catch (ValueError $e) {}
        if (!$r->dirty() || $r->revision() !== 2 || $r->name() !== RESOURCE_PREFIX . 'conversion-committed') return false;
        $r->discardChanges();
        $r->rejectNextSnapshot();
        try { $r->refresh(); return false; } catch (ValueError $e) {}
        if ($r->name() !== RESOURCE_PREFIX . 'resource-saved' || $r->revision() !== 2) return false;
        $r->refresh();
        return $r->name() === RESOURCE_PREFIX . 'conversion-committed' && $r->revision() === 3;
    )PHP");
    proxy.drop_method = "queue.update";
    proxy.drops       = 1;
    auto writes       = proxy.requests["queue.update"].size();
    run(R"PHP(
        $r = $GLOBALS['r'];
        $r->setName(RESOURCE_PREFIX . 'uncertain-committed');
        $GLOBALS['view']->setTimeout(JobU\Duration::seconds(5));
        try { $r->save(); return false; } catch (JobU\OutcomeUnknownException $e) {
            if ($e->getOperation() !== 'queue.update') return false;
        }
        return $r->dirty() && $r->revision() === 3 && $r->name() === RESOURCE_PREFIX . 'uncertain-committed'
            && $GLOBALS['view']->getTimeout() == JobU\Duration::seconds(5);
    )PHP");
    CHECK(proxy.requests["queue.update"].size() == writes + 1);
    run(R"PHP(
        $r = $GLOBALS['r'];
        $r->discardChanges();
        if ($r->name() !== RESOURCE_PREFIX . 'conversion-committed'
            || $GLOBALS['view']->getTimeout() != JobU\Duration::seconds(4)) return false;
        $r->refresh();
        return $r->name() === RESOURCE_PREFIX . 'uncertain-committed' && $r->revision() === 4
            && $r->defaults()->cli() === $GLOBALS['view'];
    )PHP");

    test_job_snapshot();
    test_payload_restoration();
    reads = proxy.requests["queue.get"].size();
    run(R"PHP(
        $GLOBALS['c']->close();
        $r = $GLOBALS['r'];
        $r->save();
        $r->setName('closed-local');
        try { $r->save(); return false; } catch (JobU\ConnectionException $e) {}
        if (!$r->dirty() || $r->revision() !== 4) return false;
        $r->discardChanges();
        try { $r->refresh(); return false; } catch (JobU\ConnectionException $e) {}
        $r->markDeleted();
        foreach (['save', 'lifecycle'] as $method) {
            try { $r->$method(); return false; } catch (JobU\NotFoundException $e) {}
        }
        try { $GLOBALS['view']->setTimeout(JobU\Duration::seconds(9)); return false; }
        catch (JobU\NotFoundException $e) {}
        try { $GLOBALS['defaults']->clear('job.timeout'); return false; }
        catch (JobU\NotFoundException $e) {}
        try { $x = clone $r; return false; } catch (Error $e) {}
        try { serialize($r); return false; } catch (Exception $e) {}
        return $r->name() === RESOURCE_PREFIX . 'uncertain-committed' && $r->revision() === 4
            && $GLOBALS['view']->getTimeout() == JobU\Duration::seconds(5);
    )PHP");
    CHECK(proxy.requests["queue.get"].size() == reads);
    run(R"PHP(
        $client = WeakReference::create($GLOBALS['c']);
        $resource = WeakReference::create($GLOBALS['r']);
        $GLOBALS['r']->reference = $GLOBALS['r'];
        unset($GLOBALS['c'], $GLOBALS['peer'], $GLOBALS['r']);
        gc_collect_cycles();
        if ($client->get() === null || $resource->get() === null) return false;
        unset($GLOBALS['defaults'], $GLOBALS['view'], $GLOBALS['httpView']);
        gc_collect_cycles();
        return $client->get() === null && $resource->get() === null;
    )PHP");
}

} // namespace jobu_php_test
