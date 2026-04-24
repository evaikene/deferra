#include "shared_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using jb::core::ExplicitlySharedDataPointer;
using jb::core::SharedData;
using jb::core::make_explicitly_shared;

namespace {

/// Test payload
struct TestData : public SharedData {
    int value{0};
    std::string name;

    static std::atomic<int> instances;

    TestData()
    {
        instances.fetch_add(1, std::memory_order_relaxed);
    }
    TestData(int v, std::string n)
        : value{v}
        , name{std::move(n)}
    {
        instances.fetch_add(1, std::memory_order_relaxed);
    }
    TestData(TestData const& other)
        : SharedData(other)
        , value{other.value}
        , name{other.name}
    {
        instances.fetch_add(1, std::memory_order_relaxed);
    }

    ~TestData()
    {
        instances.fetch_sub(1, std::memory_order_relaxed);
    }

};

std::atomic<int> TestData::instances{0};

using TestPtr = ExplicitlySharedDataPointer<TestData>;

inline constexpr int kMagicNumber{42};

} // anonymous namespace

TEST_CASE("ExplicitlySharedDataPointer default constructor", "[core]")
{
    TestData::instances = 0;
    {
        TestPtr p;
        REQUIRE_FALSE(static_cast<bool>(p));
        REQUIRE(!p);
        REQUIRE(p.get() == nullptr);
        REQUIRE(p.use_count() == 0);
        REQUIRE_FALSE(p.unique());
        REQUIRE(p == nullptr);
    }
    REQUIRE(TestData::instances.load() == 0);
}

TEST_CASE("ExplicitlySharedDataPointer constructor from raw pointer", "[core]")
{
    TestData::instances = 0;
    {
        auto p = TestPtr(new TestData(kMagicNumber, "test"));
        REQUIRE(p);
        REQUIRE(p.get() != nullptr);
        REQUIRE(p->value == kMagicNumber);
        REQUIRE(p->name == "test");
        REQUIRE(p.use_count() == 1);
        REQUIRE(p.unique());
        REQUIRE(TestData::instances.load() == 1);
    }
    REQUIRE(TestData::instances.load() == 0);
}

TEST_CASE("Make ExplicitlySharedDataPointer with forward arguments", "[core]")
{
    TestData::instances = 0;
    {
        auto p = make_explicitly_shared<TestData>(kMagicNumber, std::string{"test"});
        REQUIRE(p);
        REQUIRE(p.get() != nullptr);
        REQUIRE(p->value == kMagicNumber);
        REQUIRE(p->name == "test");
        REQUIRE(p.use_count() == 1);
        REQUIRE(p.unique());
        REQUIRE(TestData::instances.load() == 1);
    }
    REQUIRE(TestData::instances.load() == 0);
}

TEST_CASE("ExplicitlySharedDataPointer copy constructor", "[core]")
{
    TestData::instances = 0;
    auto p1 = make_explicitly_shared<TestData>(1, "a");
    {
        TestPtr p2{p1}; // NOLINT(performance-unnecessary-copy-initialization)
        REQUIRE(p1.get() == p2.get());
        REQUIRE(p1.use_count() == 2);
        REQUIRE(p2.use_count() == 2);
        REQUIRE_FALSE(p1.unique());
        REQUIRE(TestData::instances.load() == 1);
    }
    REQUIRE(p1.use_count() == 1);
    REQUIRE(p1.unique());
    REQUIRE(TestData::instances.load() == 1);
}

TEST_CASE("Mutating shared data through one pointer affects all pointers sharing the same data", "[core]")
{
    TestData::instances = 0;
    auto p1 = make_explicitly_shared<TestData>(1, "initial");
    auto p2 = p1; // NOLINT(performance-unnecessary-copy-initialization)

    p2->value = kMagicNumber;
    p2->name = "changed";

    REQUIRE(p1->value == kMagicNumber);
    REQUIRE(p1->name == "changed");
    REQUIRE(p1.get() == p2.get());
}

