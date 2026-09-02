#include "application.hpp"
#include "event_thread.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace jb::core;

TEST_CASE("Main Application", "[core]")
{
    // Construction and destruction

    REQUIRE(Application::instance() == nullptr);
    auto* thread_ctx = ThreadCtx::current();
    REQUIRE(thread_ctx->event_loop() == nullptr);

    {
        constexpr int argc   = 1;
        char const*   argv[] = {"test", nullptr};

        Application app{argc, argv};
        CHECK(Application::instance() == &app);
        REQUIRE(app.thread() != nullptr);

        // Thread context should have the application event loop set
        CHECK(thread_ctx->event_loop() == app.thread()->as_event_loop());
        CHECK(app.event_loop() == app.thread()->as_event_loop());
        CHECK(app.exit_code() == 0);
    }

    REQUIRE(Application::instance() == nullptr);
    CHECK(thread_ctx->event_loop() == nullptr);
}

TEST_CASE("Running main Application", "[core]")
{
    Application app{0, nullptr};
    CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);

    std::vector<int> emissions;

    app.about_to_start.connect([&emissions]() -> void { emissions.push_back(1); });

    app.about_to_quit.connect([&emissions]() -> void { emissions.push_back(2); });

    // Post the quit signal to the event loop to ensure the application will exit
    CHECK(app.quit());
    CHECK(app.exit_code() == 0);

    // Run the application
    CHECK(app.exec() == 0);

    CHECK(emissions == std::vector{1, 2});

    // Run again with a non-zero exit code
    emissions.clear();

    constexpr int exit_code = 42;
    CHECK(app.quit(exit_code));
    CHECK(app.exit_code() == exit_code);
    CHECK(app.exec() == exit_code);

    CHECK(app.exit_code() == exit_code);
    CHECK(emissions == std::vector{1, 2});
}
