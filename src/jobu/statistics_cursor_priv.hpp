#pragma once

#include "result.hpp"
#include "statistics.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>

namespace jb::jobu::detail {

/// Resolved query and keyset boundary; the original expiry survives successor tokens.
struct StatisticsCursorState {
    StatisticsRequest   request;
    StatisticsScope     scope{StatisticsScope::System};
    StatisticsGroupKey  after;
    jb::core::TimePoint expires_at;
};

/// Owner-thread statistics cursor storage with history's five-minute and 1024-entry policy.
class StatisticsCursorStore final {
public:
    static constexpr std::size_t maximum_entries = 1024;

    StatisticsCursorStore(jb::core::UuidGenerator& uuid_generator, jb::core::TimeSource& time_source);

    [[nodiscard]] auto start(StatisticsRequest request, StatisticsScope scope, StatisticsGroupKey after)
        -> jb::core::Result<std::string, jb::core::Error>;
    [[nodiscard]] auto get(std::string_view token, StatisticsScope scope)
        -> jb::core::Result<StatisticsCursorState, jb::core::Error>;
    [[nodiscard]] auto advance(StatisticsCursorState state, StatisticsGroupKey after)
        -> jb::core::Result<std::string, jb::core::Error>;
    void clear() noexcept;

private:
    struct Entry {
        std::string           token;
        StatisticsCursorState state;
    };

    [[nodiscard]] auto issue(StatisticsCursorState state) -> jb::core::Result<std::string, jb::core::Error>;
    void               evict_expired(jb::core::TimePoint now);

    jb::core::UuidGenerator&                                    _uuid_generator;
    jb::core::TimeSource&                                       _time_source;
    std::list<Entry>                                            _entries;
    std::unordered_map<std::string, std::list<Entry>::iterator> _by_token;
};

} // namespace jb::jobu::detail
