#include "error.hpp"
#include "result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

using namespace jb::core;

static_assert(!std::is_convertible_v<int, Result<int, std::string>>);
static_assert(!std::is_convertible_v<std::string, Result<int, std::string>>);

namespace {

struct Record {
    std::string name;
};

struct MoveOnly {
    explicit MoveOnly(int number)
        : number{number}
    {}

    MoveOnly(MoveOnly const&)                    = delete;
    MoveOnly(MoveOnly&&)                         = default;
    auto operator=(MoveOnly const&) -> MoveOnly& = delete;
    auto operator=(MoveOnly&&) -> MoveOnly&      = default;

    int number;
};

} // anonymous namespace

TEST_CASE("Result distinguishes uninitialized, successful, and failed states", "[core][result]")
{
    Result<int, std::string> uninitialized;
    auto const               success = Result<int, std::string>::success(42);
    auto const               failure = Result<int, std::string>::failure("missing");

    CHECK_FALSE(uninitialized.is_initialized());
    CHECK_FALSE(uninitialized.has_value());
    CHECK_FALSE(uninitialized);

    CHECK(success.is_initialized());
    CHECK(success.has_value());
    CHECK(success);
    CHECK(success.value() == 42);
    CHECK(success.value_or(1) == 42);

    CHECK(failure.is_initialized());
    CHECK_FALSE(failure.has_value());
    CHECK_FALSE(failure);
    CHECK(failure.error() == "missing");
    CHECK(failure.value_or(1) == 1);
}

TEST_CASE("Result supports identical value and error types", "[core][result]")
{
    auto const success = Result<std::string, std::string>::success("value");
    auto const failure = Result<std::string, std::string>::failure("error");

    CHECK(success.value() == "value");
    CHECK(failure.error() == "error");
    CHECK(success != failure);
}

TEST_CASE("Result provides dereference, arrow, and move-only access", "[core][result]")
{
    auto record = Result<Record, std::string>::success({.name = "JobU"});
    auto value  = Result<MoveOnly, std::string>::success(MoveOnly{7});

    CHECK((*record).name == "JobU");
    CHECK(record->name == "JobU");
    CHECK(std::move(value).value().number == 7);
}

TEST_CASE("Result moves a move-only value through rvalue value_or", "[core][result]")
{
    auto success = Result<MoveOnly, std::string>::success(MoveOnly{7});
    auto failure = Result<MoveOnly, std::string>::failure("missing");

    CHECK(std::move(success).value_or(MoveOnly{1}).number == 7);
    CHECK(std::move(failure).value_or(MoveOnly{1}).number == 1);
}

TEST_CASE("Result throws for wrong-state access", "[core][result]")
{
    Result<int, std::string> uninitialized;
    auto                     failure = Result<int, std::string>::failure("missing");
    auto                     success = Result<int, std::string>::success(42);

    CHECK_THROWS_AS(uninitialized.value(), std::logic_error);
    CHECK_THROWS_AS(uninitialized.error(), std::logic_error);
    CHECK_THROWS_AS(failure.value(), std::logic_error);
    CHECK_THROWS_AS(success.error(), std::logic_error);
}

TEST_CASE("Void Result supports successful and failed completion", "[core][result]")
{
    auto const success = Result<void, std::string>::success();
    auto const failure = Result<void, std::string>::failure("cancelled");

    CHECK(success.is_initialized());
    CHECK(success.has_value());
    CHECK(success);
    CHECK_NOTHROW(success.value());

    CHECK(failure.is_initialized());
    CHECK_FALSE(failure.has_value());
    CHECK(failure.error() == "cancelled");
    CHECK_THROWS_AS(failure.value(), std::logic_error);
}

TEST_CASE("Error preserves its stable code", "[core][error]")
{
    auto const error = Error{
        .category = ErrorCategory::Conflict,
        .code     = "jobu.job.revision_conflict",
        .message  = "Job revision does not match",
        .detail   = "expected 3, got 2",
    };

    auto const result = Result<int, Error>::failure(error);

    CHECK(result.error() == error);
    CHECK(result.error().code == "jobu.job.revision_conflict");
}
