#include "ini_file.hpp"

#include <string_view>
#include <type_traits>

static_assert(std::is_same_v<decltype(jb::core::IniFile::from_text(std::string_view{})), jb::core::IniFile>);

auto main() -> int
{
    auto const ini = jb::core::IniFile::from_text("key = value\n");
    return ini.ok() && ini.value("key") == "value" ? 0 : 1;
}
