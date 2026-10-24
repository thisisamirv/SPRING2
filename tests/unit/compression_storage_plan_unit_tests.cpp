#include "../support/doctest.h"

#include "../../src/workflow/workflow_internal.h"

#include <filesystem>

namespace {

std::string sample_asset_path(const std::string &name) {
#ifdef UNIT_TEST_ASSET_DIR
  return (std::filesystem::path(UNIT_TEST_ASSET_DIR) / name).generic_string();
#else
  return (std::filesystem::path("..") / ".." / "data" / "samples" / name)
      .generic_string();
#endif
}

double bytes_to_gib(const uint64_t bytes) {
  return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

} // namespace

TEST_CASE("Compression storage plan uses peak intermediate memory margin") {
  const spring::string_list input_paths = {sample_asset_path("test_1.fastq")};
  const spring::compression_storage_plan baseline_plan =
      spring::build_compression_storage_plan(input_paths, 1024.0);

  CHECK(baseline_plan.estimated_input_bytes > 0);
  CHECK(baseline_plan.estimated_peak_intermediate_bytes >
        baseline_plan.estimated_input_bytes);
  CHECK(baseline_plan.safety_margin_bytes > 0);
  CHECK(baseline_plan.required_peak_memory_bytes >
        baseline_plan.estimated_peak_intermediate_bytes);

  const uint64_t one_mebibyte = 1024ULL * 1024ULL;
  REQUIRE(baseline_plan.required_peak_memory_bytes > one_mebibyte);

  const spring::compression_storage_plan below_threshold_plan =
      spring::build_compression_storage_plan(
          input_paths, bytes_to_gib(baseline_plan.required_peak_memory_bytes -
                                    one_mebibyte));
  CHECK(below_threshold_plan.selected_path ==
        spring::compression_storage_path::disk_path);

  const spring::compression_storage_plan above_threshold_plan =
      spring::build_compression_storage_plan(
          input_paths, bytes_to_gib(baseline_plan.required_peak_memory_bytes +
                                    one_mebibyte));
  CHECK(above_threshold_plan.selected_path ==
        spring::compression_storage_path::memory_path);
}

TEST_CASE("Compression storage route override bypasses auto planner") {
  const spring::string_list input_paths = {sample_asset_path("test_1.fastq")};
  const spring::compression_storage_plan tiny_budget_memory =
      spring::build_compression_storage_plan(
          input_paths, 0.00001, spring::CompressionStorageRoute::memory);
  CHECK(tiny_budget_memory.selected_path ==
        spring::compression_storage_path::memory_path);

  const spring::compression_storage_plan huge_budget_disk =
      spring::build_compression_storage_plan(
          input_paths, 1024.0, spring::CompressionStorageRoute::disk);
  CHECK(huge_budget_disk.selected_path ==
        spring::compression_storage_path::disk_path);
}

TEST_CASE("Compression workspace dir honors --tmp-dir/-w override") {
  const std::filesystem::path archive = "outdir/sample.sp";
  const auto default_work = spring::resolve_compression_workspace_dir(
      archive, "", ".work-tmp");
  CHECK(default_work == std::filesystem::path("outdir/sample.sp.work-tmp"));

  const auto overridden = spring::resolve_compression_workspace_dir(
      archive, "/dev/shm", ".work-tmp");
  CHECK(overridden == std::filesystem::path("/dev/shm/sample.sp.work-tmp"));

  const auto grouped = spring::resolve_compression_workspace_dir(
      archive, "/tmp/spring-work", ".grouped-tmp");
  CHECK(grouped ==
        std::filesystem::path("/tmp/spring-work/sample.sp.grouped-tmp"));
}