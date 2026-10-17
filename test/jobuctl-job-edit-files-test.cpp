#include "job_edit_files_priv.hpp"
#include "job_edit_recovery_priv.hpp"

#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "framing.hpp"
#include "input_priv.hpp"
#include "job_edit_priv.hpp"
#include "job_payload_admission_priv.hpp"
#include "json.hpp"
#include "management.hpp"
#include "management_json.hpp"
#include "support/temporary_directory.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;

namespace {

auto parsed(std::string_view text) -> JsonValue
{
    auto value = parse_json(text);
    REQUIRE(value);
    return std::move(value).value();
}

auto serialized(JsonValue const& value) -> std::string
{
    auto text = serialize_json(value);
    REQUIRE(text);
    return std::move(text).value();
}

auto make_baseline(AttributeRegistry const& registry) -> JobEditBaseline
{
    auto job        = JobDefinition{};
    auto id         = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto at         = parse_utc_timestamp("2026-10-16T12:00:00Z");
    auto attributes = materialize_attributes(registry, {}, {}, {});
    REQUIRE(id);
    REQUIRE(at);
    REQUIRE(attributes);
    job.id         = *id;
    job.queue_id   = *id;
    job.revision   = 7;
    job.type       = JobType::Http;
    job.schedule   = OnceSchedule{.planned_at = *at};
    job.attributes = std::move(attributes).value();
    // Recovery of a metadata edit must never replace this unchanged legacy HTTP payload.
    job.payload    = parsed(R"({"url":"https://example.test","method":"get","body":{"secret":"reports.body"}})");
    job.created_at = *at;
    job.updated_at = *at;
    auto baseline  = JobEditBaseline::from_job(job, registry);
    REQUIRE(baseline);
    return std::move(baseline).value();
}

struct Fixture {
    jb::test::TemporaryDirectory root{std::filesystem::perms::owner_all};
    StandardAttributeRegistry    registry;
    JobEditBaseline              baseline{make_baseline(registry)};

    auto create(JobEditFileIo io = {}) const -> JobEditFiles
    {
        auto files = JobEditFiles::create(baseline.value(), root.path(), std::move(io));
        REQUIRE(files);
        return std::move(files).value();
    }

    auto update() const -> PreparedJobUpdate
    {
        auto draft                                             = baseline.value();
        std::get<JsonValue::Object>(draft.data).at("priority") = JsonValue{.data = std::int64_t{50}};
        auto prepared = prepare_job_update(baseline, serialized(draft), registry);
        REQUIRE(prepared);
        REQUIRE(*prepared);
        return std::move(**prepared);
    }
};

void write_text(std::filesystem::path const& path, std::string_view text)
{
    auto file = std::ofstream{path, std::ios::binary | std::ios::trunc};
    REQUIRE(file);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    REQUIRE(file);
    REQUIRE(::chmod(path.c_str(), 0600) == 0);
}

auto read_text(std::filesystem::path const& path) -> std::string
{
    auto file = std::ifstream{path, std::ios::binary};
    REQUIRE(file);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

auto metadata(std::filesystem::path const& path) -> struct stat {
    struct stat info{};
    REQUIRE(::lstat(path.c_str(), &info) == 0);
    return info;
}

} // namespace

TEST_CASE("Edit files are private, closed before editing, and retained by default", "[jobuctl][edit][files]")
{
    Fixture          fixture;
    auto             finishes = 0;
    JobEditArtifacts paths;
    {
        auto files = fixture.create({.finish = [&](int fd) {
            ++finishes;
            return ::close(fd);
        }});
        paths      = files.artifacts();
        CHECK(paths.directory.is_absolute());
        CHECK(metadata(paths.directory).st_uid == ::geteuid());
        CHECK((metadata(paths.directory).st_mode & 0777) == 0700);
        CHECK(metadata(paths.draft).st_uid == ::geteuid());
        CHECK((metadata(paths.draft).st_mode & 0777) == 0600);
        CHECK(finishes == 1);
        CHECK_FALSE(paths.request);
        auto text = files.read_draft();
        REQUIRE(text);
        CHECK(text->starts_with("jobu:"));
        auto decoded = parse_job_edit_draft(*text);
        REQUIRE(decoded);
        CHECK(serialized(*decoded) == serialized(fixture.baseline.value()));
        CHECK_FALSE(load_json_object(paths.draft));
        CHECK(text->back() == '\n');

        auto moved = std::move(files);
        CHECK(moved.artifacts().directory == paths.directory);
    }
    CHECK(std::filesystem::exists(paths.draft));
}

TEST_CASE("Draft generation failure creates no artifacts", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    oversized = JsonValue{.data = std::string(max_job_edit_draft_bytes, 'x')};
    auto    files     = JobEditFiles::create(oversized, fixture.root.path());
    REQUIRE_FALSE(files);
    CHECK(files.error().error.code == "jobuctl.edit.too_large");
    CHECK(files.error().retained_directory.empty());
    CHECK(std::filesystem::is_empty(fixture.root.path()));
}

TEST_CASE("A completed draft and minimal request both survive a failure return", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    paths = [&] {
        auto files = fixture.create();
        REQUIRE(files.write_request(fixture.update()));
        return files.artifacts();
    }();
    REQUIRE(paths.request);
    CHECK(std::filesystem::exists(paths.draft));
    auto params = parsed(read_text(*paths.request));
    CHECK(params.as_object().at("expected_revision").as_uint() == 7);
    CHECK_FALSE(params.as_object().contains("payload"));
}

TEST_CASE("Safe atomic saves are accepted without changing the edited bytes", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files       = fixture.create();
    auto    paths       = files.artifacts();
    auto    old_inode   = metadata(paths.draft).st_ino;
    auto    replacement = paths.directory / "atomic-save.tmp";
    auto    edited      = std::string{
        "jobu: editor note\r\n"
        " {\"job_id\":\"00112233-4455-6677-8899-aabbccddeeff\",\"expected_revision\":7,\"priority\":50} \n"};
    write_text(replacement, edited);
    REQUIRE(::rename(replacement.c_str(), paths.draft.c_str()) == 0);
    CHECK(metadata(paths.draft).st_ino != old_inode);
    auto text = files.read_draft();
    REQUIRE(text);
    CHECK(*text == edited);
    auto update = prepare_job_update(fixture.baseline, *text, fixture.registry);
    REQUIRE(update);
    REQUIRE(*update);
    REQUIRE(files.write_request(**update));
    paths = files.artifacts();
    REQUIRE(paths.request);
    CHECK(read_text(paths.draft) == edited);
    CHECK((metadata(*paths.request).st_mode & 0777) == 0600);
    CHECK(metadata(*paths.request).st_uid == ::geteuid());

    auto params = parsed(read_text(*paths.request));
    CHECK(serialized(params) == serialized((**update).params));
    CHECK_FALSE(params.as_object().contains("payload"));
    auto decoded = decode_command_request(CommandKind::JobUpdate, params, fixture.registry);
    REQUIRE(decoded);
    auto outgoing = update_job_request_to_json(std::get<UpdateJobRequest>(*decoded), fixture.registry);
    REQUIRE(outgoing);
    CHECK(serialized(*outgoing) == serialized(params));

    REQUIRE(files.cleanup());
    CHECK_FALSE(std::filesystem::exists(paths.directory));
    REQUIRE(files.cleanup());
}

TEST_CASE("Compact recovery requests remain loadable when readable JSON exceeds the request-file limit",
          "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    nested = JsonValue{.data = JsonValue::Array(12000, JsonValue{.data = std::string{"x"}})};
    // Additive payload data is valid, but indentation at this depth makes its readable form exceed 1 MiB.
    for (auto depth = 0; depth < 48; ++depth) {
        auto layer = JsonValue::Array{};
        layer.push_back(std::move(nested));
        nested = JsonValue{.data = std::move(layer)};
    }
    auto payload = fixture.baseline.value().as_object().at("payload");
    std::get<JsonValue::Object>(payload.data).emplace("extra", std::move(nested));
    auto admitted = jb::jobu::detail::normalize_submitted_job_payload(JobType::Http, std::move(payload));
    REQUIRE(admitted);
    auto draft                                            = fixture.baseline.value();
    std::get<JsonValue::Object>(draft.data).at("payload") = std::move(admitted).value();
    auto readable                                         = format_job_edit_json(draft);
    REQUIRE(readable);
    auto const request_file_limit = jb::rpc::FramingLimits{}.max_body_bytes;
    REQUIRE(readable->size() > request_file_limit);

    auto files = fixture.create();
    write_text(files.artifacts().draft, *readable);
    auto edited = files.read_draft();
    REQUIRE(edited);
    auto prepared = prepare_job_update(fixture.baseline, *edited, fixture.registry);
    REQUIRE(prepared);
    REQUIRE(*prepared);
    auto const& update         = **prepared;
    auto        pretty_request = format_job_edit_json(update.params);
    REQUIRE(pretty_request);
    REQUIRE(pretty_request->size() > request_file_limit);
    auto compact = serialized(update.params);
    REQUIRE(compact.size() < request_file_limit);

    REQUIRE(files.write_request(update));
    auto paths = files.artifacts();
    REQUIRE(paths.request);
    CHECK(read_text(paths.draft) == *readable);
    CHECK(read_text(*paths.request) == compact);
    auto loaded = load_json_object(*paths.request);
    REQUIRE(loaded);
    CHECK(serialized(*loaded) == compact);

    auto retry = Command{.kind = CommandKind::JobUpdate, .request_file = paths.request};
    REQUIRE(load_request_file(retry, fixture.registry));
    auto outgoing = update_job_request_to_json(std::get<UpdateJobRequest>(retry.request), fixture.registry);
    REQUIRE(outgoing);
    CHECK(serialized(*outgoing) == compact);
    REQUIRE(files.cleanup());
}

TEST_CASE("Recovery files use the inclusive request-file limit without changing draft allowance",
          "[jobuctl][edit][files]")
{
    auto const limit = jb::rpc::FramingLimits{}.max_body_bytes;
    for (auto const bytes : {limit - 1, limit, limit + 1}) {
        Fixture fixture;
        auto    files                                          = fixture.create();
        auto    original                                       = read_text(files.artifacts().draft);
        // Synthetic params isolate this file boundary from the smaller domain payload/name limits.
        auto    params                                         = parsed(R"({"padding":""})");
        auto    overhead                                       = serialized(params).size();
        std::get<JsonValue::Object>(params.data).at("padding") = JsonValue{.data = std::string(bytes - overhead, 'x')};
        auto update                                            = PreparedJobUpdate{.params = std::move(params)};
        auto written                                           = files.write_request(update);
        if (bytes <= limit) {
            REQUIRE(written);
            REQUIRE(files.artifacts().request);
            CHECK(std::filesystem::file_size(*files.artifacts().request) == bytes);
            CHECK(load_json_object(*files.artifacts().request));
        }
        else {
            REQUIRE_FALSE(written);
            CHECK(written.error().code == "jobuctl.edit.request_too_large");
            CHECK_FALSE(files.artifacts().request);
            CHECK_FALSE(std::filesystem::exists(files.artifacts().directory / "update-request.json"));
            CHECK(read_text(files.artifacts().draft) == original);
        }
        REQUIRE(files.cleanup());
    }
}

TEST_CASE("Recovery serialization rejects invalid encoding and excessive nesting before file creation",
          "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files    = fixture.create();
    auto    original = read_text(files.artifacts().draft);
    auto    update   = fixture.update();
    SECTION("invalid UTF-8")
    {
        update.params = JsonValue{.data = std::string{"\xff"}};
    }
    SECTION("excessive depth")
    {
        for (auto depth = std::size_t{0}; depth <= JsonLimits{}.max_depth; ++depth) {
            auto layer = JsonValue::Array{};
            layer.push_back(std::move(update.params));
            update.params = JsonValue{.data = std::move(layer)};
        }
    }
    auto written = files.write_request(update);
    REQUIRE_FALSE(written);
    CHECK(written.error().code == "jobuctl.edit.invalid_json");
    CHECK_FALSE(files.artifacts().request);
    CHECK_FALSE(std::filesystem::exists(files.artifacts().directory / "update-request.json"));
    CHECK(read_text(files.artifacts().draft) == original);
    REQUIRE(files.cleanup());
}

TEST_CASE("Unsafe draft replacements are rejected without following or removing them", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files = fixture.create();
    auto    paths = files.artifacts();
    REQUIRE(std::filesystem::remove(paths.draft));
    auto target = fixture.root.path() / "outside";
    write_text(target, "outside-sensitive-literal");

    SECTION("symlink")
    {
        REQUIRE(::symlink(target.c_str(), paths.draft.c_str()) == 0);
    }
    SECTION("FIFO with no writer")
    {
        REQUIRE(::mkfifo(paths.draft.c_str(), 0600) == 0);
    }
    SECTION("directory")
    {
        REQUIRE(std::filesystem::create_directory(paths.draft));
    }
    SECTION("group read permission")
    {
        write_text(paths.draft, "sensitive-literal");
        REQUIRE(::chmod(paths.draft.c_str(), 0640) == 0);
    }
    SECTION("other execute permission")
    {
        write_text(paths.draft, "sensitive-literal");
        REQUIRE(::chmod(paths.draft.c_str(), 0601) == 0);
    }
    auto text = files.read_draft();
    REQUIRE_FALSE(text);
    CHECK(text.error().message.find("sensitive-literal") == std::string::npos);
    CHECK_FALSE(files.artifacts().request);
    CHECK(std::filesystem::symlink_status(paths.draft).type() != std::filesystem::file_type::not_found);
    CHECK(read_text(target) == "outside-sensitive-literal");
}

TEST_CASE("Draft ownership and inspection errors use the opened descriptor", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    fail_stat = false;
    auto    files     = fixture.create({.inspect = [&](int fd, struct stat* info) {
        CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
        if (fail_stat) {
            errno = EIO;
            return -1;
        }
        auto result  = ::fstat(fd, info);
        info->st_uid = ::geteuid() == 0 ? 1 : 0;
        return result;
    }});
    CHECK_FALSE(files.read_draft());
    fail_stat = true;
    CHECK_FALSE(files.read_draft());
    CHECK(std::filesystem::exists(files.artifacts().draft));
    REQUIRE(files.cleanup());
}

TEST_CASE("Missing empty and invalid drafts preserve recovery evidence without a minimal request",
          "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files = fixture.create();
    auto    paths = files.artifacts();
    SECTION("missing")
    {
        REQUIRE(std::filesystem::remove(paths.draft));
        auto missing = files.read_draft();
        REQUIRE_FALSE(missing);
        CHECK(missing.error().code == "jobuctl.edit.missing_draft");
    }
    SECTION("empty")
    {
        write_text(paths.draft, "");
        auto text = files.read_draft();
        REQUIRE(text);
        CHECK_FALSE(prepare_job_update(fixture.baseline, *text, fixture.registry));
        CHECK(read_text(paths.draft).empty());
    }
    SECTION("invalid JSON")
    {
        write_text(paths.draft, "{invalid sensitive-literal");
        auto text = files.read_draft();
        REQUIRE(text);
        auto update = prepare_job_update(fixture.baseline, *text, fixture.registry);
        REQUIRE_FALSE(update);
        CHECK(update.error().message.find("sensitive-literal") == std::string::npos);
        CHECK(read_text(paths.draft) == *text);
    }
    CHECK_FALSE(files.artifacts().request);
    CHECK(std::filesystem::exists(paths.directory));
}

TEST_CASE("Draft reads enforce the byte bound during accumulation", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files = fixture.create();
    auto    text  = std::string(max_job_edit_draft_bytes, ' ');
    write_text(files.artifacts().draft, text);
    auto accepted = files.read_draft();
    REQUIRE(accepted);
    CHECK(accepted->size() == max_job_edit_draft_bytes);
    text += ' ';
    write_text(files.artifacts().draft, text);
    auto rejected = files.read_draft();
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobuctl.edit.too_large");
    CHECK(std::filesystem::file_size(files.artifacts().draft) == max_job_edit_draft_bytes + 1);
    REQUIRE(files.cleanup());
}

TEST_CASE("Interrupted short and failed reads never overwrite a draft", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    reads     = 0;
    auto    fail_read = false;
    auto    files     = fixture.create({.read = [&](int fd, void* data, std::size_t size) -> ssize_t {
        if (++reads == 1) {
            errno = EINTR;
            return -1;
        }
        if (fail_read && reads > 3) {
            errno = EIO;
            return -1;
        }
        return ::read(fd, data, std::min(size, std::size_t{13}));
    }});
    auto    original  = read_text(files.artifacts().draft);
    auto    text      = files.read_draft();
    REQUIRE(text);
    CHECK(*text == original);
    reads       = 0;
    fail_read   = true;
    auto failed = files.read_draft();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "jobuctl.edit.read_failed");
    CHECK(read_text(files.artifacts().draft) == original);
    REQUIRE(files.cleanup());
}

TEST_CASE("Interrupted short writes finish complete protected recovery files", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    writes = 0;
    auto    files  = fixture.create({.write = [&](int fd, void const* data, std::size_t size) -> ssize_t {
        CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
        if (++writes == 1) {
            errno = EINTR;
            return -1;
        }
        return ::write(fd, data, std::min(size, std::size_t{17}));
    }});
    writes         = 0;
    auto update    = fixture.update();
    REQUIRE(files.write_request(update));
    REQUIRE(files.artifacts().request);
    CHECK(serialized(parsed(read_text(*files.artifacts().request))) == serialized(update.params));
    CHECK_FALSE(files.write_request(update));
    REQUIRE(files.cleanup());
}

TEST_CASE("Partial request write and finish failures retain the draft without publishing a request",
          "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    failing       = false;
    auto    writes        = 0;
    auto    close_failure = false;
    auto    zero_write    = false;
    auto    files         = fixture.create({.write = [&](int fd, void const* data, std::size_t size) -> ssize_t {
                                     if (failing && !close_failure && ++writes > 1) {
                                         errno = EIO;
                                         return zero_write ? 0 : -1;
                                     }
                                     return ::write(fd, data, std::min(size, std::size_t{5}));
                                            },
                                            .finish =
                                                [&](int fd) {
                                         auto result = ::close(fd);
                                         if (failing && close_failure) {
                                             errno = EIO;
                                             return -1;
                                         }
                                         return result;
                                                }});
    SECTION("write error")
    {}
    SECTION("zero progress")
    {
        zero_write = true;
    }
    SECTION("close error")
    {
        close_failure = true;
    }
    auto paths    = files.artifacts();
    auto original = read_text(paths.draft);
    failing       = true;
    auto written  = files.write_request(fixture.update());
    REQUIRE_FALSE(written);
    CHECK(written.error().code == "jobuctl.edit.write_failed");
    CHECK_FALSE(files.artifacts().request);
    CHECK_FALSE(std::filesystem::exists(paths.directory / "update-request.json"));
    CHECK(read_text(paths.draft) == original);
}

TEST_CASE("Initial file failures clean known files and report unexpected retained entries", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    keep_backup = false;
    SECTION("ordinary creation failure")
    {}
    SECTION("unexpected backup prevents directory cleanup")
    {
        keep_backup = true;
    }
    auto writes = 0;
    auto files  = JobEditFiles::create(fixture.baseline.value(),
                                       fixture.root.path(),
                                       {.write = [&](int fd, void const* data, std::size_t size) -> ssize_t {
                                          if (++writes == 1) {
                                              return ::write(fd, data, std::min(size, std::size_t{5}));
                                          }
                                          if (keep_backup) {
                                              auto child =
                                                  std::filesystem::directory_iterator{fixture.root.path()}->path();
                                              write_text(child / "unexpected.bak", "backup");
                                          }
                                          errno = ENOSPC;
                                          return -1;
                                       }});
    REQUIRE_FALSE(files);
    if (keep_backup) {
        REQUIRE_FALSE(files.error().retained_directory.empty());
        CHECK(read_text(files.error().retained_directory / "unexpected.bak") == "backup");
        CHECK_FALSE(std::filesystem::exists(files.error().retained_directory / "draft.json"));
    }
    else {
        CHECK(files.error().retained_directory.empty());
        CHECK(std::filesystem::is_empty(fixture.root.path()));
    }
}

TEST_CASE("Cleanup removes known files but never recurses into unexpected backups", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files = fixture.create();
    REQUIRE(files.write_request(fixture.update()));
    auto paths  = files.artifacts();
    auto backup = paths.directory / "editor-backup";
    REQUIRE(std::filesystem::create_directory(backup));
    write_text(backup / "keep", "backup contents");
    REQUIRE(::symlink(fixture.root.path().c_str(), (paths.directory / "backup-link").c_str()) == 0);
    auto cleaned = files.cleanup();
    REQUIRE_FALSE(cleaned);
    CHECK(cleaned.error().code == "jobuctl.edit.cleanup_failed");
    CHECK(files.artifacts().directory == paths.directory);
    CHECK_FALSE(std::filesystem::exists(paths.draft));
    CHECK_FALSE(std::filesystem::exists(*paths.request));
    CHECK(read_text(backup / "keep") == "backup contents");
    CHECK(std::filesystem::is_symlink(paths.directory / "backup-link"));
}

TEST_CASE("Cleanup preserves unowned request entries and replaced directories", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files = fixture.create();
    auto    paths = files.artifacts();
    SECTION("preexisting request")
    {
        auto path = paths.directory / "update-request.json";
        write_text(path, "preexisting");
        CHECK_FALSE(files.write_request(fixture.update()));
        CHECK_FALSE(files.artifacts().request);
        CHECK_FALSE(files.cleanup());
        CHECK(read_text(path) == "preexisting");
    }
    SECTION("preexisting request symlink")
    {
        auto path   = paths.directory / "update-request.json";
        auto target = fixture.root.path() / "outside";
        write_text(target, "untouched");
        REQUIRE(::symlink(target.c_str(), path.c_str()) == 0);
        CHECK_FALSE(files.write_request(fixture.update()));
        CHECK_FALSE(files.artifacts().request);
        CHECK_FALSE(files.cleanup());
        CHECK(std::filesystem::is_symlink(path));
        CHECK(read_text(target) == "untouched");
    }
    SECTION("replacement request inode")
    {
        REQUIRE(files.write_request(fixture.update()));
        auto path        = *files.artifacts().request;
        auto replacement = paths.directory / "replacement";
        write_text(replacement, "replacement");
        REQUIRE(::rename(replacement.c_str(), path.c_str()) == 0);
        CHECK_FALSE(files.cleanup());
        CHECK(read_text(path) == "replacement");
    }
    SECTION("renamed directory")
    {
        auto moved = fixture.root.path() / "moved";
        std::filesystem::rename(paths.directory, moved);
        REQUIRE(std::filesystem::create_directory(paths.directory));
        write_text(paths.directory / "draft.json", "unrelated");
        CHECK_FALSE(files.cleanup());
        CHECK(read_text(paths.directory / "draft.json") == "unrelated");
        CHECK(std::filesystem::exists(moved / "draft.json"));
    }
}

TEST_CASE("No-op preparation has no recovery request and permits explicit cleanup", "[jobuctl][edit][files]")
{
    Fixture fixture;
    auto    files = fixture.create();
    auto    text  = files.read_draft();
    REQUIRE(text);
    auto update = prepare_job_update(fixture.baseline, *text, fixture.registry);
    REQUIRE(update);
    CHECK_FALSE(*update);
    CHECK_FALSE(files.artifacts().request);
    auto path = files.artifacts().directory;
    REQUIRE(files.cleanup());
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Recovery commands quote argv as data without shell evaluation", "[jobuctl][edit][recovery]")
{
    auto arguments =
        std::array{std::string{}, std::string{"a'b"}, std::string{"$(touch never); `echo no` $HOME * > | \\ \"\n"}};
    auto quoted = quote_shell_argv(arguments);
    REQUIRE(quoted);
    CHECK(*quoted == "'' 'a'\\''b' '$(touch never); `echo no` $HOME * > | \\ \"\n'");
    auto invalid = std::array{
        std::string{"sensitive\0value", 15}
    };
    auto failed = quote_shell_argv(invalid);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().message.find("sensitive") == std::string::npos);
}

TEST_CASE("Recovery preserves connection settings and the existing request-file grammar", "[jobuctl][edit][recovery]")
{
    auto command = Command{.socket_path = "/tmp/daemon's socket", .timeout = std::chrono::milliseconds{12345}};
    auto paths   = JobEditArtifacts{.directory = "/tmp/edit's directory",
                                    .draft     = "/tmp/edit's directory/draft.json",
                                    .request   = "/tmp/edit's directory/update-request.json"};
    for (auto reason : {JobEditRecoveryReason::SubmissionFailed,
                        JobEditRecoveryReason::RevisionConflict,
                        JobEditRecoveryReason::OutcomeUnknown}) {
        auto advice = format_job_edit_recovery(command, reason, paths);
        REQUIRE(advice);
        REQUIRE(advice->retry_command);
        CHECK(*advice->retry_command == "'jobuctl' '--socket' '/tmp/daemon'\\''s socket' '--timeout' '12345' 'job' "
                                        "'update' '--request-file' '/tmp/edit'\\''s directory/update-request.json'");
        if (reason == JobEditRecoveryReason::RevisionConflict) {
            CHECK(advice->instructions.find("old revision") != std::string::npos);
            CHECK(advice->instructions.find("manually reconcile") != std::string::npos);
        }
        if (reason == JobEditRecoveryReason::OutcomeUnknown) {
            CHECK(advice->instructions.find("Inspect current server state before any retry") != std::string::npos);
        }
    }
    auto invalid = format_job_edit_recovery(command, JobEditRecoveryReason::InvalidDraft, paths);
    REQUIRE(invalid);
    CHECK_FALSE(invalid->retry_command);
    CHECK_FALSE(invalid->paths.request);
    CHECK(invalid->instructions.find("every explicitly present field") != std::string::npos);
    CHECK(invalid->instructions.find("removing any jobu: instruction lines") != std::string::npos);
    CHECK(invalid->instructions.find("strict JSON") != std::string::npos);
    paths.request.reset();
    auto no_request = format_job_edit_recovery(command, JobEditRecoveryReason::SubmissionFailed, paths);
    REQUIRE(no_request);
    CHECK_FALSE(no_request->retry_command);

    auto arguments = std::vector<std::string>{"jobuctl",
                                              "--socket",
                                              command.socket_path.string(),
                                              "--timeout",
                                              "12345",
                                              "job",
                                              "update",
                                              "--request-file",
                                              "/tmp/edit's directory/update-request.json"};
    auto argv      = std::vector<char*>{};
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    StandardAttributeRegistry registry;
    auto                      parsed_command = parse_command_line(static_cast<int>(argv.size()), argv.data(), registry);
    REQUIRE(parsed_command.action);
    auto const& recovered = std::get<Command>(*parsed_command.action);
    CHECK(recovered.kind == CommandKind::JobUpdate);
    CHECK(recovered.socket_path == command.socket_path);
    CHECK(recovered.timeout == command.timeout);
    CHECK(recovered.request_file == "/tmp/edit's directory/update-request.json");
}
