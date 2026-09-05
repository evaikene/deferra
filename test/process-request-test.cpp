#include "process_request_priv.hpp"

#include "error.hpp"
#include "event_loop_types.hpp"
#include "process.hpp"
#include "result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::core::priv;
using namespace std::chrono_literals;

namespace {

auto request() -> ProcessStartInfo
{
    return {.executable = "/target"};
}

auto prepare(ProcessStartInfo info)
{
    return prepare_process_request(std::move(info), TimePoint{1s}, 2L * 1024 * 1024);
}

void expect_invalid(ProcessStartInfo info, std::string_view detail)
{
    auto result = prepare(std::move(info));
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "core.process.invalid_request");
    CHECK(result.error().detail == detail);
}

auto joined_path(std::size_t count, std::string_view directory) -> std::string
{
    std::string path;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            path += ':';
        }
        path += directory;
    }
    return path;
}

} // namespace

TEST_CASE("Process preparation preserves explicit values and defaults", "[core][process][request]")
{
    auto info        = request();
    info.arguments   = {"", "-x", "two words", "$(literal)"};
    info.environment = {
        {"Z",   ""             },
        {"A_1", "literal=value"}
    };
    auto result = prepare(info);
    REQUIRE(result);
    auto const& prepared = *result.value();
    REQUIRE(prepared.argv().size() == 6);
    CHECK(std::string{prepared.argv()[0]} == "/target");
    for (std::size_t index = 0; index < info.arguments.size(); ++index) {
        CHECK(std::string{prepared.argv()[index + 1]} == info.arguments[index]);
    }
    CHECK(prepared.argv().back() == nullptr);
    REQUIRE(prepared.envp().size() == 3);
    CHECK(std::string{prepared.envp()[0]} == "A_1=literal=value");
    CHECK(std::string{prepared.envp()[1]} == "Z=");
    CHECK(prepared.envp().back() == nullptr);
    CHECK(prepared.candidates() == std::vector<std::string>{"/target"});
    CHECK(prepared.working_directory() == "/");
    CHECK_FALSE(prepared.deadline());
    CHECK(prepared.termination_grace() == 5s);
    CHECK_FALSE(prepared.require_non_root());
    CHECK_FALSE(prepared.prevent_privilege_gain());

    info.working_directory      = "/nonexistent/literal/../directory";
    info.timeout                = 30s;
    info.termination_grace      = 0s;
    info.require_non_root       = true;
    info.prevent_privilege_gain = true;
    result                      = prepare(info);
    REQUIRE(result);
    CHECK(result.value()->working_directory() == info.working_directory.native());
    CHECK(result.value()->deadline() == TimePoint{31s});
    CHECK(result.value()->termination_grace() == 0s);
    CHECK(result.value()->require_non_root());
    CHECK(result.value()->prevent_privilege_gain());

    auto empty_environment = prepare(request());
    REQUIRE(empty_environment);
    CHECK(empty_environment.value()->envp() == std::vector<char*>{nullptr});
}

TEST_CASE("Process validates executable and working directory syntax and byte bounds", "[core][process][request]")
{
    for (auto const& value : std::vector<std::string>{
             "",
             "relative/path",
             "./target",
             "../target",
             std::string{"/a\0b", 4},
             "/" + std::string(4096, 'x')
    }) {
        auto info       = request();
        info.executable = value;
        expect_invalid(std::move(info), "executable.invalid");
    }
    for (auto const& value : std::vector<std::string>{
             "",
             "relative",
             std::string{"/a\0b", 4},
             "/" + std::string(4096, 'x')
    }) {
        auto info              = request();
        info.working_directory = value;
        expect_invalid(std::move(info), "working_directory.invalid");
    }
    auto info              = request();
    info.executable        = "/" + std::string(4095, 'x');
    info.working_directory = "/" + std::string(4095, 'y');
    REQUIRE(prepare(info));
    info.executable          = "one literal command";
    info.environment["PATH"] = "/bin";
    auto result              = prepare(info);
    REQUIRE(result);
    CHECK(result.value()->candidates() == std::vector<std::string>{"/bin/one literal command"});
}

