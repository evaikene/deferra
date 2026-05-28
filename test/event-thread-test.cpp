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

    // threaded event loop object itself should still belong to the current thread context
    CHECK(event_thread->thread_ctx() == ThreadCtx::current());

    // threaded event loop object itself should use the same event loop it implements
    CHECK(event_thread->event_loop() == event_thread->as_event_loop());

    // event loop it implements should be different from the main event loop
    CHECK(event_thread->as_event_loop() != app.event_loop());

    // run the event loop
    CHECK_NOTHROW(event_thread->exec(true));
    CHECK(event_thread->is_running());

    // event loop should now have a different thread context
    CHECK(event_thread->thread_ctx() != ThreadCtx::current());

    // quit the event loop with an exit code
    CHECK_NOTHROW(event_thread->quit(1));
    CHECK_NOTHROW(event_thread->wait());
    CHECK(event_thread->exit_code() == 1);

    delete event_thread;
}
