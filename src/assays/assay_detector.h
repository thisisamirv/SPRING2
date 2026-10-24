// Declares assay detection interfaces used for startup sampling and
// automatic assay classification.

#ifndef SPRING_ASSAY_DETECTOR_H_
#define SPRING_ASSAY_DETECTOR_H_

#include "input_preparation.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct AssayDetectionStats {
  // Fragments sampled (PE pair = 1).
  uint64_t total_reads = 0;
  uint64_t r1_C = 0, r1_T = 0, r1_G = 0, r1_A = 0;
  uint64_t r2_C = 0, r2_T = 0, r2_G = 0, r2_A = 0;
  std::vector<int> r1_lengths;
  std::vector<int> r2_lengths;
  std::vector<int> r3_lengths; // chemistry-only sample (not used for PE layout)
  // Per-lane signal counts (not fragments). Score with max_lane_signal_rate().
  uint64_t atac_adapters_r1 = 0;
  uint64_t atac_adapters_r2 = 0;
  uint64_t atac_adapters_r3 = 0;
  uint64_t poly_a_tails_r1 = 0;
  uint64_t poly_a_tails_r2 = 0;
  uint64_t poly_a_tails_r3 = 0;
  uint64_t rna_hits = 0;
  uint64_t atac_hits = 0;
  uint64_t intron_hits = 0;
  uint64_t total_sampled_kmers = 0;
  uint64_t headers_with_cb_tag = 0;
  uint64_t headers_with_umi_tag = 0;
};

// Max of per-lane hit rates so PE cannot exceed 1.0 (unlike hits/fragments),
// a signal on only one mate is preserved, and multiome R3 chemistry counts.
[[nodiscard]] inline double max_lane_signal_rate(uint64_t r1_hits,
                                                 uint64_t r2_hits,
                                                 uint64_t r3_hits,
                                                 const AssayDetectionStats
                                                     &stats) {
  auto rate = [](uint64_t hits, const std::vector<int> &lengths) {
    return lengths.empty()
               ? 0.0
               : static_cast<double>(hits) /
                     static_cast<double>(lengths.size());
  };
  const double r1_rate = rate(r1_hits, stats.r1_lengths);
  const double r2_rate = rate(r2_hits, stats.r2_lengths);
  const double r3_rate = rate(r3_hits, stats.r3_lengths);
  return std::max({r1_rate, r2_rate, r3_rate});
}

struct AssayScoreBreakdown {
  double score = 0.0;
  std::vector<std::string> evidence;
};

struct BisulfiteDetectionScore {
  AssayScoreBreakdown breakdown;
  double c_ratio = 1.0;
  double g_ratio = 1.0;
  char depleted_base_by_stream[2] = {'N', 'N'};
};

struct SingleCellDetectionEvidence {
  bool is_single_cell = false;
  int indicator_count = 0;
  std::vector<std::string> evidence;
};

// Shared single-cell layout heuristic (explicit lanes, CB/UMI header tags,
// R1/R2 read-length asymmetry) used by AssayDetector during startup sampling.
[[nodiscard]] SingleCellDetectionEvidence
detect_single_cell_layout(const AssayDetectionStats &stats,
                          bool explicit_sc_layout);

// Specialty assay with the highest score (>= min_score). On exact ties, prefer
// sequence evidence (atac, then rna) over composition (bisulfite). Returns
// nullptr when no specialty score clears min_score (caller treats as dna).
[[nodiscard]] const char *
pick_specialty_assay(double bisulfite_score, double atac_score,
                     double rna_score, double min_score = 20.0);

class AssayDetector {
public:
  AssayDetector() = default;

  struct DetectionResult {
    std::string assay;
    std::string confidence;
    double c_ratio = 1.0;
    double g_ratio = 1.0;
    char depleted_base_by_stream[2] = {'N', 'N'};
  };

  struct StartupAnalysisResult {
    input_detection_summary input_summary;
    DetectionResult assay_result;
  };

  // Runs the 5-stage heuristic detection on the provided FASTQ or FASTA
  // inputs (format is sniffed from R1). Returns the predicted assay type.
  DetectionResult detect(const std::string &r1_path, const std::string &r2_path,
                         const std::string &r3_path, const std::string &i1_path,
                         const std::string &i2_path);

  StartupAnalysisResult
  analyze_startup_sample(const std::string &r1_path, const std::string &r2_path,
                         const std::string &r3_path, const std::string &i1_path,
                         const std::string &i2_path, bool paired_end,
                         bool fasta_input);

private:
  enum class BlockID : uint8_t {
    NONE = 0,
    RNA_EXON = 1,
    ATAC_PROMOTER = 2,
    INTRON_CTRL = 3,
    GENOME_BACKBONE = 4,
    // Kmer seen under more than one specialty label — ignored when scoring.
    AMBIGUOUS = 5
  };

  void load_reference();
  DetectionResult evaluate_stages(const AssayDetectionStats &stats,
                                  bool explicit_sc_layout);

  std::unordered_map<uint64_t, BlockID> kmer_index_;
  bool reference_loaded_ = false;
  bool reference_load_attempted_ = false;
  static constexpr int kKmerSize = 17;
  static constexpr int kMaxReadsToScan = 10000;
};

} // namespace spring

#endif // SPRING_ASSAY_DETECTOR_H_
