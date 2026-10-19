#include <jb/core/application.hpp>
#include <jb/core/file.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/client/control_client.hpp>
#include <jb/jobu/management.hpp>
#include <jb/jobu/management_json.hpp>
#include <jb/rpc/client.hpp>

#include <string>

int main(int argc, char const* argv[])
{
    // RPC requires an open device. The private consumer build directory owns
    // this temporary file; the uninitialized typed client must perform no I/O.
    jb::core::Application app{argc, argv};
    jb::core::File        device;
    if (!device.open("sdk-client-device.tmp", {jb::core::OpenMode::ReadWrite, jb::core::OpenMode::Create})) {
        return 2;
    }

    // Reverse destruction order preserves every borrowed dependency.
    jb::rpc::Client                     raw{device};
    jb::jobu::StandardAttributeRegistry attributes;
    jb::jobu::ControlClient             client{raw, attributes};

    // Compile and link both public get entry points and codecs using only the installed SDK.
    auto const request = jb::jobu::GetQueueRequest{.queue = std::string{"archived"}, .include_deleted = true};
    auto       params  = jb::jobu::queue_get_request_to_json(request);
    if (!params) {
        return 3;
    }
    auto decoded = jb::jobu::queue_get_request_from_json(*params);
    if (!decoded || decoded->queue != request.queue || !decoded->include_deleted) {
        return 4;
    }
    auto richer = client.get_queue(*decoded);
    auto live   = client.get_queue(request.queue, jb::jobu::ControlCallOptions{});
    auto call   = client.get_system_info();
    return !call && call.error().code == "jobu.client.not_ready" && !richer &&
                   richer.error().code == "jobu.client.not_ready" && !live &&
                   live.error().code == "jobu.client.not_ready"
             ? 0
             : 1;
}
