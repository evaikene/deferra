cmake_minimum_required(VERSION 3.20)
foreach(required fixture test_logs jobu_source)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
set(client "${fixture}/client space%$'\"")
set(calls "${fixture}/calls")
set(probe "${jobu_source}/packaging/systemd/wait-ready.sh")
file(WRITE "${client}" [=[#!/bin/sh
printf '%s\n' call "$@" >> "$call_log"
count=$(awk '$0 == "call" { n++ } END { print n }' "$call_log")
if [ "$count" -ge "$ready_after" ]; then
    printf '%s\n' '{"rpc_api_version":"1.4"}'
    exit 0
fi
printf '%s\n' 'fixture RPC unavailable' >&2
exit 3
]=])
file(CHMOD "${client}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

function(check_probe name ready_after expected expected_calls)
    file(WRITE "${calls}" "")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "call_log=${calls}" "ready_after=${ready_after}"
        sh "${probe}" "${client}" /run/jobu/test.sock ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    file(WRITE "${test_logs}/${name}.log" "exit=${result}\n${output}\n${error}")
    if(NOT result STREQUAL "${expected}")
        message(FATAL_ERROR "${name} returned ${result}, expected ${expected}: ${output}\n${error}")
    endif()
    file(STRINGS "${calls}" invocations REGEX "^call$")
    list(LENGTH invocations count)
    if(NOT count EQUAL expected_calls)
        message(FATAL_ERROR "${name} made ${count} RPC attempts, expected ${expected_calls}")
    endif()
    if(expected EQUAL 0)
        if(NOT output MATCHES "rpc_api_version" OR NOT error STREQUAL "")
            message(FATAL_ERROR "${name} lost its successful RPC result")
        endif()
    elseif(NOT output STREQUAL "")
        message(FATAL_ERROR "${name} printed a success result on failure")
    endif()
endfunction()

check_probe(immediate 1 0 1)
check_probe(delayed 3 0 3 3 17)
file(READ "${calls}" arguments)
if(NOT arguments MATCHES "--socket\n/run/jobu/test.sock\n--timeout\n17\n--json\nsystem\ninfo\n")
    message(FATAL_ERROR "The probe did not preserve its explicit endpoint/deadline")
endif()
check_probe(exhausted 99 1 2 2 17)
foreach(invalid 0 01 -1 301 999999999999999999999999 1x "1:2")
    check_probe("bad-attempts-${invalid}" 1 2 0 "${invalid}" 17)
endforeach()
foreach(invalid 0 01 -1 60001 999999999999999999999999 1x "1:2")
    check_probe("bad-timeout-${invalid}" 1 2 0 1 "${invalid}")
endforeach()
message(STATUS "Bounded readiness retries, explicit RPC deadline and invalid budgets passed")