TEST_CASE("Process validates arguments and ASCII environment names without inspecting values",
          "[core][process][request]")
{
    auto info = request();
    info.arguments.resize(1024);
    REQUIRE(prepare(info));
    info.arguments.emplace_back();
    expect_invalid(info, "arguments.too_many");
    info.arguments = {
        std::string{"a\0b", 3}
    };
    expect_invalid(info, "argument.contains_nul");

    for (auto const& name : std::vector<std::string>{
             "",
             "1A",
             "A=B",
             "A-B",
             " A",
             "A B",
             "A\n",
             "\xc3\xa4",
             std::string{"A\0B", 3}
    }) {
        info                   = request();
        info.environment[name] = "private-marker";
        expect_invalid(info, "environment.invalid_name");
    }
    info             = request();
    info.environment = {
        {"_",           ""                        },
        {"aZ_09",       "with=equals\n"           },
        {"JOBU_JOB_ID", "generic-core-allows-this"}
    };
    REQUIRE(prepare(info));
    info.environment["A"] = std::string{"x\0y", 3};
    expect_invalid(info, "environment.value_contains_nul");
}

TEST_CASE("Process timeout arithmetic checks exact representability before addition", "[core][process][request]")
{
    auto const base     = TimePoint{Duration{17}};
    auto const exact    = TimePoint::max().time_since_epoch() - base.time_since_epoch();
    auto       boundary = checked_process_deadline(base, exact);
    REQUIRE(boundary);
    CHECK(boundary.value() == TimePoint::max());
    for (auto timeout : {exact + Duration{1}, Duration::max()}) {
        auto result = checked_process_deadline(base, timeout);
        REQUIRE_FALSE(result);
        CHECK(result.error().detail == "timeout.deadline_out_of_range");
    }
    REQUIRE(checked_process_deadline(TimePoint::min(), Duration::max()));
    REQUIRE(checked_process_deadline(TimePoint{}, Duration::max()));
    REQUIRE(checked_process_deadline(base, 24h * 30));
    REQUIRE(checked_process_deadline(base, 24h * 31));
    for (auto timeout : {Duration::zero(), Duration{-1}, Duration::min()}) {
        auto info    = request();
        info.timeout = timeout;
        expect_invalid(info, "timeout.not_positive");
    }
    auto info    = request();
    info.timeout = Duration::max();
    expect_invalid(info, "timeout.deadline_out_of_range");
    info.timeout  = exact;
    auto prepared = prepare_process_request(info, base, 2L * 1024 * 1024);
    REQUIRE(prepared);
    CHECK(prepared.value()->deadline() == TimePoint::max());
    for (auto grace : {Duration{-1}, Duration{5min} + Duration{1}, Duration::max()}) {
        info                   = request();
        info.termination_grace = grace;
        expect_invalid(info, "termination_grace.out_of_range");
    }
    info                   = request();
    info.termination_grace = 5min;
    REQUIRE(prepare(info));
}

TEST_CASE("Process PATH lookup uses only explicit absolute entries in order", "[core][process][request]")
{
    auto info       = request();
    info.executable = "tool";
    expect_invalid(info, "path.missing");
    for (auto const& path : {"", ":/bin", "/bin:", "/bin::/usr/bin", "bin", "/bin:relative", "/bin:./local"}) {
        info.environment["PATH"] = path;
        expect_invalid(info, "path.invalid_entry");
    }
    info.environment["PATH"] = "/first:/second/:/:/first";
    auto result              = prepare(info);
    REQUIRE(result);
    CHECK(result.value()->candidates() ==
          std::vector<std::string>{"/first/tool", "/second/tool", "/tool", "/first/tool"});
    CHECK(std::string{result.value()->argv()[0]} == "tool");

    info.executable          = "/absolute";
    info.environment["PATH"] = "relative::entries";
    result                   = prepare(info);
    REQUIRE(result);
    CHECK(result.value()->candidates() == std::vector<std::string>{"/absolute"});
}

TEST_CASE("Process PATH count and expanded bytes have independent exact boundaries", "[core][process][request]")
{
    auto info                = request();
    info.executable          = "x";
    info.environment["PATH"] = joined_path(256, "/d");
    auto result              = prepare(info);
    REQUIRE(result);
    CHECK(result.value()->candidates().size() == 256);
    info.environment["PATH"] += ":/d";
    expect_invalid(info, "path.too_many_entries");

    // A long executable expands each short PATH entry; argv/env remain well below their separate limit.
    info.executable          = std::string(1020, 'x');
    info.environment["PATH"] = joined_path(256, "/d");
    result                   = prepare(info);
    REQUIRE(result);
    std::size_t expanded_bytes{0};
    for (auto const& candidate : result.value()->candidates()) {
        expanded_bytes += candidate.size() + 1;
    }
    CHECK(expanded_bytes == kMaxPathCandidateBytes);
    info.environment["PATH"].insert(1, "d");
    expect_invalid(info, "path.candidates_too_large");
}

