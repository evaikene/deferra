---
name: cpp-expert
description: >
  Modern C++ development expertise. Use for tasks involving
  RAII, templates, performance optimization, build systems
  (CMake), memory management, and concurrency patterns.
---

# C++ Expert Skill

## Standards & Practices
- Default to C++20 unless otherwise specified
- Prefer RAII and smart pointers over raw memory
- Use `std::span`, ranges, and concepts where appropriate
- Favor composition over inheritance; use `final` for classes not meant to be inherited
- Reuse existing classes and functions when possible; avoid unnecessary abstraction

## Build System
- CMake is the primary build tool for this project
- Run: `cmake -B .bld && cmake --build .bld`

## Testing
- Catch2 is the testing framework of choice
- Run: `cd .bld/test && ctest`

## Style
- Formatting is defined by `.clang-format` in the project root
- 4-space indentation, `snake_case` for functions, `PascalCase` for types, and `kMyConstant` for constants.
- Class members are prefixed with `_` (e.g., `_member_variable`)
