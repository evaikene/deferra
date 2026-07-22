#pragma once

#include "attempt.hpp"
#include "byte_buffer.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

struct AttemptOutput {
    std::optional<jb::core::ByteBuffer> stdout_bytes;
    std::optional<jb::core::ByteBuffer> stderr_bytes;
    bool                                stdout_truncated{false};
    bool                                stderr_truncated{false};
    bool                                capture_lost{false};
};

class AttemptRepository final {
public:
    explicit AttemptRepository(jb::db::Database& database) noexcept;

    [[nodiscard]] auto insert_attempt(JobAttempt const& attempt) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto find(jb::core::Uuid const& run_id, AttemptNumber attempt_number)
        -> jb::core::Result<std::optional<JobAttempt>, jb::core::Error>;
    [[nodiscard]] auto list_for_run(jb::core::Uuid const& run_id, std::size_t limit)
        -> jb::core::Result<std::vector<JobAttempt>, jb::core::Error>;
    [[nodiscard]] auto insert_or_replace_output(jb::core::Uuid const& run_id,
                                                AttemptNumber         attempt_number,
                                                AttemptOutput const& output) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto find_output(jb::core::Uuid const& run_id, AttemptNumber attempt_number)
        -> jb::core::Result<std::optional<AttemptOutput>, jb::core::Error>;
    [[nodiscard]] auto has_any_for_run(jb::core::Uuid const& run_id) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto has_started_for_job(jb::core::Uuid const& job_id) -> jb::core::Result<bool, jb::core::Error>;

private:
    jb::db::Database& _database;
};

} // namespace jb::jobu::detail
