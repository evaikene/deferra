cmake_minimum_required(VERSION 3.20)

execute_process(COMMAND "${readelf}" -d "${module}"
    RESULT_VARIABLE result OUTPUT_VARIABLE dynamic ERROR_VARIABLE error)
if(NOT "${result}" STREQUAL "0" OR NOT dynamic MATCHES "NEEDED")
    message(FATAL_ERROR "Cannot inspect module dynamic dependencies: ${dynamic}\n${error}")
endif()
string(REGEX MATCHALL "Shared library: \\[[^]]+\\]" dependencies "${dynamic}")
if(NOT dependencies)
    message(FATAL_ERROR "Unsupported dynamic dependency inspection output: ${dynamic}")
endif()
set(evidence "Dynamic dependencies: ${dependencies}\n")
foreach(dependency IN LISTS dependencies)
    if(dependency MATCHES "lib(sqlite|php[.0-9-]|jobu|db[.-])")
        message(FATAL_ERROR "Production module acquired a server/Embed dependency: ${dependency}")
    endif()
endforeach()

# Client links the broad Jobu archive for domain codecs. Audit extracted members,
# not merely archive names or exported symbols: hidden server code is still code
# in the PHP process. New codec dependencies require an explicit review here.
set(allowed_jobu
    attribute attribute_registry cli_exit_policy_priv cli_job_payload_priv control_json
    history_json history_page_json history_output_json history_scalar_codec_priv
    http_job_method_priv http_job_payload_priv management_json payload_template_priv
    schedule_input_priv secret_json statistics_request_json statistics_result_json
    storage_failure_priv system_info utc_timestamp)
set(allowed_rpc client framing protocol)
set(allowed_net http_validation_priv local_socket_linux local_socket_kqueue local_socket)
file(READ "${link_map}" contents)
foreach(archive jobu rpc net)
    string(REGEX MATCHALL "lib${archive}[.]a\\([^)]*[.]o\\)" members "${contents}")
    if(NOT members)
        message(FATAL_ERROR "Link map has no selected ${archive} members; unsupported or missing map evidence")
    endif()
    list(REMOVE_DUPLICATES members)
    string(APPEND evidence "Selected ${archive} members: ${members}\n")
    foreach(member IN LISTS members)
        string(REGEX REPLACE "^lib${archive}[.]a\\((.*)[.]cpp[.]o\\)$" "\\1" name "${member}")
        if(NOT name IN_LIST allowed_${archive})
            message(FATAL_ERROR "Unreviewed implementation linked into PHP: ${member}")
        endif()
    endforeach()
endforeach()
if(contents MATCHES "lib(db|db-sqlite|jobu-sqlite|jobu-cli|jobu-http|net-http)[.]a\\(" OR
   contents MATCHES "libphp[.]a\\(" OR contents MATCHES "jobud[.]dir/[^ \n]+[.]o")
    message(FATAL_ERROR "Server/database/Embed implementation was linked into PHP")
endif()
file(WRITE "${link_map}.summary" "${evidence}")
message(STATUS "Production module uses only reviewed client/domain archive members and no server/Embed libraries")
