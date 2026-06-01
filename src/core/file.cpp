#include "file.hpp"

#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

namespace jb::core {

struct File::Private {
    std::fstream          stream;
    std::filesystem::path path;
    OpenModes             modes;
};

namespace {

auto filesystem_error(std::error_code const& ec) -> std::string
{
    return ec ? ec.message() : std::string{"unknown filesystem error"};
}

auto make_open_mode(OpenModes modes) -> std::ios::openmode
{
    auto open_mode = std::ios::openmode{};

    if (!modes.test(OpenMode::Text)) {
        open_mode |= std::ios::binary;
    }

    auto const readable = modes.test(OpenMode::ReadOnly);
    auto const writable = modes.test(OpenMode::WriteOnly);

    if (readable) {
        open_mode |= std::ios::in;
    }

    if (writable) {
        open_mode |= std::ios::out;
    }
    if (writable && !modes.test(OpenMode::Truncate) && !modes.test(OpenMode::Append)) {
        open_mode |= std::ios::in;
    }
    if (modes.test(OpenMode::Append)) {
        open_mode |= std::ios::app;
    }
    if (modes.test(OpenMode::Truncate)) {
        open_mode |= std::ios::trunc;
    }

    return open_mode;
}

void strip_crlf(std::string& line)
{
    if (!line.empty() && line.back() == '\n') {
        line.pop_back();
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }
}

} // anonymous namespace

File::File(Object* parent)
    : IODevice(parent)
    , _d_file(new Private)
{}

File::~File()
{
    delete _d_file;
}

auto File::open(std::filesystem::path path, OpenModes modes) -> bool
{
    if (is_open()) {
        close();
    }

    clear_error();

    auto const readable = modes.test(OpenMode::ReadOnly);
    auto const writable = modes.test(OpenMode::WriteOnly);

    if (!readable && !writable) {
        return fail(IOError::InvalidArgument, "file open mode must include reading or writing");
    }
    if ((modes.test(OpenMode::Append) || modes.test(OpenMode::Truncate)) && !writable) {
        return fail(IOError::InvalidArgument, "append and truncate require a writable file mode");
    }

    std::error_code ec;
    auto const      exists = std::filesystem::exists(path, ec);
    if (ec) {
        return fail(IOError::OpenError, filesystem_error(ec));
    }
    if (!exists && !modes.test(OpenMode::Create)) {
        return fail(IOError::OpenError, "file does not exist");
    }

    if (!exists && modes.test(OpenMode::Create)) {
        auto create_mode = std::ios::out;
        if (!modes.test(OpenMode::Text)) {
            create_mode |= std::ios::binary;
        }

        std::ofstream create_file{path, create_mode};
        if (!create_file.is_open()) {
            return fail(IOError::OpenError, "failed to create file");
        }
    }

    _d_file->stream.clear();
    _d_file->stream.open(path, make_open_mode(modes));
    if (!_d_file->stream.is_open()) {
        return fail(IOError::OpenError, "failed to open file");
    }

    _d_file->path  = std::move(path);
    _d_file->modes = modes;
    return true;
}

auto File::is_open() const -> bool
{
    return _d_file->stream.is_open();
}

void File::close()
{
    if (!is_open()) {
        return;
    }

    clear_error();
    _d_file->stream.close();
    if (_d_file->stream.fail()) {
        set_error(IOError::CloseError, "failed to close file");
    }
}

auto File::read(std::size_t max_size) -> std::string
{
    if (!is_open()) {
        set_error(IOError::NotOpen, "file is not open");
        return {};
    }
    if (!can_read()) {
        set_error(IOError::Unsupported, "file is not open for reading");
        return {};
    }

    clear_error();

    std::string data(max_size, '\0');
    _d_file->stream.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(_d_file->stream.gcount()));

    if (_d_file->stream.bad()) {
        set_error(IOError::ReadError, "failed to read file");
        return {};
    }

    if (_d_file->stream.eof()) {
        _d_file->stream.clear();
    }

    return data;
}

auto File::read_all() -> std::string
{
    if (!is_open()) {
        set_error(IOError::NotOpen, "file is not open");
        return {};
    }
    if (!can_read()) {
        set_error(IOError::Unsupported, "file is not open for reading");
        return {};
    }

    auto const available = bytes_available();
    if (error() != IOError::NoError) {
        return {};
    }
    return read(available);
}

