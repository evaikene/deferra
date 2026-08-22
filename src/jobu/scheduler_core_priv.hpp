#pragma once

#include "error.hpp"
#include "result.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace jb::core {
class TimeSource;
}

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttemptExecutor;
class AttributeRegistry;
class CronEngine;

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
                  CronEngine const&        cron,
                  jb::core::UuidGenerator& uuid_generator,
                  jb::core::TimeSource&    time_source,
                  AttemptExecutor&         executor,
                  SchedulerCoreOptions     options = {}) noexcept;

    [[nodiscard]] auto process_cycle() -> jb::core::Result<void, jb::core::Error>;

private:
    jb::db::Database&                       _database;
    AttributeRegistry const&                _attributes;
    CronEngine const&                       _cron;
    jb::core::UuidGenerator&                _uuid_generator;
    jb::core::TimeSource&                   _time_source;
    AttemptExecutor&                        _executor;
    SchedulerCoreOptions                    _options;
    std::map<jb::core::Uuid, std::uint32_t> _queue_weights;
    std::map<jb::core::Uuid, std::int64_t>  _cli_credits;
    std::map<jb::core::Uuid, std::int64_t>  _http_credits;
    std::map<jb::core::Uuid, std::uint64_t> _active_attempts;
    std::optional<jb::core::Error>          _failure;
    bool                                    _cli_first{true};
};

} // namespace detail

} // namespace jb::jobu
