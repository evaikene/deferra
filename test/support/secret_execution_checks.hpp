#pragma once

#include "catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "database.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "query.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::test {

inline auto secret_reference(std::string name) -> jb::core::JsonValue
{
    return {.data = jb::core::JsonValue::Object{{"secret", {.data = std::move(name)}}}};
}

/// All fixture values contain this marker, including values rejected before external execution.
inline constexpr std::string_view execution_secret_marker{"stage89-private-"};

inline void check_no_execution_secret(std::string_view text)
{
    CHECK(text.find(execution_secret_marker) == std::string_view::npos);
    // Binary request bodies become base64 in transient concrete payloads; scan that representation as well.
    CHECK(text.find("c3RhZ2U4OS1wcml2YXRl") == std::string_view::npos);
}

/// Both canonical request and recorded create result must keep the original template after execution and rotation.
inline void check_secret_create_record(jb::db::Database& database, jb::core::JsonValue const& payload)
{
    jb::db::Query query{database};
    REQUIRE(query.exec("SELECT request_json, result_json FROM jobu_idempotency WHERE method = 'job.create'"));
    auto next = query.next();
    REQUIRE(next);
    REQUIRE(*next);
    for (auto column : {0U, 1U}) {
        auto document = jb::core::parse_json(std::get<std::string>(query.value(column)));
        REQUIRE(document);
        CHECK(document->as_object().at("payload") == payload);
    }
    next = query.next();
    REQUIRE(next);
    CHECK_FALSE(*next);
    REQUIRE(query.finish());
}

/// Scan generated metadata only: the secret table and faithful raw output intentionally contain secret bytes.
inline void check_secret_execution_metadata(jb::db::Database& database)
{
    jb::db::Query query{database};
    REQUIRE(query.exec("SELECT payload_json AS document FROM jobu_jobs UNION ALL "
                       "SELECT payload_json FROM jobu_runs UNION ALL "
                       "SELECT result_json FROM jobu_runs WHERE result_json IS NOT NULL UNION ALL "
                       "SELECT result_json FROM jobu_attempts WHERE result_json IS NOT NULL UNION ALL "
                       "SELECT request_json FROM jobu_idempotency UNION ALL "
                       "SELECT result_json FROM jobu_idempotency"));
    while (true) {
        auto next = query.next();
        REQUIRE(next);
        if (!*next) {
            break;
        }
        check_no_execution_secret(std::get<std::string>(query.value(0)));
    }
    REQUIRE(query.finish());
}

/// Records every enabled level, including worker-thread diagnostics, without printing secret-bearing failures.
class SecretExecutionLogger final : public jb::core::Logger {
public:
    void log(jb::core::LogMessage const& message) override
    {
        std::lock_guard lock{_mutex};
        _messages.emplace_back(message.message);
    }

    void check(std::string_view expected_message) const
    {
        std::lock_guard lock{_mutex};
        bool            found = expected_message.empty();
        for (auto const& message : _messages) {
            check_no_execution_secret(message);
            found = found || message.find(expected_message) != std::string::npos;
        }
        CHECK(found);
    }

private:
    mutable std::mutex       _mutex;
    std::vector<std::string> _messages;
};

/// Restore the process-wide logger even when a test assertion unwinds the runner fixture.
class SecretExecutionLogGuard final {
public:
    SecretExecutionLogGuard()
        : _previous{jb::core::logger()}
        , _capture{std::make_shared<SecretExecutionLogger>()}
    {
        _capture->set_level(jb::core::LogLevel::Debug3);
        jb::core::set_logger(_capture);
    }

    ~SecretExecutionLogGuard() { jb::core::set_logger(std::move(_previous)); }

    SecretExecutionLogGuard(SecretExecutionLogGuard const&)                    = delete;
    auto operator=(SecretExecutionLogGuard const&) -> SecretExecutionLogGuard& = delete;

    void check(std::string_view expected_message = {}) const { _capture->check(expected_message); }

private:
    std::shared_ptr<jb::core::Logger>      _previous;
    std::shared_ptr<SecretExecutionLogger> _capture;
};

} // namespace jb::test
