#include <jb/core/application.hpp>
#include <jb/core/file.hpp>
#include <jb/jobu/attribute_registry.hpp>
#include <jb/jobu/client/control_client.hpp>
#include <jb/rpc/client.hpp>

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

    auto call = client.get_system_info();
    return !call && call.error().code == "jobu.client.not_ready" ? 0 : 1;
}
