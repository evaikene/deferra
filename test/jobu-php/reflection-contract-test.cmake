cmake_minimum_required(VERSION 3.20)

execute_process(COMMAND "${php_binary}" -n
    "${CMAKE_CURRENT_LIST_DIR}/reflection-contract.php" "${stub}"
    RESULT_VARIABLE result OUTPUT_VARIABLE expected ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${result}" STREQUAL "0" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Stub Reflection failed: ${expected}\n${error}")
endif()
execute_process(COMMAND "${php_binary}" -n "-dextension=${module}"
    "${CMAKE_CURRENT_LIST_DIR}/reflection-contract.php"
    RESULT_VARIABLE result OUTPUT_VARIABLE actual ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${result}" STREQUAL "0" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Module Reflection failed: ${actual}\n${error}")
endif()
if(NOT actual STREQUAL expected)
    file(WRITE "${output_dir}/reflection-expected.json" "${expected}")
    file(WRITE "${output_dir}/reflection-actual.json" "${actual}")
    message(FATAL_ERROR "Reflection contract differs; compare ${output_dir}/reflection-{expected,actual}.json")
endif()

# Native enums have an enum flag without the userland final flag. Verify the
# non-inheritable contract itself rather than requiring identical engine flags.
string(JSON count LENGTH "${actual}")
math(EXPR last "${count} - 1")
foreach(index RANGE 0 ${last})
    string(JSON name MEMBER "${actual}" ${index})
    string(JSON kind TYPE "${actual}" "${name}" enum)
    if(NOT kind STREQUAL "NULL")
        execute_process(COMMAND "${php_binary}" -n "-dextension=${module}"
            -r "class InvalidEnumChild extends ${name} {}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
        if(NOT "${result}" STREQUAL "255" OR
           NOT "${output}${error}" MATCHES "cannot extend (enum|final class)|Non-enum class InvalidEnumChild cannot implement interface UnitEnum")
            message(FATAL_ERROR "Enum inheritance was not rejected for ${name}: ${output}\n${error}")
        endif()
    endif()
endforeach()