auto File::read_line(std::size_t max_size) -> std::string
{
    if (!is_open()) {
        set_error(IOError::NotOpen, "file is not open");
        return {};
    }
    if (!can_read()) {
        set_error(IOError::Unsupported, "file is not open for reading");
        return {};
    }

    clear_error();

    std::string line;
    line.reserve(std::min<std::size_t>(max_size, 256));

    while (line.size() < max_size) {
        char ch = '\0';
        if (!_d_file->stream.get(ch)) {
            break;
        }

        line.push_back(ch);
        if (ch == '\n') {
            break;
        }
    }

    if (_d_file->stream.bad()) {
        set_error(IOError::ReadError, "failed to read file");
        return {};
    }

    if (_d_file->stream.eof()) {
        _d_file->stream.clear();
    }

    strip_crlf(line);
    return line;
}

auto File::can_read_line() const -> bool
{
    return bytes_available() > 0 && error() == IOError::NoError;
}

auto File::write(std::string_view data) -> std::size_t
{
    if (!is_open()) {
        set_error(IOError::NotOpen, "file is not open");
        return 0;
    }
    if (!can_write()) {
        set_error(IOError::Unsupported, "file is not open for writing");
        return 0;
    }

    clear_error();
    if (has_mode(OpenMode::Append)) {
        _d_file->stream.seekp(0, std::ios::end);
    }

    _d_file->stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!_d_file->stream) {
        set_error(IOError::WriteError, "failed to write file");
        return 0;
    }

    _d_file->stream.flush();
    if (!_d_file->stream) {
        set_error(IOError::WriteError, "failed to flush file");
        return 0;
    }

    emit_bytes_written(data.size());
    return data.size();
}

auto File::bytes_available() const -> std::size_t
{
    if (!is_open()) {
        const_cast<File*>(this)->set_error(IOError::NotOpen, "file is not open");
        return 0;
    }
    if (!can_read()) {
        const_cast<File*>(this)->set_error(IOError::Unsupported, "file is not open for reading");
        return 0;
    }

    auto& stream  = _d_file->stream;
    auto  current = stream.tellg();
    if (current < 0) {
        const_cast<File*>(this)->set_error(IOError::ReadError, "failed to query file position");
        return 0;
    }

    stream.seekg(0, std::ios::end);
    auto end = stream.tellg();
    stream.seekg(current);

    if (end < 0) {
        const_cast<File*>(this)->set_error(IOError::ReadError, "failed to query file size");
        return 0;
    }

    return static_cast<std::size_t>(end - current);
}

auto File::path() const -> std::filesystem::path const&
{
    return _d_file->path;
}

auto File::size() const -> std::size_t
{
    if (_d_file->path.empty()) {
        const_cast<File*>(this)->set_error(IOError::NotOpen, "file is not open");
        return 0;
    }

    std::error_code ec;
    auto const      size = std::filesystem::file_size(_d_file->path, ec);
    if (ec) {
        const_cast<File*>(this)->set_error(IOError::ResourceError, filesystem_error(ec));
        return 0;
    }

    const_cast<File*>(this)->clear_error();
    return static_cast<std::size_t>(size);
}

auto File::position() const -> std::size_t
{
    if (!is_open()) {
        const_cast<File*>(this)->set_error(IOError::NotOpen, "file is not open");
        return 0;
    }

    auto& stream = _d_file->stream;
    auto  pos    = can_read() ? stream.tellg() : stream.tellp();
    if (pos < 0) {
        const_cast<File*>(this)->set_error(IOError::SeekError, "failed to query file position");
        return 0;
    }

    const_cast<File*>(this)->clear_error();
    return static_cast<std::size_t>(pos);
}

auto File::seek(std::size_t offset) -> bool
{
    if (!is_open()) {
        return fail(IOError::NotOpen, "file is not open");
    }

    clear_error();
    _d_file->stream.clear();
    if (can_read()) {
        _d_file->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    }
    if (can_write() && !has_mode(OpenMode::Append)) {
        _d_file->stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    }

    if (!_d_file->stream) {
        return fail(IOError::SeekError, "failed to seek file");
    }
    return true;
}

auto File::at_end() const -> bool
{
    return bytes_available() == 0 && error() == IOError::NoError;
}

auto File::can_read() const -> bool
{
    return has_mode(OpenMode::ReadOnly);
}

auto File::can_write() const -> bool
{
    return has_mode(OpenMode::WriteOnly);
}

auto File::has_mode(OpenMode mode) const -> bool
{
    return _d_file->modes.test(mode);
}

auto File::fail(IOError error, std::string message) -> bool
{
    set_error(error, std::move(message));
    return false;
}

} // namespace jb::core
