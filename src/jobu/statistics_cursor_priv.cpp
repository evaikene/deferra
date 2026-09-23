#include "statistics_cursor_priv.hpp"

#include <chrono>
#include <iterator>
#include <optional>
#include <utility>

namespace jb::jobu::detail {

namespace {

auto invalid_cursor() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = "jobu.statistics.invalid_cursor",
            .message  = "Statistics cursor is invalid or unavailable"};
}

auto cursor_unavailable() -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::ResourceExhausted,
            .code     = "jobu.statistics.cursor_unavailable",
            .message  = "A statistics cursor could not be created"};
}

auto canonical_token(std::string_view token) -> bool
{
    auto parsed = jb::core::Uuid::parse(token);
    return parsed && !parsed->is_nil() && parsed->to_string() == token;
}

} // namespace

StatisticsCursorStore::StatisticsCursorStore(jb::core::UuidGenerator& uuid_generator, jb::core::TimeSource& time_source)
    : _uuid_generator{uuid_generator}
    , _time_source{time_source}
{}

auto StatisticsCursorStore::start(StatisticsRequest request, StatisticsScope scope, StatisticsGroupKey after)
    -> jb::core::Result<std::string, jb::core::Error>
{
    return issue({.request    = request,
                  .scope      = scope,
                  .after      = after,
                  .expires_at = _time_source.monotonic_now() + std::chrono::minutes{5}});
}

auto StatisticsCursorStore::get(std::string_view token, StatisticsScope scope)
    -> jb::core::Result<StatisticsCursorState, jb::core::Error>
{
    if (!canonical_token(token)) {
        return jb::core::Result<StatisticsCursorState, jb::core::Error>::failure(invalid_cursor());
    }
    evict_expired(_time_source.monotonic_now());
    auto found = _by_token.find(std::string{token});
    if (found == _by_token.end() || found->second->state.scope != scope) {
        return jb::core::Result<StatisticsCursorState, jb::core::Error>::failure(invalid_cursor());
    }

    // Reading a token only updates LRU recency. The immutable query and boundary remain retryable.
    _entries.splice(_entries.end(), _entries, found->second);
    return jb::core::Result<StatisticsCursorState, jb::core::Error>::success(found->second->state);
}

auto StatisticsCursorStore::advance(StatisticsCursorState state, StatisticsGroupKey after)
    -> jb::core::Result<std::string, jb::core::Error>
{
    state.after = after;
    return issue(state);
}

void StatisticsCursorStore::clear() noexcept
{
    _by_token.clear();
    _entries.clear();
}

auto StatisticsCursorStore::issue(StatisticsCursorState state) -> jb::core::Result<std::string, jb::core::Error>
{
    auto const now = _time_source.monotonic_now();
    evict_expired(now);
    if (state.expires_at <= now) {
        return jb::core::Result<std::string, jb::core::Error>::failure(invalid_cursor());
    }

    auto token = std::optional<std::string>{};
    for (auto attempt = 0; attempt < 8 && !token; ++attempt) {
        auto generated = _uuid_generator.generate();
        if (!generated) {
            return jb::core::Result<std::string, jb::core::Error>::failure(cursor_unavailable());
        }
        if (!generated->is_nil()) {
            auto candidate = generated->to_string();
            if (!_by_token.contains(candidate)) {
                token = std::move(candidate);
            }
        }
    }
    if (!token) {
        return jb::core::Result<std::string, jb::core::Error>::failure(cursor_unavailable());
    }

    if (_entries.size() == maximum_entries) {
        _by_token.erase(_entries.front().token);
        _entries.pop_front();
    }
    _entries.push_back({.token = *token, .state = state});
    auto inserted = std::prev(_entries.end());
    _by_token.emplace(inserted->token, inserted);
    return jb::core::Result<std::string, jb::core::Error>::success(std::move(*token));
}

void StatisticsCursorStore::evict_expired(jb::core::TimePoint now)
{
    for (auto entry = _entries.begin(); entry != _entries.end();) {
        if (entry->state.expires_at <= now) {
            _by_token.erase(entry->token);
            entry = _entries.erase(entry);
        }
        else {
            ++entry;
        }
    }
}

} // namespace jb::jobu::detail
