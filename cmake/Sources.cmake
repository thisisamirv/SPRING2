# Included from the top-level CMakeLists.txt.

# =============================================================================
# Reference data generation and source lists
# =============================================================================

# Reference embedding and shared source lists.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
set(REFERENCE_GENERATED ${CMAKE_BINARY_DIR}/generated/reference_data.cpp)

add_custom_command(
    OUTPUT ${REFERENCE_GENERATED}
    COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tools/codegen/embed_reference.py
    ${CMAKE_SOURCE_DIR}/tools/assay-reference/ref_hg38_gencode49.fa
    ${REFERENCE_GENERATED}
    DEPENDS
    ${CMAKE_SOURCE_DIR}/tools/codegen/embed_reference.py
    ${CMAKE_SOURCE_DIR}/tools/assay-reference/ref_hg38_gencode49.fa
    COMMENT "Embedding reference data into binary"
)

set(core_source_files
    # Common
    ${source_dir}/common/bitset_dictionary.cpp
    ${source_dir}/common/bgzf.cpp
    ${source_dir}/common/archive_integrity.cpp
    ${source_dir}/common/dna_utils.cpp
    ${source_dir}/common/fs_utils.cpp
    ${source_dir}/common/io_utils.cpp
    ${source_dir}/common/legacy_id_codec.cpp
    ${source_dir}/common/params.cpp
    ${source_dir}/common/parse_utils.cpp
    ${source_dir}/common/progress.cpp
    ${source_dir}/common/sha256.cpp
    # Assays
    ${source_dir}/assays/assay_atac.cpp
    ${source_dir}/assays/assay_detector.cpp
    ${source_dir}/assays/assay_bisulfite.cpp
    ${source_dir}/assays/assay_rna.cpp
    ${source_dir}/assays/assay_sc_atac.cpp
    ${source_dir}/assays/assay_sc_bisulfite.cpp
    ${source_dir}/assays/assay_sc_rna.cpp
    # Preprocess / reorder / encode
    ${source_dir}/preprocess/preprocess_artifacts.cpp
    ${source_dir}/preprocess/input_preparation.cpp
    ${source_dir}/encode/encoder.cpp
    ${source_dir}/encode/compression_dispatch_encoder.cpp
    ${source_dir}/reorder/compression_dispatch_reorder.cpp
    ${source_dir}/reorder/paired_end_mate_ordering.cpp
    ${source_dir}/reorder/quality_id_reordering.cpp
    ${source_dir}/reorder/stream_reordering.cpp
    # Decompression / archive IO
    ${source_dir}/decompress/archive_stream_reader.cpp
    ${source_dir}/decompress/archive_record_reconstruction.cpp
    ${source_dir}/decompress/decompress_archive_io.cpp
    # Workflow
    ${source_dir}/workflow/archive_preview.cpp
    ${source_dir}/workflow/compression_dispatch.cpp
    ${source_dir}/workflow/compression_workflow.cpp
    ${source_dir}/workflow/decompression_workflow.cpp
    ${source_dir}/workflow/spring_audit.cpp
    ${source_dir}/workflow/workflow_shared.cpp
    # Generated
    ${REFERENCE_GENERATED}
)

set(spring_src_include_dirs
    ${source_dir}
    ${source_dir}/assays
    ${source_dir}/common
    ${source_dir}/decompress
    ${source_dir}/encode
    ${source_dir}/preprocess
    ${source_dir}/reorder
    ${source_dir}/workflow
)

set(spring_app_include_dirs ${include_dir} ${spring_src_include_dirs})
set(spring_test_include_dirs
    ${include_dir}
    ${vendor_dir}
    ${CMAKE_SOURCE_DIR}/tests/support
    ${spring_src_include_dirs}
)