TEST_CASE("Move construction transfers ownership", "[core]")
{
    TestData::instances = 0;
    auto p1 = make_explicitly_shared<TestData>(1, "a");
    auto* raw = p1.get();

    TestPtr p2{std::move(p1)};
    REQUIRE_FALSE(static_cast<bool>(p1));
    REQUIRE(p1.get() == nullptr);
    REQUIRE(p2);
    REQUIRE(p2.get() == raw);
    REQUIRE(p2.use_count() == 1);
    REQUIRE(TestData::instances.load() == 1);
}

TEST_CASE("Copy assignment releases previous target", "[core]")
{
    TestData::instances = 0;
    auto p1 = make_explicitly_shared<TestData>(1, "a");
    auto p2 = make_explicitly_shared<TestData>(2, "b");

    p2 = p1;

    REQUIRE(TestData::instances.load() == 1); // p2's old data was released
    REQUIRE(p1.get() == p2.get());
    REQUIRE(p1.use_count() == 2);
    REQUIRE(p2.use_count() == 2);
    REQUIRE(p1->value == 1);
    REQUIRE(p2->value == 1);
}

TEST_CASE("Self-assignment is safe", "[core]")
{
    auto p = make_explicitly_shared<TestData>(1, "a");
    REQUIRE(p.use_count() == 1);

    auto& alias = p;
    p = alias;

    REQUIRE(p);
    REQUIRE(p->value == 1);
    REQUIRE(p.use_count() == 1);
}

TEST_CASE("Assignment from raw pointer", "[core]")
{
    TestData::instances = 0;

    TestPtr p = make_explicitly_shared<TestData>(1, "initial");

    p = new TestData{2, "raw"};
    REQUIRE(p);
    REQUIRE(p->value == 2);
    REQUIRE(p.use_count() == 1);
    REQUIRE(TestData::instances.load() == 1);

    p = nullptr;

    REQUIRE_FALSE(static_cast<bool>(p));
    REQUIRE(TestData::instances.load() == 0);
}

TEST_CASE("Release and optionally adopt a new pointer with reset()", "[core]")
{
    TestData::instances = 0;

    auto p = make_explicitly_shared<TestData>(1, "a");
    REQUIRE(TestData::instances.load() == 1);

    p.reset();
    REQUIRE_FALSE(static_cast<bool>(p));
    REQUIRE(TestData::instances.load() == 0);

    p.reset(new TestData{2, "b"});
    REQUIRE(p);
    REQUIRE(p->value == 2);
    REQUIRE(p.use_count() == 1);
    REQUIRE(TestData::instances.load() == 1);
}

TEST_CASE("Exchange two pointers with swap()", "[core]")
{
    auto p1 = make_explicitly_shared<TestData>(1, "a");
    auto p2 = make_explicitly_shared<TestData>(2, "b");
    auto* raw1 = p1.get();
    auto* raw2 = p2.get();

    SECTION("member swap") {
        p1.swap(p2);
        REQUIRE(p1.get() == raw2);
        REQUIRE(p2.get() == raw1);
        REQUIRE(p1->value == 2);
        REQUIRE(p2->value == 1);
    }

    SECTION("free-function swap") {
        std::swap(p1, p2);
        REQUIRE(p1.get() == raw2);
        REQUIRE(p2.get() == raw1);
    }
}

TEST_CASE("detach() on a unique pointer is no-op", "[core]")
{
    TestData::instances = 0;

    auto p = make_explicitly_shared<TestData>(1, "a");
    auto* before = p.get();

    p.detach();

    REQUIRE(p.get() == before);
    REQUIRE(p.use_count() == 1);
    REQUIRE(TestData::instances.load() == 1);
}

TEST_CASE("detach() on a null pointer is no-op", "[core]")
{
    TestPtr p;
    p.detach();
    REQUIRE_FALSE(static_cast<bool>(p));
}

TEST_CASE("detach() on a shared pointer creates a private copy", "[core]")
{
    TestData::instances = 0;

    auto p1 = make_explicitly_shared<TestData>(1, "original");

    auto p2 = p1; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(p1.use_count() == 2);
    REQUIRE(TestData::instances.load() == 1);

    p2.detach();

    REQUIRE(p1.get() != p2.get());
    REQUIRE(p1.use_count() == 1);
    REQUIRE(p2.use_count() == 1);
    REQUIRE(TestData::instances.load() == 2);

    // copied data must be preserved
    REQUIRE(p2->value == p1->value);
    REQUIRE(p2->name == p1->name);

    // mutating p2 must not affect p1
    p2->value = kMagicNumber;
    p2->name = "changed";
    REQUIRE(p1->value == 1);
    REQUIRE(p1->name == "original");
}

