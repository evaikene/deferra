#pragma once

#include "fault_database_driver.hpp"
#include "recovery_fixture.hpp"

#include "uuid.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace jb::test {

/// Small default regressions and an explicit full-size profile share exactly the same builders.
struct JobuScaleProfile {
    std::size_t queues{6};
    std::size_t pending{420};
    std::size_t terminal{600};
};

/// Bounded generator state; generated IDs cannot collide with the fixture's persisted identity ranges.
class ScaleUuidGenerator final : public core::UuidGenerator {
public:
    [[nodiscard]] auto generate() -> core::Result<core::Uuid, core::Error> override;

private:
    std::uint32_t _next{1'000'000'000};
};

/// Aggregate observations keep neither row values nor an ever-growing per-call trace.
/// The SQL inventory contains unique statement templates only, never bound values.
struct ScaleDatabaseObservation {
    std::map<std::string, std::string> statements;
    std::map<std::string, std::size_t> executes;
    std::map<std::string, std::size_t> fetches;
    std::map<std::string, std::size_t> page_rows;
    std::map<std::string, std::size_t> maximum_page_rows;
    std::size_t                        commits{0};
    bool                               transaction_open{false};

    [[nodiscard]] auto classify(std::string_view sql) -> std::string;
    void               observe(DatabaseCall const& call);
    void               reset_counts();
};

/// Valid inert once definitions, one run each, and complete terminal attempts.
/// Seeding uses real repositories in bounded transactions. No constraints or durability settings
/// are disabled. The caller must release service borrowers before reopening the storage.
struct JobuScaleFixture {
    explicit JobuScaleFixture(std::optional<std::filesystem::path> const& path = std::nullopt);
    void seed(JobuScaleProfile profile, std::string const& http_url = "http://127.0.0.1/fixture");

    [[nodiscard]] auto count(std::string_view predicate = "1 = 1") -> std::int64_t;
    [[nodiscard]] auto scalar(std::string_view sql) -> std::int64_t;
    void               execute(std::string_view sql);
    void               query_plans();

    /// Stable IDs let assertions derive expected membership without retaining all payload snapshots.
    [[nodiscard]] static auto queue_id(std::size_t index) -> core::Uuid;
    [[nodiscard]] static auto pending_id(std::size_t index) -> core::Uuid;
    [[nodiscard]] static auto terminal_id(std::size_t index) -> core::Uuid;
    [[nodiscard]] static auto pending_queue(std::size_t index, JobuScaleProfile profile) -> std::size_t;
    [[nodiscard]] static auto terminal_queue(std::size_t index, JobuScaleProfile profile) -> std::size_t;

    ScaleDatabaseObservation            observation;
    std::shared_ptr<DatabaseFaultState> faults{std::make_shared<DatabaseFaultState>()};
    RecoveryFixture                     storage;
};

/// Emits one machine-readable measurement; native runners retain every sample and environment detail.
void scale_measurement(std::string_view operation, double milliseconds, std::size_t units);

/// Measures a synchronous operation without imposing a host-speed-dependent pass threshold.
template <typename Function>
void measure_scale(std::string_view operation, std::size_t units, Function&& function)
{
    auto const begin = std::chrono::steady_clock::now();
    function();
    auto const elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin);
    scale_measurement(operation, elapsed.count(), units);
}

} // namespace jb::test
