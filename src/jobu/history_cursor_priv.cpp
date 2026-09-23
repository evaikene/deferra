#include "history_cursor_priv.hpp"

#include <chrono>
#include <iterator>
#include <optional>
#include <utility>

namespace jb::jobu::detail {

namespace {

using jb::core::Error;
using jb::core::ErrorCategory;

auto invalid_cursor() -> Error
{
    return {.category = ErrorCategory::InvalidArgument,
            .code     = "jobu.history.invalid_cursor",
            .message  = "History cursor is invalid or unavailable"};
}

auto cursor_unavailable() -> Error
{
    return {.category = ErrorCategory::ResourceExhausted,
            .code     = "jobu.history.cursor_unavailable",
            .message  = "A history cursor could not be created"};
}

auto expiry(auto const& state) -> jb::core::TimePoint
{
    return std::visit([](auto const& value) { return value.expires_at; }, state);
}

auto canonical_token(std::string_view token) -> bool
{
    auto parsed = jb::core::Uuid::parse(token);
    return parsed && !parsed->is_nil() && parsed->to_string() == token;
}

} // namespace

HistoryCursorStore::HistoryCursorStore(jb::core::UuidGenerator& uuid_generator, jb::core::TimeSource& time_source)
    : _uuid_generator{uuid_generator}
    , _time_source{time_source}
{}

auto HistoryCursorStore::start_run(RunQuery query, RunCursorKey after) -> jb::core::Result<std::string, jb::core::Error>
{
    return issue(RunCursorState{.query      = query,
                                .after      = after,
                                .expires_at = _time_source.monotonic_now() + std::chrono::minutes{5}});
}

auto HistoryCursorStore::start_attempt(AttemptQuery query, AttemptNumber before_attempt_number)
    -> jb::core::Result<std::string, jb::core::Error>
{
    return issue(AttemptCursorState{.query                 = query,
                                    .before_attempt_number = before_attempt_number,
                                    .expires_at            = _time_source.monotonic_now() + std::chrono::minutes{5}});
}

auto HistoryCursorStore::get_run(std::string_view token) -> jb::core::Result<RunCursorState, jb::core::Error>
{
    auto found = find(token, Family::Run);
    if (!found) {
        return jb::core::Result<RunCursorState, Error>::failure(invalid_cursor());
    }
    return jb::core::Result<RunCursorState, Error>::success(std::get<RunCursorState>(*found));
}

auto HistoryCursorStore::get_attempt(std::string_view token) -> jb::core::Result<AttemptCursorState, jb::core::Error>
{
    auto found = find(token, Family::Attempt);
    if (!found) {
        return jb::core::Result<AttemptCursorState, Error>::failure(invalid_cursor());
    }
    return jb::core::Result<AttemptCursorState, Error>::success(std::get<AttemptCursorState>(*found));
}

auto HistoryCursorStore::advance_run(RunCursorState state, RunCursorKey after)
    -> jb::core::Result<std::string, jb::core::Error>
{
    state.after = after;
    return issue(state);
}

auto HistoryCursorStore::advance_attempt(AttemptCursorState state, AttemptNumber before_attempt_number)
    -> jb::core::Result<std::string, jb::core::Error>
{
    state.before_attempt_number = before_attempt_number;
    return issue(state);
}

void HistoryCursorStore::clear() noexcept
{
    _by_token.clear();
    _entries.clear();
}

auto HistoryCursorStore::size() const noexcept -> std::size_t
{
    return _entries.size();
}

auto HistoryCursorStore::issue(State state) -> jb::core::Result<std::string, jb::core::Error>
{
    auto const now = _time_source.monotonic_now();
    evict_expired(now);
    if (expiry(state) <= now) {
        return jb::core::Result<std::string, Error>::failure(invalid_cursor());
    }

    // A broken or deterministic generator must never overwrite a live cursor's state.
    auto token = std::optional<std::string>{};
    for (auto attempt = 0; attempt < 8 && !token; ++attempt) {
        auto generated = _uuid_generator.generate();
        if (!generated) {
            return jb::core::Result<std::string, Error>::failure(cursor_unavailable());
        }
        if (generated->is_nil()) {
            continue;
        }
        auto candidate = generated->to_string();
        if (!_by_token.contains(candidate)) {
            token = std::move(candidate);
        }
    }
    if (!token) {
        return jb::core::Result<std::string, Error>::failure(cursor_unavailable());
    }

    // Expired entries go first, then the oldest recently used entry. One new entry cannot exceed the cap.
    if (_entries.size() == maximum_entries) {
        _by_token.erase(_entries.front().token);
        _entries.pop_front();
    }
    _entries.push_back(Entry{.token = *token, .state = state});
    auto inserted = std::prev(_entries.end());
    _by_token.emplace(inserted->token, inserted);
    return jb::core::Result<std::string, Error>::success(std::move(*token));
}

auto HistoryCursorStore::find(std::string_view token, Family family) -> jb::core::Result<State, jb::core::Error>
{
    if (!canonical_token(token)) {
        return jb::core::Result<State, Error>::failure(invalid_cursor());
    }
    evict_expired(_time_source.monotonic_now());
    auto found = _by_token.find(std::string{token});
    if (found == _by_token.end()) {
        return jb::core::Result<State, Error>::failure(invalid_cursor());
    }
    auto const belongs_to_method = family == Family::Run
                                     ? std::holds_alternative<RunCursorState>(found->second->state)
                                     : std::holds_alternative<AttemptCursorState>(found->second->state);
    if (!belongs_to_method) {
        return jb::core::Result<State, Error>::failure(invalid_cursor());
    }

    // Preserve this token's boundary and fixed expiry. Lookup only updates eviction recency.
    _entries.splice(_entries.end(), _entries, found->second);
    return jb::core::Result<State, Error>::success(found->second->state);
}

void HistoryCursorStore::evict_expired(jb::core::TimePoint now)
{
    for (auto entry = _entries.begin(); entry != _entries.end();) {
        if (expiry(entry->state) <= now) {
            _by_token.erase(entry->token);
            entry = _entries.erase(entry);
        }
        else {
            ++entry;
        }
    }
}

} // namespace jb::jobu::detail
