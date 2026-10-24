# Included from the top-level CMakeLists.txt.

# =============================================================================
# Utility functions
# =============================================================================

function(replace_in_file_if_needed file_path old_text new_text)
    if(NOT EXISTS "${file_path}")
        message(FATAL_ERROR "Cannot patch missing file: ${file_path}")
    endif()

    file(READ "${file_path}" file_contents)
    string(FIND "${file_contents}" "${new_text}" replacement_index)
    if(NOT replacement_index EQUAL -1)
        return()
    endif()

    string(
        REPLACE "${old_text}"
        "${new_text}"
        updated_contents
        "${file_contents}"
    )
    if(file_contents STREQUAL updated_contents)
        message(
            FATAL_ERROR
            "Expected text not found while patching ${file_path}"
        )
    endif()

    file(WRITE "${file_path}" "${updated_contents}")
endfunction()

function(run_command_checked working_directory description)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${working_directory}"
        RESULT_VARIABLE command_result
    )
    if(NOT command_result EQUAL 0)
        message(
            FATAL_ERROR
            "${description} failed with exit code ${command_result}"
        )
    endif()
endfunction()
