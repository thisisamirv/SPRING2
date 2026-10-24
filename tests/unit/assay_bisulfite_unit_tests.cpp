#include "../support/doctest.h"

#include "assay_bisulfite.h"
#include "assay_detector.h"
#include "params.h"

#include <sstream>

using spring::AssayDetectionStats;
using spring::AssayDetector;
using spring::apply_bisulfite_auto_config;
using spring::compression_params;
using spring::score_bisulfite_assay;

TEST_CASE("Directional PE bisulfite keeps per-stream depleted bases") {
  AssayDetectionStats stats;
  stats.total_reads = 2000;
  // R1: strong C-depletion (C rare vs T)
  stats.r1_C = 20;
  stats.r1_T = 800;
  stats.r1_G = 400;
  stats.r1_A = 400;
  // R2: strong G-depletion (G rare vs A)
  stats.r2_C = 400;
  stats.r2_T = 400;
  stats.r2_G = 20;
  stats.r2_A = 800;

  const auto score = score_bisulfite_assay(stats);
  CHECK(score.depleted_base_by_stream[0] == 'C');
  CHECK(score.depleted_base_by_stream[1] == 'G');
  CHECK(score.breakdown.score >= 160.0);

  AssayDetector::DetectionResult detection;
  detection.assay = "bisulfite";
  detection.c_ratio = score.c_ratio;
  detection.g_ratio = score.g_ratio;
  detection.depleted_base_by_stream[0] = score.depleted_base_by_stream[0];
  detection.depleted_base_by_stream[1] = score.depleted_base_by_stream[1];

  compression_params cp{};
  apply_bisulfite_auto_config(cp, detection);
  CHECK(cp.encoding.bisulfite_ternary);
  CHECK(cp.encoding.depleted_base_by_stream[0] == 'C');
  CHECK(cp.encoding.depleted_base_by_stream[1] == 'G');
}

TEST_CASE("SE bisulfite mirrors depleted base into stream 1") {
  AssayDetectionStats stats;
  stats.total_reads = 1000;
  stats.r1_C = 10;
  stats.r1_T = 500;
  stats.r1_G = 250;
  stats.r1_A = 250;

  const auto score = score_bisulfite_assay(stats);
  CHECK(score.depleted_base_by_stream[0] == 'C');
  CHECK(score.depleted_base_by_stream[1] == 'C');
}

TEST_CASE("Small bisulfite libraries still score without a 500-fragment floor") {
  AssayDetectionStats stats;
  stats.total_reads = 40; // previously ignored entirely
  stats.r1_C = 5;
  stats.r1_T = 200;
  stats.r1_G = 100;
  stats.r1_A = 100;

  const auto score = score_bisulfite_assay(stats);
  CHECK(score.breakdown.score >= 80.0);
  CHECK(score.depleted_base_by_stream[0] == 'C');
  CHECK(score.depleted_base_by_stream[1] == 'C');
}

TEST_CASE("Same-stream strong C and G picks the more depleted base") {
  AssayDetectionStats stats;
  stats.total_reads = 1000;
  // Both C and G look "strong", but G is more depleted (ratio 0.02 vs 0.08).
  stats.r1_C = 40;
  stats.r1_T = 460; // c_ratio ≈ 40/501 ≈ 0.08
  stats.r1_G = 10;
  stats.r1_A = 490; // g_ratio ≈ 10/501 ≈ 0.02

  const auto score = score_bisulfite_assay(stats);
  CHECK(score.depleted_base_by_stream[0] == 'G');
  CHECK(score.breakdown.evidence[0].find("G-depletion") != std::string::npos);
}

TEST_CASE("Archive metadata round-trips mate depleted base") {
  compression_params cp{};
  cp.encoding.bisulfite_ternary = true;
  cp.encoding.depleted_base_by_stream[0] = 'C';
  cp.encoding.depleted_base_by_stream[1] = 'G';
  cp.read_info.digest_length_framed = true;

  std::ostringstream out(std::ios::binary);
  spring::write_compression_params(out, cp);

  compression_params parsed{};
  std::istringstream in(out.str(), std::ios::binary);
  spring::read_compression_params(in, parsed);
  CHECK(parsed.encoding.bisulfite_ternary);
  CHECK(parsed.encoding.depleted_base_by_stream[0] == 'C');
  CHECK(parsed.encoding.depleted_base_by_stream[1] == 'G');
}
