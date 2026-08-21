#pragma once

#include "error.hpp"
#include "result.hpp"

#include <cstddef>
#include <cstdint>

namespace jb::core {
class TimeSource;
}

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttemptExecutor;
class AttributeRegistry;

namespace detail {

struct SchedulerCoreOptions {
    std::uint32_t cli_concurrency{4};
    std::uint32_t http_concurrency{16};
    std::size_t   candidate_batch_size{200};
};

class SchedulerCore final {
public:
    SchedulerCore(jb::db::Database&        database,
                  AttributeRegistry const& attributes,
                  jb::core::TimeSource&    time_source,
                  AttemptExecutor&         executor,
                  SchedulerCoreOptions     options = {}) noexcept;

    [[nodiscard]] auto process_cycle() -> jb::core::Result<void, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
    jb::core::TimeSource&    _time_source;
    AttemptExecutor&         _executor;
    SchedulerCoreOptions     _options;
};

} // namespace detail

} // namespace jb::jobu
