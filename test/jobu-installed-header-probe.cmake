# Keep the probe in this build so CMake retains the selected toolchain, compiler
# flags and fmt usage requirements. Linking a JobU target would also introduce
# checkout includes and defeat the installed-header closure check.
find_package(fmt CONFIG REQUIRED)
set(JOBU_HEADER_PROBE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/jobu-header-probe")
set(JOBU_HEADER_PROBE_PREFIX "${JOBU_HEADER_PROBE_DIRECTORY}/prefix")
set(JOBU_HEADER_PROBE_SOURCES)

foreach(header IN LISTS JOBU_INSTALL_PUBLIC_HEADERS)
    string(REPLACE "/" "_" source_name "${header}")
    set(source "${JOBU_HEADER_PROBE_DIRECTORY}/${source_name}.cpp")
    file(WRITE "${source}" "#include <jb/${header}>\n")
    list(APPEND JOBU_HEADER_PROBE_SOURCES "${source}")
endforeach()

# This support header is intentionally non-standalone. Instantiate its owning
# Object/Signal API to check the inline implementation in a valid context.
set(signal_source "${JOBU_HEADER_PROBE_DIRECTORY}/object_signal.cpp")
file(WRITE "${signal_source}"
    "#include <jb/core/object.hpp>\nvoid check() { jb::core::Object receiver; jb::core::Signal<int> signal; signal.connect(&receiver, [](int) {}); }\n")
list(APPEND JOBU_HEADER_PROBE_SOURCES "${signal_source}")

add_library(jobu-installed-header-probe OBJECT EXCLUDE_FROM_ALL ${JOBU_HEADER_PROBE_SOURCES})
target_compile_features(jobu-installed-header-probe PRIVATE cxx_std_20)
target_link_libraries(jobu-installed-header-probe PRIVATE fmt::fmt)
target_include_directories(jobu-installed-header-probe PRIVATE
    "${JOBU_HEADER_PROBE_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
foreach(module IN LISTS JOBU_INSTALL_MODULES)
    target_include_directories(jobu-installed-header-probe PRIVATE
        "${JOBU_HEADER_PROBE_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/jb/${module}")
endforeach()
