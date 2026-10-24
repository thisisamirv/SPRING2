#include "../support/doctest.h"

#include "assay_atac.h"
#include "assay_detector.h"
#include "assay_rna.h"
#include "params.h"

using spring::AssayDetectionStats;
using spring::AtacAdapterStripSample;
using spring::compression_params;
using spring::has_atac_adapter;
using spring::kAtacAdapterStripMinSampleReads;
using spring::score_atac_assay;
using spring::score_rna_assay;
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

  const std::string lowercase_terminal =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaactgtctcttatacacatct";
  CHECK(has_atac_adapter(lowercase_terminal));
}

TEST_CASE("PE ATAC adapter rate uses max per-mate rate not hits/fragments") {
  // Both mates at 5%: old hits/fragments = 10/100 = 10% (>5% → 90). Max mate
  // rate stays 5% (>1% → 70), matching SE at the same per-read rate.
  AssayDetectionStats pe;
  pe.total_reads = 100;
  pe.r1_lengths.assign(100, 50);
  pe.r2_lengths.assign(100, 50);
  pe.atac_adapters_r1 = 5;
  pe.atac_adapters_r2 = 5;
  const auto pe_score = score_atac_assay(pe, /*reference_loaded=*/false);
  CHECK(pe_score.score == 70.0);

  AssayDetectionStats se;
  se.total_reads = 100;
  se.r1_lengths.assign(100, 50);
  se.atac_adapters_r1 = 5;
  const auto se_score = score_atac_assay(se, /*reference_loaded=*/false);
  CHECK(se_score.score == pe_score.score);

  // R3-only adapters at 5% match SE/PE rate (multiome chemistry lane).
  AssayDetectionStats r3_only;
  r3_only.total_reads = 100;
  r3_only.r1_lengths.assign(100, 50);
  r3_only.r3_lengths.assign(100, 50);
  r3_only.atac_adapters_r3 = 5;
  CHECK(score_atac_assay(r3_only, false).score == pe_score.score);
}

TEST_CASE("PE poly-A on one mate is not diluted by the other") {
  AssayDetectionStats pe;
  pe.total_reads = 100;
  pe.r1_lengths.assign(100, 28); // barcode mate: no poly-A
  pe.r2_lengths.assign(100, 90);
  pe.poly_a_tails_r2 = 12; // 12% of R2 — should stay >5% high band
  const auto pe_score =
      score_rna_assay(pe, /*reference_loaded=*/false,
                      /*suppress_poly_a_signal=*/false);
  CHECK(pe_score.score == 80.0);

  // Both mates at 3%: old hits/fragments = 6%. Max rate = 3% → medium band.
  AssayDetectionStats both;
  both.total_reads = 100;
  both.r1_lengths.assign(100, 50);
  both.r2_lengths.assign(100, 50);
  both.poly_a_tails_r1 = 3;
  both.poly_a_tails_r2 = 3;
  const auto both_score = score_rna_assay(both, false, false);
  CHECK(both_score.score == 60.0);
}
