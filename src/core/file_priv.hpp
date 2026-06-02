#pragma once

#include "file.hpp"
#include "io_device_priv.hpp"

#include <filesystem>
#include <fstream>

namespace jb::core::priv {

struct FilePrivate : IODevicePrivate {
    std::fstream          stream;
    std::filesystem::path path;
    OpenModes             modes;
};

} // namespace jb::core::priv
