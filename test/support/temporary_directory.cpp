#include "temporary_directory.hpp"

#include "time_source.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace jb::test {

TemporaryDirectory::TemporaryDirectory()
{
    core::SystemTimeSource time_source;
    core::UuidV7Generator  uuid_generator{time_source};
    auto const             uuid = uuid_generator.generate();
    if (!uuid) {
        throw std::runtime_error{"Unable to generate a temporary directory name: " + uuid.error().message};
    }

    _path = std::filesystem::temp_directory_path() / ("jobu-test-" + uuid->to_string());
    std::error_code error;
    if (!std::filesystem::create_directory(_path, error)) {
        throw std::filesystem::filesystem_error{"Unable to create temporary directory", _path, error};
    }
}

TemporaryDirectory::~TemporaryDirectory()
{
    static_cast<void>(cleanup());
}

TemporaryDirectory::TemporaryDirectory(TemporaryDirectory&& other) noexcept
    : _path{std::move(other._path)}
{}

auto TemporaryDirectory::operator=(TemporaryDirectory&& other) noexcept -> TemporaryDirectory&
{
    if (this != &other) {
        static_cast<void>(cleanup());
        _path = std::move(other._path);
    }
    return *this;
}

auto TemporaryDirectory::release() noexcept -> std::filesystem::path
{
    return std::move(_path);
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
