#include "temporary_directory.hpp"

#include "time_source.hpp"
#include "uuid.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace jb::test {

TemporaryDirectory::TemporaryDirectory()
{
    core::SystemTimeSource time_source;
    core::UuidV7Generator  uuid_generator{time_source};
    auto const             temporary_root = std::filesystem::temp_directory_path();

    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        auto const uuid = uuid_generator.generate();
        if (!uuid) {
            throw std::runtime_error{"Unable to generate a temporary directory name: " + uuid.error().message};
        }

        auto            candidate = temporary_root / ("jobu-test-" + uuid->to_string());
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            _path = std::move(candidate);
            return;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error{"Unable to create temporary directory", candidate, error};
        }
    }

    throw std::runtime_error{"Unable to create a unique temporary directory"};
}

TemporaryDirectory::~TemporaryDirectory()
{
    static_cast<void>(cleanup()); // NOLINT(bugprone-unused-return-value) Noexcept cleanup is best-effort.
}

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept
    : _path{std::exchange(other._path, std::filesystem::path{})}
{}

auto TemporaryDirectory::operator=(TemporaryDirectory&& other) noexcept -> TemporaryDirectory&
{
    if (this != &other) {
        static_cast<void>(cleanup()); // NOLINT(bugprone-unused-return-value) Noexcept cleanup is best-effort.
        _path = std::exchange(other._path, std::filesystem::path{});
    }
    return *this;
}

auto TemporaryDirectory::release() noexcept -> std::filesystem::path
{
    auto path = std::move(_path);
    _path.clear();
    return path;
}

auto TemporaryDirectory::cleanup() noexcept -> std::error_code
{
    if (_path.empty()) {
        return {};
    }

    std::error_code error;
    std::filesystem::remove_all(_path, error);
    if (!error) {
        _path.clear();
    }
    return error;
}

} // namespace jb::test
