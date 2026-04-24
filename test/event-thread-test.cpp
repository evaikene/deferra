#include "event_thread.hpp"

#include "application.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace jb::core;

TEST_CASE("Event Thread", "[core]")
{
    // main event loop
    Application app{0, nullptr};

    // Construction and destructions
    auto* event_thread = new EventThread;

    // threaded event loop object itself should belong to the current thread context
    REQUIRE(event_thread->thread_ctx() == ThreadCtx::current());

    // threaded event loop object itself should use the main event loop
    REQUIRE(event_thread->event_loop() == app.thread()->as_event_loop());

    // event loop it implements should be different from the main event loop
    REQUIRE(event_thread->as_event_loop() != app.thread()->as_event_loop());

    // run the event loop
    REQUIRE_NOTHROW(event_thread->exec(true));
    REQUIRE(event_thread->is_running());

    // event loop should now have a different thread context
    REQUIRE(event_thread->as_event_loop()->thread_ctx() != ThreadCtx::current());

    // quit the event loop with an exit code
    REQUIRE_NOTHROW(event_thread->quit(1));
    REQUIRE_NOTHROW(event_thread->wait());
    REQUIRE(event_thread->exit_code() == 1);

    delete event_thread;
}
