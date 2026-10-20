#pragma once

#include <cstdint>
#include <string_view>

namespace jb::jobuctl::detail {

/// Trusted coordinator selection; never derived from draft fields or other operator-supplied strings.
enum class EditResource : std::uint8_t {
    Job,
    Queue,
};

/// Fixed vocabulary shared by document errors, identity instructions, and manual recovery commands.
struct EditResourceDescriptor {
    std::string_view title;
    std::string_view identity_member;
    std::string_view command_group;
    std::string_view revision_conflict_code;
};

/// Accepts a named EditResource value. Resource-specific field semantics and draft preambles stay in domain code.
[[nodiscard]] constexpr auto describe_edit_resource(EditResource resource) -> EditResourceDescriptor
{
    if (resource == EditResource::Job) {
        return {.title                  = "Job",
                .identity_member        = "job_id",
                .command_group          = "job",
                .revision_conflict_code = "jobu.job.revision_conflict"};
    }
    return {.title                  = "Queue",
            .identity_member        = "queue_id",
            .command_group          = "queue",
            .revision_conflict_code = "jobu.queue.revision_conflict"};
}

} // namespace jb::jobuctl::detail
