#include "queue-test.hpp"

#include "client_binding.hpp"
#include "query_binding.hpp"
#include "recovery-proxy.hpp"
#include "request.hpp"
#include "resource_state.hpp"
#include "value_binding.hpp"

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>
#include <jb/jobu/statistics_json.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/value.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <variant>

namespace jobu_php_test {
namespace {
auto run(std::string_view body) -> void
{
    auto expression = "(function () { " + std::string(body) + " })()";
    zval slot;
    ZVAL_UNDEF(&slot);
    auto status = zend_eval_stringl(expression.data(), expression.size(), &slot, "production queue test");
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

// Stage 4.2 tests the actual native statistics bridge without publishing a
// temporary PHP result shape. Page and typed statistics remain stages 4.4/5.4.
auto statistics(RecoveryProxy& proxy) -> void
{
    run(R"PHP(
        $GLOBALS['queueId'] = $GLOBALS['queueResource']->getId();
        $GLOBALS['statisticsQuery'] = new JobU\StatisticsQuery(planned: new JobU\TimeRange(new DateTimeImmutable('-1 day'), new DateTimeImmutable('+2 hours')), groupBy: JobU\StatisticsGroupBy::State, limit: 1);
        return true;
    )PHP");
    auto id = jb::php::uuid_from_php(global("queueId"));
    REQUIRE(id);
    auto client = jb::php::native_client(Z_OBJ_P(global("queueClient").get()));
    REQUIRE(client);
    auto deadline = client.value()->deadline(jb::core::Clock::now());
    REQUIRE(deadline);
    auto query = jb::php::statistics_query_from_php(global("statisticsQuery"));
    REQUIRE(query);
    jb::jobu::QueueStatisticsListRequest request =
        jb::jobu::QueueStatisticsQuery{.selector = id.value(), .statistics = query.value()};
    auto call = jb::php::Operation::bind("queue.stats", &jb::jobu::ControlClient::queue_statistics, request);
    REQUIRE(call);
    auto reply = jb::php::resource_call(global("queueClient"), deadline.value(), call.value());
    REQUIRE(reply);
    auto* page = std::get_if<jb::jobu::StatisticsPage>(&reply.value());
    REQUIRE(page);
    REQUIRE(proxy.requests["queue.stats"].back().as_object().contains("queue_id"));

    // Empty queues legitimately have no groups. Seed retained work through the
    // existing native SDK (public PHP job creation belongs to stage 4.3).
    auto payload = jb::core::parse_json(R"({"command":"/bin/true"})");
    REQUIRE(payload);
    for (int i = 0; i != 2; ++i) {
        auto job = jb::php::Operation::bind(
            "job.create",
            &jb::jobu::ControlClient::create_job,
            jb::jobu::CreateJobRequest{
                .queue    = id.value(),
                .schedule = jb::jobu::OnceSchedule{.planned_at = jb::core::UtcClock::now() + std::chrono::hours(1)},
                .payload  = payload.value()});
        REQUIRE(job);
        REQUIRE(client.value()->call(deadline.value(), job.value()));
    }
    query.value().group_by = jb::jobu::StatisticsGroupBy::Job;
    request                = jb::jobu::QueueStatisticsQuery{.selector = id.value(), .statistics = query.value()};
    call = jb::php::Operation::bind("queue.stats", &jb::jobu::ControlClient::queue_statistics, request);
    REQUIRE(call);
    reply = jb::php::resource_call(global("queueClient"), deadline.value(), call.value());
    REQUIRE(reply);
    page = std::get_if<jb::jobu::StatisticsPage>(&reply.value());
    REQUIRE(page);
    REQUIRE(page->groups.size() == 1);
    REQUIRE(page->next_cursor);
    request = jb::jobu::CursorRequest{*page->next_cursor};
    call    = jb::php::Operation::bind("queue.stats", &jb::jobu::ControlClient::queue_statistics, request);
    REQUIRE(call);
    REQUIRE(jb::php::resource_call(global("queueClient"), deadline.value(), call.value()));
    auto const& continuation = proxy.requests["queue.stats"].back().as_object();
    REQUIRE(continuation.size() == 1);
    CHECK(continuation.contains("cursor"));

    // The bridge preserves the cursor error and never restarts the query.
    // This native entry has no PHP frame; exception translation is covered by
    // the existing bridge tests, rather than raising outside a Zend call.
    request = jb::jobu::CursorRequest{"invalid-cursor"};
    call    = jb::php::Operation::bind("queue.stats", &jb::jobu::ControlClient::queue_statistics, request);
    REQUIRE(call);
    auto requests = proxy.requests["queue.stats"].size();
    auto failed   = client.value()->call(deadline.value(), call.value());
    REQUIRE_FALSE(failed);
    CHECK(jb::php::failure_metadata(failed.error()).code == "jobu.statistics.invalid_cursor");
    CHECK(proxy.requests["queue.stats"].size() == requests + 1);
}
} // namespace

auto test_queues(std::string const& endpoint, std::string const& proxy_endpoint) -> void
{
    auto anchor = jb::php::current_request()->create_session();
    REQUIRE(anchor);
    RecoveryProxy        proxy(proxy_endpoint, endpoint);
    static std::uint32_t sequence = 0;
    auto                 prefix   = std::to_string(++sequence) + ":";
    zend_register_string_constant("QUEUE_PREFIX", sizeof("QUEUE_PREFIX") - 1, prefix.c_str(), CONST_CS, 0);
    zend_register_string_constant("QUEUE_SOCKET", sizeof("QUEUE_SOCKET") - 1, proxy_endpoint.c_str(), CONST_CS, 0);
    run("return require '" JOBU_PHP_QUEUE_SCRIPT "';");

    // Recorded updates prove identity pinning, omission and explicit-null semantics.
    auto const& updates = proxy.requests["queue.update"];
    REQUIRE(!updates.empty());
    auto const& first = updates.front().as_object();
    CHECK(first.contains("queue_id"));
    CHECK_FALSE(first.contains("queue_name"));
    CHECK(first.at("expected_revision").as_uint() == 1);
    CHECK(first.at("history_retention_seconds").is_null());
    CHECK(first.at("defaults").is_object());
    auto before = proxy.requests["queue.update"].size();
    run("$GLOBALS['queueResource']->save(); return true;");
    CHECK(proxy.requests["queue.update"].size() == before);

    proxy.drop_method = "queue.create";
    proxy.drops       = 1;
    run(R"PHP(
        $c = $GLOBALS['queueClient'];
        $q = $c->createQueue(QUEUE_PREFIX . 'replayed');
        queueCheck($q->getRevision() === 1, 'lost create response');
        $key = QUEUE_PREFIX . 'explicit-key';
        $a = $c->createQueue(QUEUE_PREFIX . 'keyed', idempotencyKey: $key);
        $a->setWeight(2); $a->save();
        $b = $c->createQueue(QUEUE_PREFIX . 'keyed', idempotencyKey: $key);
        queueCheck($a->getId() === $b->getId() && $b->getRevision() === 1 && $b->getWeight() === 1, 'creation replay snapshot');
        queueThrows(JobU\Exception::class, fn() => $c->createQueue(QUEUE_PREFIX . 'different', idempotencyKey: $key));
        return true;
    )PHP");
    auto const& creates  = proxy.requests["queue.create"];
    bool        replayed = false;
    for (std::size_t i = 1; i < creates.size(); ++i) {
        if (creates[i - 1].as_object().at("name").as_string() == prefix + "replayed") {
            CHECK(jb::core::serialize_json(creates[i - 1]).value() == jb::core::serialize_json(creates[i]).value());
            replayed = true;
            break;
        }
    }
    CHECK(replayed);

    proxy.drop_method = "queue.update";
    proxy.drops       = 1;
    before            = proxy.requests["queue.update"].size();
    run(R"PHP(
        $q = $GLOBALS['queueResource']; $revision = $q->getRevision();
        $q->setWeight(8); $q->defaults()->cli()->setTimeout(JobU\Duration::seconds(5));
        queueThrows(JobU\OutcomeUnknownException::class, fn() => $q->save());
        queueCheck($q->isDirty() && $q->getRevision() === $revision && $q->getWeight() === 8, 'uncertain save');
        $q->discardChanges(); $q->refresh();
        queueCheck($q->getWeight() === 8 && !$q->isDirty(), 'explicit reconciliation');
        return true;
    )PHP");
    CHECK(proxy.requests["queue.update"].size() == before + 1);

    proxy.drop_method = "queue.suspend";
    proxy.drops       = 1;
    before            = proxy.requests["queue.suspend"].size();
    run("$GLOBALS['queueResource']->suspend(); return true;");
    CHECK(proxy.requests["queue.suspend"].size() == before + 2);
    statistics(proxy);

    run(R"PHP(
        $q = $GLOBALS['queueResource']; $c = $GLOBALS['queueClient'];
        $root = $q->defaults(); $view = $root->cli(); $revision = $q->getRevision();
        $state = $q->getState();
        // Deletion during string coercion must stop the outer setter at commit.
        $text = new class($q) {
            public function __construct(private JobU\Queue $q) {}
            public function __toString(): string { $this->q->delete(); return 'too-late'; }
        };
        queueThrows(JobU\NotFoundException::class, fn() => $q->setName($text));
        queueCheck($q->isDeleted() && $q->getRevision() === $revision && $q->getState() === $state && $q->getDeletedAt() === null, 'delete cached snapshot');
        foreach (['save', 'suspend', 'resume', 'delete'] as $method) {
            queueThrows(JobU\NotFoundException::class, fn() => $q->$method());
        }
        queueThrows(JobU\NotFoundException::class, fn() => $view->setTimeout(JobU\Duration::seconds(9)));
        queueThrows(JobU\NotFoundException::class, fn() => $root->clear('job.timeout'));
        queueThrows(JobU\NotFoundException::class, fn() => $q->setDefaults(new JobU\QueueDefaults()));
        queueThrows(JobU\NotFoundException::class, fn() => $q->refresh());
        $q->refresh(includeDeleted: true);
        queueCheck($q->getState() === JobU\QueueState::Deleted && $q->getDeletedAt() !== null && $q->getRevision() > $revision, 'historical refresh');
        queueCheck($q->defaults() === $root && $root->cli() === $view, 'historical identity');
        $loaded = $c->getQueueById($q->getId(), includeDeleted: true);
        queueCheck($loaded->isDeleted(), 'historical lookup');
        queueThrows(JobU\NotFoundException::class, fn() => $loaded->setWeight(4));
        $detached = clone $view; $detached->setTimeout(JobU\Duration::seconds(2));
        $GLOBALS['heldView'] = $view;
        return true;
    )PHP");
    // Retained statistics remain addressable by stable ID after soft deletion.
    auto id = jb::php::uuid_from_php(global("queueId"));
    REQUIRE(id);
    auto client = jb::php::native_client(Z_OBJ_P(global("queueClient").get()));
    REQUIRE(client);
    auto deadline = client.value()->deadline(jb::core::Clock::now());
    REQUIRE(deadline);
    auto call = jb::php::Operation::bind(
        "queue.stats",
        &jb::jobu::ControlClient::queue_statistics,
        jb::jobu::QueueStatisticsListRequest{jb::jobu::QueueStatisticsQuery{.selector = id.value()}});
    REQUIRE(call);
    REQUIRE(jb::php::resource_call(global("queueClient"), deadline.value(), call.value()));

    run(R"PHP(
        $c = $GLOBALS['queueClient'];
        $live = $c->createQueue(QUEUE_PREFIX . 'closed'); $c->close();
        $live->save(); $live->setName('local-only');
        queueThrows(JobU\ConnectionException::class, fn() => $live->save());
        queueCheck($live->isDirty(), 'closed save retains edits'); $live->discardChanges();
        queueCheck($live->getName() === QUEUE_PREFIX . 'closed', 'closed discard');
        queueThrows(JobU\ConnectionException::class, fn() => $live->refresh());
        $q = $GLOBALS['queueResource'];
        $q->discardChanges();
        $GLOBALS['weakQueue'] = WeakReference::create($q);
        $GLOBALS['weakClient'] = WeakReference::create($c);
        unset($GLOBALS['queueResource'], $GLOBALS['queueClient']);
        return true;
    )PHP");
    run(R"PHP(
        gc_collect_cycles();
        queueCheck($GLOBALS['weakQueue']->get() !== null && $GLOBALS['weakClient']->get() !== null, 'view retains owners');
        unset($GLOBALS['heldView']); gc_collect_cycles();
        queueCheck($GLOBALS['weakQueue']->get() === null && $GLOBALS['weakClient']->get() === null, 'cycles collected');
        return true;
    )PHP");
}
} // namespace jobu_php_test
