#include "file.hpp"

#include "file_priv.hpp"

#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

namespace jb::core {

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
    : IODevice(*new priv::FilePrivate, parent)
{}

File::~File() = default;

auto File::open(std::filesystem::path path, OpenModes modes) -> bool
{
    if (is_open()) {
        close();
    }

    auto* d = d_ptr<priv::FilePrivate>();

    clear_error();
    d->path.clear();
    d->modes.reset();

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

    d->stream.clear();
    d->stream.open(path, make_open_mode(modes));
    if (!d->stream.is_open()) {
        return fail(IOError::OpenError, "failed to open file");
    }

    d->path  = std::move(path);
    d->modes = modes;

    return true;
}

auto File::is_open() const -> bool
{
    return d_ptr<priv::FilePrivate const>()->stream.is_open();
}

void File::close()
{
    if (!is_open()) {
        return;
    }

    auto* d = d_ptr<priv::FilePrivate>();

    clear_error();
    d->stream.close();
    if (d->stream.fail()) {
        set_error(IOError::CloseError, "failed to close file");
    }
    d->path.clear();
    d->modes.reset();

    emit_closed();
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

    auto* d = d_ptr<priv::FilePrivate>();

    std::string data(max_size, '\0');
    d->stream.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(d->stream.gcount()));

    if (d->stream.bad()) {
        set_error(IOError::ReadError, "failed to read file");
        return {};
    }

    if (d->stream.eof()) {
        d->stream.clear();
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

    auto* d = d_ptr<priv::FilePrivate>();

    constexpr std::size_t kMaxLineSize = 256U;

    std::string line;
    line.reserve(std::min(max_size, kMaxLineSize));

    while (line.size() < max_size) {
        char ch = '\0';
        if (!d->stream.get(ch)) {
            break;
        }

        line.push_back(ch);
        if (ch == '\n') {
            break;
        }
    }

    if (d->stream.bad()) {
        set_error(IOError::ReadError, "failed to read file");
        return {};
    }

    if (d->stream.eof()) {
        d->stream.clear();
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

    auto* d = d_ptr<priv::FilePrivate>();

    if (has_mode(OpenMode::Append)) {
        d->stream.seekp(0, std::ios::end);
    }

    d->stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!d->stream) {
        set_error(IOError::WriteError, "failed to write file");
        return 0;
    }

    d->stream.flush();
    if (!d->stream) {
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

    auto& stream  = d_ptr<priv::FilePrivate>()->stream;
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

    const_cast<File*>(this)->clear_error();

    return static_cast<std::size_t>(end - current);
}

auto File::path() const -> std::filesystem::path const&
{
    return d_ptr<priv::FilePrivate const>()->path;
}

auto File::size() const -> std::size_t
{
    auto const* d = d_ptr<priv::FilePrivate const>();

    if (d->path.empty()) {
        const_cast<File*>(this)->set_error(IOError::NotOpen, "file is not open");
        return 0;
    }

    std::error_code ec;
    auto const      size = std::filesystem::file_size(d->path, ec);
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

    auto& stream = d_ptr<priv::FilePrivate>()->stream;
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

    auto* d = d_ptr<priv::FilePrivate>();

    clear_error();
    d->stream.clear();
    if (can_read()) {
        d->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    }
    if (can_write() && !has_mode(OpenMode::Append)) {
        d->stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    }

    if (!d->stream) {
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
    return d_ptr<priv::FilePrivate const>()->modes.test(mode);
}

auto File::fail(IOError error, std::string message) -> bool
{
    set_error(error, std::move(message));

    return false;
}

} // namespace jb::core
