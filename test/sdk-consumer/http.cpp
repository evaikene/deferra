#include <jb/core/application.hpp>
#include <jb/core/time_source.hpp>
#include <jb/jobu/http/http_attempt_executor.hpp>
#include <jb/net/http/system_http_client.hpp>

int main(int argc, char const* argv[])
{
    jb::core::Application      app{argc, argv};
    jb::core::SystemTimeSource time_source;
    auto                       client = jb::net::http::SystemHttpClient::create(*app.event_loop());
    if (!client) {
        return 1;
    }

    // Exercise both HTTP libraries and libcurl without sending a request. The
    // executor is destroyed before the client and clock it borrows.
    jb::jobu::http::HttpAttemptExecutor executor{*client.value(), time_source};
    return executor.is_available(jb::jobu::JobType::Http) ? 0 : 2;
}
