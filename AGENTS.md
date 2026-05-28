# Repository Guidelines

## Project Structure & Module Organization

This is a C++20 CMake project. The root `CMakeLists.txt` enables C++20, exports compile commands, and adds `src` and `test`.

- `src/core/`: static `core` library sources and public headers.
- `src/core/*_priv.hpp`: private implementation headers.
- `src/core/event_loop_backend_epoll.cpp` and `event_loop_backend_kqueue.cpp`: platform-specific event loop backends selected by CMake.
- `test/`: Catch2 test executables, one file per feature, named `*-test.cpp`.
- `.bld/`: local out-of-source build directory; do not commit generated build output.

## Build, Test, and Development Commands

- `cmake -B .bld -DCMAKE_BUILD_TYPE=Debug`: configure a Debug build.
- `cmake --build .bld`: build the `core` static library and all test executables.
- `ctest --test-dir .bld/test --output-on-failure`: run the registered Catch2 tests and show failures.
- `clang-format -i src/core/*.hpp src/core/*.cpp test/*.cpp`: format changed C++ files using the repository style.

The project requires CMake 3.20+, a C++20 compiler, `fmt`, and Catch2 discoverable via CMake package config.

## Coding Style & Naming Conventions

Use the checked-in `.clang-format`. Important defaults are 4-space indentation, 120-column limit, left-aligned pointers, and custom brace wrapping with function braces on their own line. Keep public API headers in `src/core` and implementation details in `.cpp` or `*_priv.hpp` files.

Follow existing naming patterns: snake_case file names such as `event_loop.cpp`, PascalCase types such as `EventLoop`, and lowerCamelCase or descriptive method names as already used in nearby code. Prefer small, focused classes and keep platform-specific code isolated behind backend files.

## Testing Guidelines

Tests use Catch2 with `Catch2::Catch2WithMain`. Add new tests under `test/` as `feature-test.cpp`, register the executable in `test/CMakeLists.txt`, link it with `core` and Catch2, and add it with `add_test`. Keep tests behavior-focused and cover event loop, timer, object lifetime, signal, and threading changes with deterministic assertions.

## Commit & Pull Request Guidelines

Use a short subject that describes the behavior change; avoid vague subjects except for temporary local work. The subject implicitly includes "This commit changes the software to" prefix.

Pull requests should include a clear description, relevant issue links, platform notes for backend changes, and test evidence such as `ctest --test-dir .bld/test --output-on-failure`.
