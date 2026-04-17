#define private public
#define protected public
#include "application.hpp"
#undef protected
#undef private

#include <catch2/catch_test_macros.hpp>

using namespace jb::core;

TEST_CASE("Main Application", "[core]")
{
    // Construction and destruction

    REQUIRE(Application::instance() == nullptr);

    {
        constexpr int argc = 1;
        char const* argv[] = { "test" , nullptr };

        Application app{argc, argv};
        REQUIRE(Application::instance() != nullptr);
        REQUIRE(app._argc == argc);
        REQUIRE(app._argv == argv);

        // Thread context should have the application event loop set
        auto* thread_ctx = ThreadCtx::current();
        REQUIRE(thread_ctx->event_loop() == &app);
    }

    REQUIRE(Application::instance() == nullptr);
}

TEST_CASE("Running main Application", "[core]")
{
    Application app{0, nullptr};

    bool about_to_start_emitted = false;
    bool about_to_quit_emitted  = false;

    app.about_to_start.connect(nullptr, [&about_to_start_emitted]() -> void {
        about_to_start_emitted = true;
    });

    app.about_to_quit.connect(nullptr, [&about_to_quit_emitted]() -> void {
        about_to_quit_emitted = true;
    });

    // Post the quit signal to the event loop to ensure the application will exit
    REQUIRE_NOTHROW(app.quit());

    // Run the application
    REQUIRE(app.exec() == 0);

    REQUIRE(about_to_start_emitted);
    REQUIRE(about_to_quit_emitted);

    // Run again with a non-zero exit code
    about_to_start_emitted = false;
    about_to_quit_emitted  = false;

    constexpr int exit_code = 42;
    REQUIRE_NOTHROW(app.quit(exit_code));
    REQUIRE(app.exec() == exit_code);

    REQUIRE(about_to_start_emitted);
    REQUIRE(about_to_quit_emitted);
}
