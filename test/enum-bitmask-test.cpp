#include "enum_bitmask.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace jb::core;

namespace {

enum class Flag : unsigned int { // NOLINT(performance-enum-size)
    none  = 0,
    read  = 1 << 0,
    write = 1 << 1,
    exec  = 1 << 2,
};

using Flags = enum_bitmask<Flag>;

} // anonymous namespace

TEST_CASE("enum_bitmask construction", "[core]")
{
    SECTION("default constructs to empty")
    {
        constexpr Flags m;
        CHECK(m.bits() == 0);
        CHECK(m.none());
    }

    SECTION("construct from single flag")
    {
        constexpr Flags m{Flag::read};
        CHECK(m.bits() == 1U);
        CHECK(m.test(Flag::read));
        CHECK_FALSE(m.test(Flag::write));
    }

    SECTION("construct from raw bits")
    {
        constexpr Flags m{3U};
        CHECK(m.test(Flag::read));
        CHECK(m.test(Flag::write));
        CHECK_FALSE(m.test(Flag::exec));
    }

    SECTION("construct from initializer list")
    {
        const Flags m{Flag::read, Flag::exec};
        CHECK(m.test(Flag::read));
        CHECK_FALSE(m.test(Flag::write));
        CHECK(m.test(Flag::exec));
    }

    SECTION("initializer list with single flag")
    {
        const Flags m{Flag::write};
        CHECK(m.test(Flag::write));
        CHECK(m.bits() == static_cast<unsigned int>(Flag::write));
    }
}

TEST_CASE("enum_bitmask modifiers", "[core]")
{
    SECTION("reset clears all flags")
    {
        Flags m{Flag::read, Flag::write, Flag::exec};
        m.reset();
        CHECK(m.none());
        CHECK(m.bits() == 0);
    }

    SECTION("set adds a flag")
    {
        Flags m;
        m.set(Flag::read);
        CHECK(m.test(Flag::read));
        m.set(Flag::write);
        CHECK(m.test(Flag::read));
        CHECK(m.test(Flag::write));
    }

    SECTION("set with enum_bitmask merges flags")
    {
        Flags m{Flag::read};
        Flags extra{Flag::write, Flag::exec};
        m.set(extra);
        CHECK(m.test(Flag::read));
        CHECK(m.test(Flag::write));
        CHECK(m.test(Flag::exec));
    }

    SECTION("clear removes a flag")
    {
        Flags m{Flag::read, Flag::write};
        m.clear(Flag::read);
        CHECK_FALSE(m.test(Flag::read));
        CHECK(m.test(Flag::write));
    }

    SECTION("clear with enum_bitmask removes multiple flags")
    {
        Flags m{Flag::read, Flag::write, Flag::exec};
        Flags to_clear{Flag::read, Flag::exec};
        m.clear(to_clear);
        CHECK_FALSE(m.test(Flag::read));
        CHECK(m.test(Flag::write));
        CHECK_FALSE(m.test(Flag::exec));
    }

    SECTION("clear of unset flag is a no-op")
    {
        Flags m{Flag::write};
        m.clear(Flag::exec);
        CHECK(m.test(Flag::write));
        CHECK_FALSE(m.test(Flag::exec));
    }
}

TEST_CASE("enum_bitmask observers", "[core]")
{
    SECTION("test requires all flags to be set")
    {
        const Flags m{Flag::read, Flag::write};
        CHECK(m.test(Flag::read));
        CHECK(m.test(Flag::write));
        CHECK_FALSE(m.test(Flag::exec));

        CHECK(m.test(Flags{Flag::read, Flag::write}));
        CHECK_FALSE(m.test(Flags{Flag::read, Flag::exec}));
    }

    SECTION("test_any returns true if at least one flag matches")
    {
        const Flags m{Flag::read};
        CHECK(m.test_any(Flag::read));
        CHECK_FALSE(m.test_any(Flag::write));

        CHECK(m.test_any(Flags{Flag::read, Flag::write}));
        CHECK_FALSE(m.test_any(Flags{Flag::write, Flag::exec}));
    }

    SECTION("none and any are complementary")
    {
        Flags m;
        CHECK(m.none());
        CHECK_FALSE(m.any());

        m.set(Flag::read);
        CHECK_FALSE(m.none());
        CHECK(m.any());
    }

    SECTION("operator bool reflects any()")
    {
        Flags m;
        CHECK_FALSE(static_cast<bool>(m));

        m.set(Flag::write);
        CHECK(static_cast<bool>(m));
    }

    SECTION("bits and operator U return the underlying value")
    {
        constexpr Flags m{Flag::read};
        CHECK(m.bits() == static_cast<unsigned int>(Flag::read));
        CHECK(static_cast<Flags::value_type>(m) == m.bits());
    }
}

