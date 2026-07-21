#include "support/fake_time_source.hpp"
#include "support/sequence_uuid_generator.hpp"
#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace std::chrono_literals;

TEST_CASE("FakeTimeSource advances clocks together and permits independent jumps", "[test][time]")
{
    jb::test::FakeTimeSource time_source;
    time_source.set_utc(UtcTimePoint{100s});
    time_source.set_monotonic(TimePoint{10s});

    time_source.advance(5s);
    CHECK(time_source.utc_now() == UtcTimePoint{105s});
    CHECK(time_source.monotonic_now() == TimePoint{15s});

    time_source.set_utc(UtcTimePoint{90s});
    CHECK(time_source.utc_now() == UtcTimePoint{90s});
    CHECK(time_source.monotonic_now() == TimePoint{15s});
}

TEST_CASE("TemporaryDirectory creates unique directories and removes them", "[test][filesystem]")
{
    std::filesystem::path first_path;
    {
        jb::test::TemporaryDirectory first;
        jb::test::TemporaryDirectory second;
        first_path = first.path();

        CHECK(std::filesystem::exists(first.path()));
        CHECK(std::filesystem::exists(second.path()));
        CHECK(first.path() != second.path());
        CHECK_FALSE(first.cleanup());
        CHECK_FALSE(std::filesystem::exists(first_path));
    }
    CHECK_FALSE(std::filesystem::exists(first_path));
}

TEST_CASE("TemporaryDirectory release transfers cleanup ownership", "[test][filesystem]")
{
    std::filesystem::path released_path;
    {
        jb::test::TemporaryDirectory directory;
        released_path = directory.release();
        CHECK(std::filesystem::exists(released_path));
    }
    CHECK(std::filesystem::exists(released_path));
    CHECK(std::filesystem::remove_all(released_path) > 0);
}

TEST_CASE("TemporaryDirectory moves cleanup ownership", "[test][filesystem]")
{
    std::filesystem::path moved_path;
    {
        jb::test::TemporaryDirectory source;
        moved_path = source.path();
        jb::test::TemporaryDirectory target{std::move(source)};
        CHECK(source.path().empty());
        CHECK(target.path() == moved_path);

        jb::test::TemporaryDirectory replacement;
        replacement = std::move(target);
        CHECK(target.path().empty());
        CHECK(replacement.path() == moved_path);
    }
    CHECK_FALSE(std::filesystem::exists(moved_path));
}

TEST_CASE("SequenceUuidGenerator returns configured values and stable exhaustion", "[test][uuid]")
{
    auto const                      first  = *Uuid::parse("00000000-0000-0000-0000-000000000001");
    auto const                      second = *Uuid::parse("00000000-0000-0000-0000-000000000002");
    jb::test::SequenceUuidGenerator generator{
        {first, second}
    };

    CHECK(*generator.generate() == first);
    CHECK(*generator.generate() == second);

    auto const exhausted = generator.generate();
    REQUIRE_FALSE(exhausted);
    CHECK(exhausted.error().category == ErrorCategory::ResourceExhausted);
    CHECK(exhausted.error().code == "test.uuid.sequence_exhausted");
}
