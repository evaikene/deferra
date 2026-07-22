#pragma once

#include "attribute.hpp"
#include "job.hpp"
#include "result.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace jb::db {
class Database;
}

namespace jb::jobu::detail {

class SerializedAttributeDocument;
class ValidatedJobPayload;

class JobRepository final {
public:
    JobRepository(jb::db::Database& database, AttributeRegistry const& attributes) noexcept;

    [[nodiscard]] auto insert(JobDefinition const&               job,
                              SerializedAttributeDocument const& attributes,
                              ValidatedJobPayload const&         payload) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto update_definition(JobDefinition const&               replacement,
                                         JobRevision                        expected_revision,
                                         SerializedAttributeDocument const& attributes,
                                         ValidatedJobPayload const& payload) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto set_state(jb::core::Uuid const&  id,
                                 JobState               expected_state,
                                 JobState               next_state,
                                 JobRevision            expected_revision,
                                 JobRevision            next_revision,
                                 jb::core::UtcTimePoint updated_at) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto move(jb::core::Uuid const&  id,
                            JobRevision            expected_revision,
                            jb::core::Uuid const&  target_queue_id,
                            JobRevision            next_revision,
                            jb::core::UtcTimePoint updated_at) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto mark_deleted(jb::core::Uuid const&  id,
                                    JobRevision            expected_revision,
                                    JobRevision            next_revision,
                                    jb::core::UtcTimePoint deleted_at) -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto has_exhausted_revision_in_queue(jb::core::Uuid const& queue_id, JobRevision maximum_revision)
        -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto mark_all_in_queue_deleted(jb::core::Uuid const& queue_id, jb::core::UtcTimePoint deleted_at)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto erase_secret_references_for_job(jb::core::Uuid const& job_id)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto erase_secret_references_for_queue(jb::core::Uuid const& queue_id)
        -> jb::core::Result<std::size_t, jb::core::Error>;
    [[nodiscard]] auto find_by_id(jb::core::Uuid const& id, bool include_deleted)
        -> jb::core::Result<std::optional<JobDefinition>, jb::core::Error>;
    [[nodiscard]] auto list(std::optional<jb::core::Uuid> queue_id,
                            bool                          include_deleted,
                            std::optional<JobState>       state,
                            std::optional<JobType>        type,
                            std::size_t                   limit,
                            std::optional<jb::core::Uuid> after_id)
        -> jb::core::Result<std::vector<JobDefinition>, jb::core::Error>;

private:
    jb::db::Database&        _database;
    AttributeRegistry const& _attributes;
};

} // namespace jb::jobu::detail
