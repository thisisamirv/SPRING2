#include "../support/doctest.h"

#include "assay_atac.h"
#include "params.h"

using spring::AtacAdapterStripSample;
using spring::compression_params;
using spring::has_atac_adapter;
using spring::kAtacAdapterStripMinSampleReads;
using spring::should_consider_atac_adapter_stripping;
using spring::update_atac_adapter_strip_sample;

TEST_CASE("ATAC strip sample waits for min eligible reads before deciding") {
  AtacAdapterStripSample sample;
  CHECK_FALSE(update_atac_adapter_strip_sample(sample, 100, 500,
                                               /*end_of_input=*/false, 1000));
  CHECK_FALSE(sample.decided);
  CHECK(sample.eligible_reads == 100);

  CHECK_FALSE(update_atac_adapter_strip_sample(sample, 100, 0,
                                               /*end_of_input=*/false, 1000));
  CHECK_FALSE(sample.decided);
  CHECK(sample.eligible_reads == 200);

  CHECK(update_atac_adapter_strip_sample(sample, 800, 800,
                                         /*end_of_input=*/false, 1000));
  CHECK(sample.decided);
  CHECK(sample.active);
  CHECK(sample.eligible_reads == 1000);
}

TEST_CASE("ATAC strip sample ignores all-N steps until real eligible reads") {
  AtacAdapterStripSample sample;
  CHECK_FALSE(update_atac_adapter_strip_sample(sample, 0, 0,
                                               /*end_of_input=*/false));
  CHECK_FALSE(sample.decided);

  CHECK(update_atac_adapter_strip_sample(
      sample, kAtacAdapterStripMinSampleReads,
      kAtacAdapterStripMinSampleReads * 2, /*end_of_input=*/false));
  CHECK(sample.decided);
  CHECK(sample.active);
}

TEST_CASE("ATAC strip sample can disable on sparse evidence at EOF") {
  AtacAdapterStripSample sample;
  // Sparse: 14 bases on 1/64 reads ≈ 0.22 avg over the min sample window.
  const uint32_t eligible = kAtacAdapterStripMinSampleReads;
  const uint64_t stripped = (eligible / 64) * 14;
  CHECK(update_atac_adapter_strip_sample(sample, eligible, stripped,
                                         /*end_of_input=*/true));
  CHECK(sample.decided);
  CHECK_FALSE(sample.active);
}

TEST_CASE("ATAC strip sample decides early on short/EOF step") {
  AtacAdapterStripSample sample;
  CHECK(update_atac_adapter_strip_sample(
      sample, 50, 50 * 12, /*end_of_input=*/true,
      kAtacAdapterStripMinSampleReads));
  CHECK(sample.decided);
  CHECK(sample.active);
}

TEST_CASE("Auto medium-confidence ATAC does not arm stripping") {
  compression_params cp{};
  cp.read_info.assay = "atac";
  cp.read_info.assay_confidence = "medium";
  CHECK_FALSE(should_consider_atac_adapter_stripping(cp, false, true));

  cp.read_info.assay_confidence = "high";
  CHECK(should_consider_atac_adapter_stripping(cp, false, true));

  cp.read_info.assay_confidence = "N/A";
  CHECK(should_consider_atac_adapter_stripping(cp, false, true));
}

TEST_CASE("ATAC adapter detection matches terminal strip, not internal kmers") {
  const std::string internal_only =
      "GCTAGCTAGCTCTGTCTCTTATAGCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTAG";
  CHECK_FALSE(has_atac_adapter(internal_only));

  const std::string terminal =
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACTGTCTCTTATACACATCT";
  CHECK(has_atac_adapter(terminal));

  const std::string reverse_terminal =
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGATGTGTATAAGAGACAG";
  CHECK(has_atac_adapter(reverse_terminal));
}
