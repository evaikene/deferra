#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>
#include <unordered_set>

using namespace jb::core;
using namespace std::chrono_literals;

namespace {

class FixedTimeSource final : public TimeSource {
public:
    [[nodiscard]] auto utc_now() const noexcept -> UtcTimePoint override { return _utc; }

    [[nodiscard]] auto monotonic_now() const noexcept -> TimePoint override { return _monotonic; }

    void set_utc(UtcTimePoint value) { _utc = value; }

private:
    UtcTimePoint _utc{UtcTimePoint{1720000000000ms}};
    TimePoint    _monotonic{TimePoint{10s}};
};

} // anonymous namespace

TEST_CASE("Uuid parses and formats canonical text", "[core][uuid]")
{
    auto const parsed = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");

    REQUIRE(parsed);
    CHECK(parsed->to_string() == "00112233-4455-6677-8899-aabbccddeeff");
    CHECK_FALSE(parsed->is_nil());
    CHECK(Uuid{}.is_nil());
    CHECK(Uuid::parse("00112233-4455-6677-8899-AABBCCDDEEFF")->to_string() == "00112233-4455-6677-8899-aabbccddeeff");
}

TEST_CASE("Uuid rejects invalid text", "[core][uuid]")
{
    for (auto const value : {std::string_view{""},
                             std::string_view{"001122334455-6677-8899-aabbccddeeff"},
                             std::string_view{"00112233-4455-6677-8899-aabbccddeefg"},
                             std::string_view{"00112233_4455_6677_8899_aabbccddeeff"}}) {
        auto const parsed = Uuid::parse(value);
        CHECK_FALSE(parsed);
        CHECK(parsed.error().code == "core.uuid.invalid_format");
    }
}

TEST_CASE("UuidV7Generator sets UUIDv7 version and variant bits", "[core][uuid]")
{
    FixedTimeSource generator_time;
    UuidV7Generator generator{generator_time};
    auto const      uuid = generator.generate();

    REQUIRE(uuid);
    CHECK((std::to_integer<unsigned char>(uuid->bytes()[6]) & 0xf0U) == 0x70U);
    CHECK((std::to_integer<unsigned char>(uuid->bytes()[8]) & 0xc0U) == 0x80U);
}

TEST_CASE("UuidV7Generator produces unique monotonically ordered UUIDs", "[core][uuid]")
{
    FixedTimeSource          generator_time;
    UuidV7Generator          generator{generator_time};
    std::unordered_set<Uuid> uuids;
    auto                     previous = generator.generate();

    REQUIRE(previous);
    REQUIRE(uuids.insert(*previous).second);
    for (auto index = 0; index < 128; ++index) {
        auto const next = generator.generate();
        REQUIRE(next);
        CHECK(*previous < *next);
        CHECK(uuids.insert(*next).second);
        previous = next;
    }

    generator_time.set_utc(UtcTimePoint{1719999999999ms});
    auto const regressed = generator.generate();
    REQUIRE(regressed);
    CHECK(*previous < *regressed);
}
