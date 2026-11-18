add_library(tui STATIC tui.cpp)
target_compile_features(tui PUBLIC cxx_std_20)
target_link_libraries(tui PUBLIC core)

jobu_install_library(tui tui COMPONENT Tui HEADERS geometry.hpp)
