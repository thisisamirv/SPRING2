# Included from the top-level CMakeLists.txt.

# =============================================================================
# Tests
# =============================================================================

# Tests.
enable_testing()
set(unit_test_sources
    tests/unit/unit_tests.cpp
    tests/unit/dna_parse_unit_tests.cpp
    tests/unit/assay_detector_unit_tests.cpp
    tests/unit/assay_detector_real_data_unit_tests.cpp
    tests/unit/assay_atac_strip_unit_tests.cpp
    tests/unit/assay_bisulfite_unit_tests.cpp
    tests/unit/assay_sc_rna_cb_strip_unit_tests.cpp
    tests/unit/compression_storage_plan_unit_tests.cpp
    tests/unit/archive_metadata_version_unit_tests.cpp
    tests/unit/archive_integrity_unit_tests.cpp
    tests/unit/bundle_manifest_unit_tests.cpp
    tests/unit/gzip_preprocess_progress_unit_tests.cpp
    tests/unit/disk_path_memory_reduction_unit_tests.cpp
    tests/unit/reorder_performance_unit_tests.cpp
)

add_executable(unit-tests ${unit_test_sources} ${core_source_files})
spring_enable_precompiled_headers_if_requested(unit-tests)
target_include_directories(unit-tests PRIVATE ${spring_test_include_dirs})
spring_apply_common_target_settings(unit-tests)
spring_link_project_libraries(unit-tests PRIVATE)
spring_link_windows_system_libraries(unit-tests)

target_compile_definitions(
    unit-tests
    PRIVATE
    DOCTEST_CONFIG_USE_STD_HEADERS
    UNIT_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    UNIT_TEST_REFERENCE_DIR="${CMAKE_SOURCE_DIR}/tools/assay-reference"
)
add_test(NAME unit_tests COMMAND unit-tests)

set(integration_test_sources
    tests/integration/integration_tests.cpp
    tests/integration/integration_test_support.cpp
    tests/integration/archive_integration_tests.cpp
    tests/integration/reader_integration_tests.cpp
    tests/integration/storage_path_integration_tests.cpp
    tests/integration/workflow_metadata_integration_tests.cpp
    tests/integration/assay_integration_tests.cpp
    tests/integration/grouped_archive_integration_tests.cpp
)
add_executable(
    integration-tests
    ${integration_test_sources}
    ${core_source_files}
)
spring_enable_precompiled_headers_if_requested(integration-tests)
target_include_directories(
    integration-tests
    PRIVATE ${spring_test_include_dirs}
)
spring_apply_common_target_settings(integration-tests)
spring_link_project_libraries(integration-tests PRIVATE)
spring_link_windows_system_libraries(integration-tests)

target_compile_definitions(
    integration-tests
    PRIVATE
    DOCTEST_CONFIG_USE_STD_HEADERS
    SPRING2_EXECUTABLE=\"$<TARGET_FILE:spring2>\"
    INTEGRATION_TEST_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/tests/data\"
)
add_test(NAME integration_tests COMMAND integration-tests)

set(smoke_test_sources
    tests/smoke/smoke_tests.cpp
    tests/smoke/smoke_test_support.cpp
    tests/smoke/basic_roundtrip_smoke_tests.cpp
    tests/smoke/grouped_roundtrip_smoke_tests.cpp
    tests/smoke/gzip_roundtrip_smoke_tests.cpp
    tests/smoke/cli_mode_smoke_tests.cpp
    tests/smoke/determinism_smoke_tests.cpp
)
add_executable(smoke-tests ${smoke_test_sources})
add_dependencies(smoke-tests spring2)
target_include_directories(smoke-tests PRIVATE ${spring_test_include_dirs})
spring_apply_common_target_settings(smoke-tests)
spring_link_project_libraries(smoke-tests PRIVATE)
spring_link_windows_system_libraries(smoke-tests)

target_compile_definitions(
    smoke-tests
    PRIVATE
    DOCTEST_CONFIG_USE_STD_HEADERS
    SPRING2_EXECUTABLE="$<TARGET_FILE:spring2>"
    SMOKE_TEST_ASSET_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    SMOKE_TEST_OUTPUT_ROOT="${CMAKE_SOURCE_DIR}/out/tests/smoke"
)
add_test(NAME smoke_tests COMMAND smoke-tests)