TEST_CASE("Process aggregate counts strings separators NULs and both pointer arrays", "[core][process][request]")
{
    auto info        = request();
    info.arguments   = {"", "abc"};
    info.environment = {
        {"A", "xy"}
    };
    auto result = prepare(info);
    REQUIRE(result);
    auto const expected = std::size_t{8 + 1 + 4 + 5} + (6 * sizeof(char*));
    CHECK(result.value()->argument_bytes() == expected);

    auto const runtime_boundary = static_cast<long>(expected + kProcessArgumentSafetyMargin);
    REQUIRE(prepare_process_request(info, TimePoint{}, runtime_boundary));
    result = prepare_process_request(info, TimePoint{}, runtime_boundary - 1);
    REQUIRE_FALSE(result);
    CHECK(result.error().detail == "aggregate.runtime_limit");
    for (long limit : {-1L, 0L, static_cast<long>(kProcessArgumentSafetyMargin)}) {
        result = prepare_process_request(info, TimePoint{}, limit);
        REQUIRE_FALSE(result);
        CHECK(result.error().detail == "aggregate.runtime_limit_unavailable");
    }

    info           = request();
    info.arguments = {""};
    result         = prepare(info);
    REQUIRE(result);
    info.arguments[0].resize(kMaxProcessArgumentBytes - result.value()->argument_bytes(), 'x');
    result = prepare(info);
    REQUIRE(result);
    CHECK(result.value()->argument_bytes() == kMaxProcessArgumentBytes);
    info.arguments[0] += 'x';
    expect_invalid(info, "aggregate.too_large");

    info             = request();
    info.environment = {
        {"A", ""}
    };
    result = prepare(info);
    REQUIRE(result);
    info.environment["A"].resize(kMaxProcessArgumentBytes - result.value()->argument_bytes(), 'x');
    REQUIRE(prepare(info));
    info.environment["A"] += 'x';
    expect_invalid(info, "aggregate.too_large");

    info                                                         = request();
    info.environment[std::string(kMaxProcessArgumentBytes, 'A')] = "";
    expect_invalid(info, "aggregate.too_large");
}

TEST_CASE("Process prepared pointers survive result and owner transfers", "[core][process][request]")
{
    auto info        = request();
    info.arguments   = {"sso", std::string(2048, 'x')};
    info.environment = {
        {"A", "short"},
        {"B", std::string(2048, 'y')}
    };
    auto first = prepare(info);
    REQUIRE(first);
    auto const                                           argv      = first.value()->argv();
    auto const                                           envp      = first.value()->envp();
    auto const*                                          candidate = first.value()->candidates()[0].data();
    auto                                                 second    = std::move(first);
    auto                                                 owner     = std::move(second).value();
    std::vector<std::unique_ptr<PreparedProcessRequest>> owners;
    owners.push_back(std::move(owner));
    for (int index = 0; index < 20; ++index) {
        auto next = prepare(request());
        REQUIRE(next);
        owners.push_back(std::move(next).value());
    }
    CHECK(owners[0]->argv() == argv);
    CHECK(owners[0]->envp() == envp);
    CHECK(owners[0]->candidates()[0].data() == candidate);
    CHECK(std::string{argv[1]} == "sso");
    CHECK(std::string{argv[2]} == std::string(2048, 'x'));
    CHECK(std::string{envp[0]} == "A=short");
    CHECK(std::string{envp[1]} == "B=" + std::string(2048, 'y'));
}

TEST_CASE("Process request errors exclude supplied marker strings", "[core][process][request]")
{
    auto info        = request();
    info.executable  = "private-marker/command";
    info.arguments   = {"private-marker"};
    info.environment = {
        {"PRIVATE_MARKER", "private-marker"}
    };
    info.working_directory = "/private-marker";
    auto result            = prepare(info);
    REQUIRE_FALSE(result);
    auto const& error = result.error();
    for (auto const& value : {error.code, error.message, error.detail}) {
        CHECK(value.find("private-marker") == std::string::npos);
        CHECK(value.find("PRIVATE_MARKER") == std::string::npos);
    }
}