TEST_CASE("Comparison operators", "[core]")
{
    auto p1 = make_explicitly_shared<TestData>(1, "a");
    auto p2 = p1; // NOLINT(performance-unnecessary-copy-initialization) copy
    auto p3 = make_explicitly_shared<TestData>(1, "a"); // equal content but different data pointer
    TestPtr null;

    REQUIRE(p1 == p2);
    REQUIRE_FALSE(p1 != p2);
    REQUIRE(p1 != p3);
    REQUIRE_FALSE(p1 == p3);
    REQUIRE(null == nullptr);
    REQUIRE_FALSE(null != nullptr);
    REQUIRE(p1 != nullptr);
    REQUIRE_FALSE(p1 == nullptr);
}

TEST_CASE("Dereference operators", "[core]")
{
    auto p = make_explicitly_shared<TestData>(1, "a");
    REQUIRE((*p).value == 1);
    REQUIRE((*p).name == "a");
    REQUIRE(p->value == 1);
    REQUIRE(p->name == "a");
}

TEST_CASE("Nested scopes track refcount correctly", "[core]")
{
    TestData::instances = 0;
    {
        auto p1 = make_explicitly_shared<TestData>(1, "a");
        REQUIRE(p1.use_count() == 1);
        {
            auto p2 = p1; // NOLINT(performance-unnecessary-copy-initialization)
            REQUIRE(p1.use_count() == 2);
            {
                auto p3 = p1; // NOLINT(performance-unnecessary-copy-initialization)
                REQUIRE(p1.use_count() == 3);
            }
            REQUIRE(p1.use_count() == 2);
        }
        REQUIRE(p1.use_count() == 1);
        REQUIRE(TestData::instances.load() == 1);
    }
    REQUIRE(TestData::instances.load() == 0);
}

TEST_CASE("Concurrent copies and destructions are race-free", "[core][thread]")
{
    TestData::instances = 0;
    auto shared = make_explicitly_shared<TestData>(0, "shared");

    constexpr int num_threads = 8;
    constexpr int iters_per_thread = 20'000;

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([shared, &go]() -> void { // capture-by-value bumps refcount
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int j = 0; j < iters_per_thread; ++j) {
                auto local = shared; // NOLINT(performance-unnecessary-copy-initialization) copy with atomic bump
                auto moved = std::move(local); // move without bump
                (void)moved->name; // touch the object
            } // moved destructs with atomic decrement
        });
    }

    // guarded join in case any thread throws an exception
    struct JoinAll {
        std::vector<std::thread>& ts;
        ~JoinAll()
        {
            for (auto& t : ts) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    } join_all{threads};

    go.store(true, std::memory_order_release);

    // explicit join
    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(shared.use_count() == 1);
    REQUIRE(TestData::instances.load() == 1);
}

TEST_CASE("detach() in one thread is isolated from readers in another", "[core][thread]")
{
    TestData::instances = 0;

    auto p1 = make_explicitly_shared<TestData>(kMagicNumber, "orig");
    auto p2 = p1; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(p1.use_count() == 2);

    constexpr int iters = 10'000;

    // p2 detaches and mutates its copy from another thread
    std::thread t([&p2]() -> void {
        p2.detach();
        for (int i = 0; i < iters; ++i) {
            p2->value = i;
        }
    });

    // guarded join in case the thread throws an exception
    struct Join {
        std::thread& t;
        ~Join()
        {
            if (t.joinable()) {
                t.join();
            }
        }
    } join{t};

    // p1 keeps reading its own data
    for (int i = 0; i < iters; ++i) {
        REQUIRE(p1->value == kMagicNumber);
        REQUIRE(p1->name == "orig");
    }

    t.join(); // explicit join
    REQUIRE(p1.get() != p2.get());
    REQUIRE(p1->value == kMagicNumber);

    REQUIRE(TestData::instances.load() == 2);
}
