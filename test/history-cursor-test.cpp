#include "history_cursor_priv.hpp"

#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "support/fake_time_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;
using namespace jb::test;
using namespace std::chrono_literals;

namespace {

class CountingUuidGenerator final : public UuidGenerator {
public:
    auto generate() -> Result<Uuid, Error> override
    {
        auto bytes = Uuid::Storage{};
        bytes[6]   = std::byte{0x70};
        bytes[8]   = std::byte{0x80};
        auto value = ++_next;
        for (auto index = std::size_t{0}; index < 8; ++index) {
            bytes[15 - index]   = static_cast<std::byte>(value & 0xff);
            value             >>= 8;
        }
        return Result<Uuid, Error>::success(Uuid{bytes});
    }

private:
    std::uint64_t _next{0};
};

auto query() -> RunQuery
{
    return RunQuery{.filters = {.state = RunState::Running}, .limit = 7};
}

auto key() -> RunCursorKey
{
    auto parsed = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(parsed);
    return {.planned_at = UtcTimePoint{1s}, .id = *parsed};
}

template <typename T>
void invalid_cursor(Result<T, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.history.invalid_cursor");
    CHECK(result.error().message == "History cursor is invalid or unavailable");
}

} // namespace

TEST_CASE("History cursors bind family and query without consuming a token", "[jobu][history][cursor]")
{
    FakeTimeSource        time;
    CountingUuidGenerator ids;
    HistoryCursorStore    store{ids, time};
    auto                  token = store.start_run(query(), key());
    REQUIRE(token);

    auto first = store.get_run(*token);
    auto again = store.get_run(*token);
    REQUIRE(first);
    REQUIRE(again);
    CHECK(first->query.limit == 7);
    CHECK(first->query.filters.state == RunState::Running);
    CHECK(first->after.id == again->after.id);
    CHECK(first->expires_at == again->expires_at);
    invalid_cursor(store.get_attempt(*token));
    invalid_cursor(store.get_run("not-a-token"));
    invalid_cursor(store.get_run("00000000-0000-0000-0000-000000000000"));

    auto attempt_token = store.start_attempt(AttemptQuery{.run_id = key().id, .limit = 11}, 4);
    REQUIRE(attempt_token);
    auto attempt = store.get_attempt(*attempt_token);
    REQUIRE(attempt);
    CHECK(attempt->query.run_id == key().id);
    CHECK(attempt->query.limit == 11);
    CHECK(attempt->before_attempt_number == 4);
    invalid_cursor(store.get_run(*attempt_token));
}

TEST_CASE("Successor cursors retain the initial monotonic expiry and input boundary", "[jobu][history][cursor]")
{
    FakeTimeSource        time;
    CountingUuidGenerator ids;
    HistoryCursorStore    store{ids, time};
    auto                  original = store.start_run(query(), key());
    REQUIRE(original);

    time.advance(4min);
    auto state = store.get_run(*original);
    REQUIRE(state);
    auto next_key        = key();
    next_key.planned_at += 1s;
    auto successor       = store.advance_run(*state, next_key);
    REQUIRE(successor);
    CHECK(*successor != *original);
    auto old_state = store.get_run(*original);
    auto new_state = store.get_run(*successor);
    REQUIRE(old_state);
    REQUIRE(new_state);
    CHECK(old_state->after.planned_at == key().planned_at);
    CHECK(new_state->after.planned_at == next_key.planned_at);
    CHECK(old_state->expires_at == new_state->expires_at);

    // Wall-clock movement cannot extend or shorten a monotonic cursor lease.
    time.set_utc(UtcTimePoint{1000000s});
    time.advance(1min);
    invalid_cursor(store.get_run(*original));
    invalid_cursor(store.get_run(*successor));
    CHECK(store.size() == 0);
}

TEST_CASE("History cursor storage evicts expired entries before the least recently used live entry",
          "[jobu][history][cursor]")
{
    FakeTimeSource        time;
    CountingUuidGenerator ids;
    HistoryCursorStore    store{ids, time};
    auto                  first = store.start_run(query(), key());
    REQUIRE(first);

    time.advance(1min);
    auto tokens = std::vector<std::string>{};
    tokens.reserve(HistoryCursorStore::maximum_entries);
    for (auto index = std::size_t{1}; index < HistoryCursorStore::maximum_entries; ++index) {
        auto token = store.start_run(query(), key());
        REQUIRE(token);
        tokens.push_back(*token);
    }
    REQUIRE(store.size() == HistoryCursorStore::maximum_entries);
    REQUIRE(store.get_run(*first)); // Make first the most recently used, without extending its expiry.

    auto overflow = store.start_run(query(), key());
    REQUIRE(overflow);
    CHECK(store.size() == HistoryCursorStore::maximum_entries);
    invalid_cursor(store.get_run(tokens.front()));
    REQUIRE(store.get_run(*first));

    time.advance(4min);
    auto replacement = store.start_run(query(), key());
    REQUIRE(replacement);
    invalid_cursor(store.get_run(*first));
    CHECK(store.size() == HistoryCursorStore::maximum_entries);

    store.clear();
    CHECK(store.size() == 0);
    invalid_cursor(store.get_run(*replacement));
}
