#pragma once

#include "attribute.hpp"
#include "result.hpp"
#include "run.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct RunSnapshot {
    JobRevision            job_revision{1};
    jb::core::Uuid         queue_id;
    jb::core::UtcTimePoint planned_at;
    jb::core::UtcTimePoint runnable_at;
    JobType                type{JobType::Cli};
    std::int32_t           priority{0};
    AttributeSet           attributes;
    jb::rpc::JsonValue     payload;
};

class RunRepository final {
public:
    RunRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto insert_schedule_owned(JobRun const& run) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto find_schedule_owned(jb::core::Uuid const& job_id)
        -> jb::core::Result<std::optional<JobRun>, jb::core::Error>;
    [[nodiscard]] auto find_by_id(jb::core::Uuid const& run_id)
        -> jb::core::Result<std::optional<JobRun>, jb::core::Error>;
    [[nodiscard]] auto refresh_unstarted_schedule_owned(jb::core::Uuid const& job_id, RunSnapshot const& snapshot)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto move_non_terminal(jb::core::Uuid const& job_id,
                                         jb::core::Uuid const& target_queue_id,
                                         JobRevision next_revision) -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto
    cancel_pending_for_job(jb::core::Uuid const& job_id, jb::core::UtcTimePoint completed_at, std::string_view reason)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto cancel_pending_for_queue(jb::core::Uuid const&  queue_id,
                                                jb::core::UtcTimePoint completed_at,
                                                std::string_view       reason)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto count_running_for_job(jb::core::Uuid const& job_id)
        -> jb::core::Result<std::uint64_t, jb::core::Error>;
    [[nodiscard]] auto count_running_for_queue(jb::core::Uuid const& queue_id)
        -> jb::core::Result<std::uint64_t, jb::core::Error>;
    [[nodiscard]] auto list_terminal_before(jb::core::UtcTimePoint cutoff, std::size_t limit)
        -> jb::core::Result<std::vector<jb::core::Uuid>, jb::core::Error>;
    [[nodiscard]] auto delete_selected_terminal(std::span<jb::core::Uuid const> run_ids)
        -> jb::core::Result<std::size_t, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
