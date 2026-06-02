#pragma once

#include "io_device.hpp"
#include "object_priv.hpp"

#include <string>

namespace jb::core::priv {

struct IODevicePrivate : ObjectPrivate {
    IOError     error{IOError::NoError};
    std::string error_string;
};

} // namespace jb::core::priv
