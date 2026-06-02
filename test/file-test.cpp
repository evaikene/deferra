#include "file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

using namespace jb::core;

namespace {

auto test_dir(std::string_view name) -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "deferra-file-test" / std::filesystem::path{name};
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void write_file(std::filesystem::path const& path, std::string_view data)
{
    std::ofstream file{path, std::ios::binary};
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

auto read_file(std::filesystem::path const& path) -> std::string
{
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

} // anonymous namespace

TEST_CASE("File opens an existing file for reading", "[core][file]")
{
    auto const path = test_dir("read") / "data.txt";
    write_file(path, "abcdef");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadOnly));

    CHECK(file.is_open());
    CHECK(file.path() == path);
    CHECK(file.size() == 6);
    CHECK(file.bytes_available() == 6);
    CHECK(file.read(2) == "ab");
    CHECK(file.position() == 2);
    CHECK(file.read_all() == "cdef");
    CHECK(file.at_end());
}

TEST_CASE("File reports open errors for missing files without Create", "[core][file]")
{
    auto const path = test_dir("missing") / "missing.txt";

    File file;
    CHECK_FALSE(file.open(path, OpenMode::ReadOnly));
    CHECK(file.error() == IOError::OpenError);
    CHECK_FALSE(file.error_string().empty());
    CHECK_FALSE(file.is_open());
}

TEST_CASE("File creates and writes a missing file", "[core][file]")
{
    auto const path = test_dir("create-write") / "created.txt";

    File        file;
    std::size_t written = 0;
    file.bytes_written.connect([&](std::size_t bytes) -> void { written += bytes; });

    REQUIRE(file.open(path, {OpenMode::WriteOnly, OpenMode::Create}));
    CHECK(file.write("hello") == 5);
    CHECK(written == 5);
    file.close();

    CHECK(read_file(path) == "hello");
}

TEST_CASE("File clears open metadata after close", "[core][file]")
{
    auto const path = test_dir("close-clears-path") / "data.txt";
    write_file(path, "abc");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadOnly));
    REQUIRE(file.path() == path);

    file.close();

    CHECK_FALSE(file.is_open());
    CHECK(file.path().empty());
    CHECK(file.size() == 0);
    CHECK(file.error() == IOError::NotOpen);
}

TEST_CASE("File clears open metadata before failed reopen", "[core][file]")
{
    auto const dir  = test_dir("failed-reopen-clears-path");
    auto const path = dir / "data.txt";
    write_file(path, "abc");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadOnly));

    CHECK_FALSE(file.open(dir / "missing.txt", OpenMode::ReadOnly));
    CHECK(file.error() == IOError::OpenError);
    CHECK_FALSE(file.is_open());
    CHECK(file.path().empty());
    CHECK(file.size() == 0);
    CHECK(file.error() == IOError::NotOpen);
}

TEST_CASE("File preserves existing contents without Truncate", "[core][file]")
{
    auto const path = test_dir("preserve") / "data.txt";
    write_file(path, "abcdef");

    File file;
    REQUIRE(file.open(path, OpenMode::WriteOnly));
    CHECK(file.write("XY") == 2);
    file.close();

    CHECK(read_file(path) == "XYcdef");
}

TEST_CASE("File truncates existing contents when requested", "[core][file]")
{
    auto const path = test_dir("truncate") / "data.txt";
    write_file(path, "abcdef");

    File file;
    REQUIRE(file.open(path, {OpenMode::WriteOnly, OpenMode::Truncate}));
    CHECK(file.write("xy") == 2);
    file.close();

    CHECK(read_file(path) == "xy");
}

TEST_CASE("File appends writes at the end", "[core][file]")
{
    auto const path = test_dir("append") / "data.txt";
    write_file(path, "abc");

    File file;
    REQUIRE(file.open(path, {OpenMode::WriteOnly, OpenMode::Append}));
    CHECK(file.seek(0));
    CHECK(file.write("def") == 3);
    file.close();

    CHECK(read_file(path) == "abcdef");
}

TEST_CASE("File supports seek, position, and overwrite in ReadWrite mode", "[core][file]")
{
    auto const path = test_dir("seek") / "data.txt";
    write_file(path, "abcdef");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadWrite));
    REQUIRE(file.seek(3));
    CHECK(file.position() == 3);
    CHECK(file.read(2) == "de");
    REQUIRE(file.seek(1));
    CHECK(file.write("ZZ") == 2);
    file.close();

    CHECK(read_file(path) == "aZZdef");
}

TEST_CASE("File reads text lines without trailing newlines", "[core][file]")
{
    auto const path = test_dir("lines") / "data.txt";
    write_file(path, "first\nsecond\r\n\nlast");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadOnly));

    CHECK(file.can_read_line());
    CHECK(file.read_line() == "first");
    CHECK(file.read_line() == "second");
    CHECK(file.can_read_line());
    CHECK(file.read_line().empty());
    CHECK(file.can_read_line());
    CHECK(file.read_line() == "last");
    CHECK_FALSE(file.can_read_line());
}

TEST_CASE("File read_line respects max_size without requiring a newline", "[core][file]")
{
    auto const path = test_dir("line-max") / "data.txt";
    write_file(path, "abcdef\n");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadOnly));

    CHECK(file.read_line(3) == "abc");
    CHECK(file.read_line() == "def");
}

TEST_CASE("File bytes_available clears stale errors after successful query", "[core][file]")
{
    auto const path = test_dir("available-clears-error") / "data.txt";
    write_file(path, "abc");

    File file;
    REQUIRE(file.open(path, OpenMode::ReadOnly));

    CHECK(file.write("x") == 0);
    CHECK(file.error() == IOError::Unsupported);

    CHECK(file.bytes_available() == 3);
    CHECK(file.error() == IOError::NoError);
    CHECK(file.can_read_line());
}

TEST_CASE("File reports unsupported reads and writes for incompatible modes", "[core][file]")
{
    auto const path = test_dir("unsupported") / "data.txt";
    write_file(path, "abcdef");

    File write_only;
    REQUIRE(write_only.open(path, OpenMode::WriteOnly));
    CHECK(write_only.read(1).empty());
    CHECK(write_only.error() == IOError::Unsupported);
    CHECK_FALSE(write_only.can_read_line());
    CHECK(write_only.error() == IOError::Unsupported);

    File read_only;
    REQUIRE(read_only.open(path, OpenMode::ReadOnly));
    CHECK(read_only.write("x") == 0);
    CHECK(read_only.error() == IOError::Unsupported);
}

TEST_CASE("File reports not-open errors", "[core][file]")
{
    File file;

    CHECK(file.read(1).empty());
    CHECK(file.error() == IOError::NotOpen);

    CHECK_FALSE(file.can_read_line());
    CHECK(file.error() == IOError::NotOpen);

    CHECK(file.write("x") == 0);
    CHECK(file.error() == IOError::NotOpen);

    CHECK_FALSE(file.seek(1));
    CHECK(file.error() == IOError::NotOpen);
}

TEST_CASE("File rejects invalid open mode combinations", "[core][file]")
{
    auto const path = test_dir("invalid") / "data.txt";
    write_file(path, "abcdef");

    File file;
    CHECK_FALSE(file.open(path, {}));
    CHECK(file.error() == IOError::InvalidArgument);

    CHECK_FALSE(file.open(path, {OpenMode::ReadOnly, OpenMode::Append}));
    CHECK(file.error() == IOError::InvalidArgument);
}
