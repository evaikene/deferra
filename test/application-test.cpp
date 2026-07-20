#include "application.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace jb::core;

TEST_CASE("Main Application", "[core]")
{
    // Construction and destruction

    REQUIRE(Application::instance() == nullptr);

    {
        constexpr int argc   = 1;
        char const*   argv[] = {"test", nullptr};

        Application app{argc, argv};
        CHECK(Application::instance() != nullptr);

        // Thread context should have the application event loop set
        auto* thread_ctx = ThreadCtx::current();
        CHECK(thread_ctx->event_loop() == app.event_loop());
    }

    REQUIRE(Application::instance() == nullptr);
}

TEST_CASE("Running main Application", "[core]")
{
    Application app{0, nullptr};
    CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);

    bool about_to_start_emitted = false;
    bool about_to_quit_emitted  = false;

    app.about_to_start.connect([&about_to_start_emitted]() -> void { about_to_start_emitted = true; });

    app.about_to_quit.connect([&about_to_quit_emitted]() -> void { about_to_quit_emitted = true; });

    // Post the quit signal to the event loop to ensure the application will exit
    CHECK(app.quit());

    // Run the application
    CHECK(app.exec() == 0);

    CHECK(about_to_start_emitted);
    CHECK(about_to_quit_emitted);

    // Run again with a non-zero exit code
    about_to_start_emitted = false;
    about_to_quit_emitted  = false;

    constexpr int exit_code = 42;
    CHECK(app.quit(exit_code));
    CHECK(app.exec() == exit_code);

    CHECK(about_to_start_emitted);
    CHECK(about_to_quit_emitted);
}
