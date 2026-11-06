#include "lifecycle-fixture.hpp"

#include <phpcxx/exception.hpp>

#include <Zend/zend_exceptions.h>
#include <catch2/catch_test_macros.hpp>

#ifdef ZTS
#  include <array>
#  include <barrier>
#  include <thread>
#endif

TEST_CASE("Embed can restart its complete runtime without stale registration", "[php-cxx][lifecycle]")
{
    for (int cycle = 0; cycle < 3; ++cycle) {
        INFO("Runtime cycle " << cycle);
        {
            phpcxx_test::Runtime runtime;
            REQUIRE(phpcxx_test::exercise_request());
        }
        REQUIRE(phpcxx_test::cleanup_balanced());
    }
}

TEST_CASE("Requests reuse a runtime without leaking owners or engine state", "[php-cxx][lifecycle]")
{
    {
        phpcxx_test::Runtime runtime;
        for (int request = 0; request < 5; ++request) {
            INFO("Request cycle " << request);
            REQUIRE(phpcxx_test::exercise_request());
            phpcxx_test::leave_pending_exception();
            REQUIRE(php::has_exception());
            // The host resolves pending exceptions before normal request teardown.
            zend_clear_exception();
            REQUIRE(runtime.restart_request());
            REQUIRE_FALSE(php::has_exception());
        }
    }
    REQUIRE(phpcxx_test::cleanup_balanced());
}

#ifdef ZTS
namespace {

auto run_worker(int identity, std::barrier<>& rendezvous) -> bool
{
    // MINIT is already complete. TSRM constructs this worker's engine/SAPI
    // globals; no Zend object or request pointer crosses the thread boundary.
    (void)ts_resource(0);
    bool success = true;
    for (int request = 0; request < 3; ++request) {
        bool const started = php_request_startup() == SUCCESS;
        success            = started && success;
        if (started) {
            SG(headers_sent)            = 1;
            SG(request_info).no_headers = true;
            success                     = phpcxx_test::exercise_request() && success;
            success = phpcxx_test::evaluate_boolean(identity == 0 ? "($GLOBALS['worker'] = 11) === 11"
                                                                  : "($GLOBALS['worker'] = 22) === 22") &&
                      success;
            if (identity == 0) {
                phpcxx_test::leave_pending_exception();
            }
        }

        // Both requests stay live while one holds a pending exception. All paths
        // reach every barrier, so a failed check cannot strand the peer worker.
        rendezvous.arrive_and_wait();
        if (started) {
            success = (php::has_exception() == (identity == 0)) && success;
        }
        // Keep the exception pending until both workers have observed their state.
        rendezvous.arrive_and_wait();
        if (started) {
            if (php::has_exception()) {
                zend_clear_exception();
            }
            success = phpcxx_test::evaluate_boolean(identity == 0 ? "$GLOBALS['worker'] === 11"
                                                                  : "$GLOBALS['worker'] === 22") &&
                      success;
        }
        rendezvous.arrive_and_wait();
        if (started) {
            php_request_shutdown(nullptr);
            success = phpcxx_test::cleanup_balanced() && success;
        }
    }
    ts_free_thread();
    return success;
}

} // namespace

TEST_CASE("ZTS requests keep native ownership and exceptions on their execution threads", "[php-cxx][zts]")
{
    {
        phpcxx_test::Runtime runtime;
        std::barrier         rendezvous(2);
        std::array<bool, 2>  results{};
        {
            std::jthread first([&] { results[0] = run_worker(0, rendezvous); });
            std::jthread second([&] { results[1] = run_worker(1, rendezvous); });
        }
        CHECK(results[0]);
        CHECK(results[1]);
        CHECK_FALSE(php::has_exception());
        CHECK(phpcxx_test::exercise_request());
    }
    CHECK(phpcxx_test::cleanup_balanced());
}
#endif
