include(CMakePackageConfigHelpers)

get_property(JOBU_SDK_COMPONENTS GLOBAL PROPERTY JOBU_INSTALL_SDK_COMPONENTS)
list(REMOVE_DUPLICATES JOBU_SDK_COMPONENTS)
set(JOBU_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/JobU")

foreach(component IN LISTS JOBU_SDK_COMPONENTS)
    install(EXPORT JobU${component}Targets
        FILE JobU${component}Targets.cmake NAMESPACE JobU::
        DESTINATION "${JOBU_INSTALL_CMAKEDIR}" COMPONENT Development)
endforeach()

configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/JobUConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/jobu-generated/JobUConfig.cmake"
    INSTALL_DESTINATION "${JOBU_INSTALL_CMAKEDIR}")
# The unreleased SDK makes no cross-version ABI guarantee. Keep the normal
# architecture check: these are compiled archives, not a header-only package.
write_basic_package_version_file("${PROJECT_BINARY_DIR}/jobu-generated/JobUConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES
    "${PROJECT_BINARY_DIR}/jobu-generated/JobUConfig.cmake"
    "${PROJECT_BINARY_DIR}/jobu-generated/JobUConfigVersion.cmake"
    DESTINATION "${JOBU_INSTALL_CMAKEDIR}" COMPONENT Development)
