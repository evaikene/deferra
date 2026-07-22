#include "management.hpp"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<jb::jobu::ManagementService>);
static_assert(!std::is_move_constructible_v<jb::jobu::ManagementService>);
static_assert(std::is_same_v<jb::jobu::QueueSelector, std::variant<jb::core::Uuid, std::string>>);
static_assert(std::is_same_v<decltype(jb::jobu::JobPage::items), std::vector<jb::jobu::JobDefinition>>);

int main()
{
    jb::jobu::PageOptions const      page;
    jb::jobu::CreateJobRequest const request;
    return page.limit == 100 && request.type == jb::jobu::JobType::Cli ? 0 : 1;
}