TEST_CASE("enum_bitmask bitwise operators", "[core]")
{
    SECTION("operator| combines flags")
    {
        constexpr Flags a{Flag::read};
        constexpr Flags b{Flag::write};
        constexpr Flags c = a | b;
        CHECK(c.test(Flag::read));
        CHECK(c.test(Flag::write));
        CHECK_FALSE(c.test(Flag::exec));
    }

    SECTION("operator& masks flags")
    {
        constexpr Flags a{Flag::read, Flag::write};
        constexpr Flags b{Flag::write, Flag::exec};
        constexpr Flags c = a & b;
        CHECK_FALSE(c.test(Flag::read));
        CHECK(c.test(Flag::write));
        CHECK_FALSE(c.test(Flag::exec));
    }

    SECTION("operator^ toggles flags")
    {
        constexpr Flags a{Flag::read, Flag::write};
        constexpr Flags b{Flag::write, Flag::exec};
        constexpr Flags c = a ^ b;
        CHECK(c.test(Flag::read));
        CHECK_FALSE(c.test(Flag::write));
        CHECK(c.test(Flag::exec));
    }

    SECTION("operator~ inverts all bits")
    {
        constexpr Flags a{Flag::read};
        constexpr Flags b = ~a;
        CHECK_FALSE(b.test(Flag::read));
        CHECK(b.test(Flag::write));
        CHECK(b.test(Flag::exec));
    }

    SECTION("operator|= merges flags in-place")
    {
        Flags m{Flag::read};
        m |= Flags{Flag::exec};
        CHECK(m.test(Flag::read));
        CHECK_FALSE(m.test(Flag::write));
        CHECK(m.test(Flag::exec));
    }

    SECTION("operator&= masks flags in-place")
    {
        Flags m{Flag::read, Flag::write};
        m &= Flags{Flag::write, Flag::exec};
        CHECK_FALSE(m.test(Flag::read));
        CHECK(m.test(Flag::write));
        CHECK_FALSE(m.test(Flag::exec));
    }

    SECTION("operator^= toggles flags in-place")
    {
        Flags m{Flag::read, Flag::write};
        m ^= Flags{Flag::write, Flag::exec};
        CHECK(m.test(Flag::read));
        CHECK_FALSE(m.test(Flag::write));
        CHECK(m.test(Flag::exec));
    }

    SECTION("compound operators return self-reference")
    {
        Flags m;
        Flags& ref = (m |= Flags{Flag::read});
        CHECK(&ref == &m);

        Flags& ref2 = (m &= Flags{Flag::read});
        CHECK(&ref2 == &m);

        Flags& ref3 = (m ^= Flags{Flag::write});
        CHECK(&ref3 == &m);
    }
}

TEST_CASE("enum_bitmask constexpr", "[core]")
{
    SECTION("key operations are usable in constant expressions")
    {
        static_assert(Flags{}.none());
        static_assert(Flags{Flag::read}.any());
        static_assert(Flags{Flag::read}.test(Flag::read));
        static_assert(!Flags{Flag::read}.test(Flag::write));
        static_assert((Flags{Flag::read} | Flags{Flag::write}).test(Flags{Flag::read, Flag::write}));
        static_assert((Flags{Flag::read, Flag::write} & Flags{Flag::write}).bits() ==
                      static_cast<unsigned int>(Flag::write));
    }
}
