#include "control_client.hpp"

#include <string>
#include <type_traits>

static_assert(std::is_base_of_v<jb::core::Object, jb::jobu::ControlClient>);
static_assert(!std::is_copy_constructible_v<jb::jobu::ControlClient>);

static_assert(requires(jb::jobu::ControlClient&         client,
                       jb::jobu::GetQueueRequest const& request,
                       jb::jobu::QueueSelector const&   selector,
                       jb::core::Uuid const&            id,
                       std::string const&               name,
                       jb::jobu::ControlCallOptions     options) {
    client.get_queue(request);
    client.get_queue(request, options);
    client.get_queue(selector);
    client.get_queue(selector, options);
    client.get_queue(id);
    client.get_queue(id, options);
    client.get_queue(name);
    client.get_queue(name, options);
});

auto main() -> int
{
    return 0;
}
