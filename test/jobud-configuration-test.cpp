#include "configuration_priv.hpp"

#include "support/temporary_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::jobud::detail;

namespace {

auto failure(std::string_view text) -> StartupError
{
    auto parsed = parse_configuration_text(text);
    REQUIRE_FALSE(parsed);
    return std::move(parsed).error();
}

} // anonymous namespace

TEST_CASE("daemon configuration retains supplied values and accepts the full key table", "[jobud][configuration]")
{
    jb::test::TemporaryDirectory directory;
    auto const                   ca_bundle = directory.path() / "ca.pem";
    std::ofstream{ca_bundle} << "test certificate file";

    auto text  = std::string{"database.backend = sqlite\n"
                             "database.path = /var/lib/jobu/jobu.sqlite3\n"
                             "socket.path = /run/jobu/jobud.sock\n"
                             "socket.owner = jobu\n"
                             "socket.mode = 0660\n"
                             "socket.group = operators\n"
                             "daemon.run_as_user = jobu\n"
                             "daemon.run_as_group = jobu\n"
                             "daemon.allow_root = Off\n"
                             "cli.allow_root = YES\n"
                             "cli.concurrency = 4294967295\n"
                             "http.concurrency = 1\n"
                             "http.proxy = https://proxy.test:443\n"
                             "schedule.default_timezone = UTC\n"
                             "history.default_retention = 30d\n"
                             "history.sweep_interval = 1m\n"
                             "history.batch_size = 100\n"
                             "telemetry.checkpoint_interval = 30s\n"
                             "rpc.header_limit_bytes = 64k\n"
                             "rpc.body_limit_bytes = 16m\n"
                             "rpc.max_batch_entries = 64\n"
                             "rpc.max_connections = 4096\n"
                             "rpc.queued_output_bytes = 32m\n"
                             "logging.level = debug3\n"
                             "logging.format = text\n"
                             "defaults.retry.mode = '\"blocking\"'\n"
                             "defaults.retry.max_attempts = 3\n"};
    text      += "http.ca_bundle = " + ca_bundle.string() + "\n";

    auto parsed = parse_configuration_text(text);
    if (!parsed) {
        FAIL(parsed.error().code << " key=" << parsed.error().key.value_or("none")
                                 << " message=" << parsed.error().message);
    }
    CHECK(parsed->database_backend == DatabaseBackend::Sqlite);
    CHECK(parsed->database_path == "/var/lib/jobu/jobu.sqlite3");
    CHECK(parsed->socket_path == "/run/jobu/jobud.sock");
    CHECK(parsed->socket_owner == "jobu");
    CHECK(parsed->socket_mode == 0660U);
    CHECK(parsed->socket_group == "operators");
    CHECK(parsed->run_as_user == "jobu");
    CHECK(parsed->run_as_group == "jobu");
    CHECK(parsed->allow_root_daemon == false);
    CHECK(parsed->allow_root_cli == true);
    CHECK(parsed->cli_concurrency == UINT32_MAX);
    CHECK(parsed->http_concurrency == 1U);
    CHECK(parsed->http_proxy == "https://proxy.test:443");
    CHECK(parsed->http_ca_bundle == ca_bundle);
    CHECK(parsed->default_timezone == "UTC");
    CHECK(parsed->default_retention == std::chrono::seconds{2'592'000});
    CHECK(parsed->history_sweep_interval == std::chrono::seconds{60});
    CHECK(parsed->history_batch_size == 100U);
    CHECK(parsed->telemetry_checkpoint_interval == std::chrono::seconds{30});
    CHECK(parsed->rpc_header_limit_bytes == 65'536U);
    CHECK(parsed->rpc_body_limit_bytes == 16'777'216U);
    CHECK(parsed->rpc_max_batch_entries == 64U);
    CHECK(parsed->rpc_max_connections == 4'096U);
    CHECK(parsed->rpc_queued_output_bytes == 33'554'432U);
    CHECK(parsed->rpc_read_buffer_capacity == 16'842'752U);
    CHECK(parsed->logging_level == jb::core::LogLevel::Debug3);
    CHECK(parsed->logging_format == LoggingFormat::Text);
    CHECK(parsed->daemon_defaults.size() == 2U);
    CHECK(std::get<std::string>(parsed->daemon_defaults.at("retry.mode").data) == "blocking");
    CHECK(std::get<std::int64_t>(parsed->daemon_defaults.at("retry.max_attempts").data) == 3);
}

TEST_CASE("daemon configuration keeps omissions distinct from explicit false and zero", "[jobud][configuration]")
{
    auto empty = parse_configuration_text("# comment\n; another comment\n");
    REQUIRE(empty);
    CHECK_FALSE(empty->allow_root_daemon);
    CHECK_FALSE(empty->database_path);
    CHECK(empty->daemon_defaults.empty());
    CHECK(empty->rpc_read_buffer_capacity == 1'064'960U);

    auto explicit_values = parse_configuration_text("daemon.allow_root = 0\n"
                                                    "history.default_retention = 0\n"
                                                    "socket.owner = \n");
    REQUIRE(explicit_values);
    CHECK(explicit_values->allow_root_daemon == false);
    CHECK(explicit_values->default_retention == std::chrono::seconds::zero());
    CHECK_FALSE(explicit_values->socket_owner);
}

TEST_CASE("daemon configuration rejects malformed grammar and unsafe text", "[jobud][configuration]")
{
    CHECK(failure("[daemon]\n").line == 1U);
    CHECK(failure("include = other.ini\n").line == 1U);
    CHECK(failure("socket.path = /run/jobu.sock\nsocket.path = /run/other.sock\n").code ==
          "jobud.config.duplicate_key");
    CHECK(failure("defaults.retry.mode = '\"blocking\"'\ndefaults.retry.mode = '\"reschedule\"'\n").code ==
          "jobud.config.duplicate_key");
    CHECK(failure("unknown.option = value\n").code == "jobud.config.unknown_key");
    CHECK_FALSE(failure("unknown.option = value\n").key);
    CHECK(failure("defaults.unknown = 3\n").code == "jobud.config.unknown_key");

    auto const nul =
        std::string{"logging.level = info\0secret = yes", sizeof("logging.level = info\0secret = yes") - 1U};
    CHECK(failure(nul).code == "jobud.config.invalid");
    CHECK(failure(std::string{"logging.level = \xff\n", sizeof("logging.level = \xff\n") - 1U}).code ==
          "jobud.config.invalid");
    CHECK(parse_configuration_text(std::string(65'536U, ' ')));
    CHECK(failure(std::string(65'537U, ' ')).code == "jobud.config.invalid");
}

TEST_CASE("daemon configuration accepts the explicit boolean vocabulary", "[jobud][configuration]")
{
    for (auto const* value : {"true", "TRUE", "1", "On", "YES"}) {
        CAPTURE(value);
        auto parsed = parse_configuration_text(std::string{"cli.allow_root = "} + value + "\n");
        REQUIRE(parsed);
        CHECK(parsed->allow_root_cli == true);
    }
    for (auto const* value : {"false", "FALSE", "0", "Off", "NO"}) {
        CAPTURE(value);
        auto parsed = parse_configuration_text(std::string{"daemon.allow_root = "} + value + "\n");
        REQUIRE(parsed);
        CHECK(parsed->allow_root_daemon == false);
    }
    for (auto const* value : {"y", "n", "2", "maybe", " false", "false "}) {
        CAPTURE(value);
        CHECK(failure(std::string{"cli.allow_root = '"} + value + "'\n").key == "cli.allow_root");
    }
}

TEST_CASE("daemon configuration checks numbers paths and timezone", "[jobud][configuration]")
{
    for (auto const* value : {"-1", "+1", "0", "4294967296", "1x", "1.0", "18446744073709551616"}) {
        CAPTURE(value);
        CHECK(failure(std::string{"cli.concurrency = "} + value + "\n").key == "cli.concurrency");
    }
    for (auto const* value : {"600", "660", "0601", "0666", "0o600"}) {
        CAPTURE(value);
        CHECK(failure(std::string{"socket.mode = "} + value + "\n").key == "socket.mode");
    }
    CHECK(failure("database.backend = mysql\n").key == "database.backend");
    CHECK(failure("database.path = relative.db\n").key == "database.path");
    CHECK(failure("socket.path = /tmp/" + std::string(200, 'a') + "\n").key == "socket.path");
    CHECK(failure("http.proxy = http://user:pass@proxy.test\n").key == "http.proxy");
    for (auto const* malformed_proxy : {"http://proxy.test:99999", "http://[invalid]:8080"}) {
        auto const error = failure(std::string{"http.proxy = "} + malformed_proxy + "\n");
        CHECK(error.code == "jobud.config.invalid");
        CHECK(error.key == "http.proxy");
        CHECK(error.message.find(malformed_proxy) == std::string::npos);
    }
    CHECK(failure("http.ca_bundle = /missing/jobu-ca.pem\n").key == "http.ca_bundle");
    CHECK(failure("schedule.default_timezone = ../UTC\n").key == "schedule.default_timezone");
    CHECK(failure("history.default_retention = 9223372036855s\n").key == "history.default_retention");
}

TEST_CASE("daemon configuration converts intervals and byte quantities before validation", "[jobud][configuration]")
{
    for (auto const& [value, seconds] : {
             std::pair{"30",  30    },
             std::pair{"30s", 30    },
             std::pair{"5m",  300   },
             std::pair{"1h",  3'600 },
             std::pair{"1d",  86'400}
    }) {
        CAPTURE(value);
        auto parsed = parse_configuration_text(std::string{"history.default_retention = "} + value + "\n");
        REQUIRE(parsed);
        CHECK(parsed->default_retention == std::chrono::seconds{seconds});
    }

    auto bytes = parse_configuration_text("rpc.header_limit_bytes = 2k\n"
                                          "rpc.body_limit_bytes = 1m\n"
                                          "rpc.queued_output_bytes = 2m\n");
    REQUIRE(bytes);
    CHECK(bytes->rpc_header_limit_bytes == 2'048U);
    CHECK(bytes->rpc_body_limit_bytes == 1'048'576U);
    CHECK(bytes->rpc_queued_output_bytes == 2'097'152U);
    CHECK(bytes->rpc_read_buffer_capacity == 1'050'624U);

    for (auto const* value : {"-1m", "+1h", "1ms", "1.5h", "1H", "18446744073709551615d"}) {
        CAPTURE(value);
        CHECK(failure(std::string{"history.default_retention = "} + value + "\n").key == "history.default_retention");
    }
    for (auto const* value : {"-1k", "+2m", "1kb", "1.5m", "1M", "18446744073709551615g"}) {
        CAPTURE(value);
        CHECK(failure(std::string{"rpc.header_limit_bytes = "} + value + "\n").key == "rpc.header_limit_bytes");
    }

    for (auto const* key : {"history.default_retention_seconds",
                            "history.sweep_interval_seconds",
                            "telemetry.checkpoint_interval_seconds"}) {
        CAPTURE(key);
        CHECK(failure(std::string{key} + " = 30\n").code == "jobud.config.unknown_key");
    }
}

TEST_CASE("daemon configuration accepts inclusive limits and rejects adjacent values", "[jobud][configuration]")
{
    struct Boundary {
        std::string_view key;
        std::string_view accepted;
        std::string_view rejected;
    };

    for (auto const& boundary : {
             Boundary{.key = "history.sweep_interval",        .accepted = "24h",  .rejected = "86401s"},
             Boundary{.key = "history.batch_size",            .accepted = "1000", .rejected = "1001"  },
             Boundary{.key = "telemetry.checkpoint_interval", .accepted = "1s",   .rejected = "0m"    },
             Boundary{.key = "rpc.header_limit_bytes",        .accepted = "1k",   .rejected = "1023"  },
             Boundary{.key = "rpc.max_batch_entries",         .accepted = "64",   .rejected = "65"    },
             Boundary{.key = "rpc.max_connections",           .accepted = "4096", .rejected = "4097"  }
    }) {
        CAPTURE(boundary.key);
        CHECK(parse_configuration_text(std::string{boundary.key} + " = " + std::string{boundary.accepted} + "\n"));
        CHECK(failure(std::string{boundary.key} + " = " + std::string{boundary.rejected} + "\n").key == boundary.key);
    }

    CHECK(parse_configuration_text("history.default_retention = 9223372036854s\n"));
    CHECK(parse_configuration_text("rpc.body_limit_bytes = 16m\nrpc.queued_output_bytes = 16842752\n"));
    CHECK(parse_configuration_text("rpc.queued_output_bytes = 64m\n"));
    CHECK(failure("rpc.queued_output_bytes = 65m\n").key == "rpc.queued_output_bytes");
    CHECK(parse_configuration_text("socket.mode = 0600\n"));
}

TEST_CASE("daemon configuration enforces registry defaults and RPC budgets", "[jobud][configuration]")
{
    CHECK(failure("defaults.retry.mode = block\n").code == "jobud.config.invalid");
    CHECK(failure("defaults.retry.initial_delay = 2000\ndefaults.retry.max_delay = 1000\n").code ==
          "jobud.config.invalid");
    CHECK(failure("defaults.retry.max_delay = -1\n").code == "jobud.config.invalid");
    CHECK(failure("defaults.retry.initial_delay = 30s\n").code == "jobud.config.invalid");
    CHECK(failure("rpc.body_limit_bytes = 16m\n").key == "rpc.queued_output_bytes");
    CHECK(failure("rpc.header_limit_bytes = 64k\nrpc.queued_output_bytes = 1m\n").key == "rpc.queued_output_bytes");
    CHECK(failure("rpc.max_batch_entries = 65\n").key == "rpc.max_batch_entries");
    CHECK(failure("rpc.max_connections = 0\n").key == "rpc.max_connections");
    CHECK(failure("history.batch_size = 1001\n").key == "history.batch_size");
    CHECK(failure("telemetry.checkpoint_interval = 0\n").key == "telemetry.checkpoint_interval");
}

TEST_CASE("daemon configuration errors retain no rejected values", "[jobud][configuration]")
{
    constexpr auto secret = "sentinel-secret-923641";
    for (auto const* text : {"http.proxy = http://sentinel-secret-923641@proxy.test\n",
                             "unknown.sentinel-secret-923641 = 1\n",
                             "unknown.sentinel-secret-923641 = 1\nunknown.sentinel-secret-923641 = 2\n",
                             "defaults.retry.mode = sentinel-secret-923641\n",
                             "history.default_retention = sentinel-secret-923641\n",
                             "rpc.header_limit_bytes = sentinel-secret-923641\n",
                             "socket.path = sentinel-secret-923641\n"}) {
        auto error = failure(text);
        CHECK(error.code.find(secret) == std::string::npos);
        CHECK(error.message.find(secret) == std::string::npos);
        CHECK_FALSE((error.key && error.key->find(secret) != std::string::npos));
    }
}
