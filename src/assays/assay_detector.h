// Declares assay detection interfaces used for startup sampling and
// automatic assay classification.

#ifndef SPRING_ASSAY_DETECTOR_H_
#define SPRING_ASSAY_DETECTOR_H_

#include "input_preparation.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct AssayDetectionStats {
  uint64_t total_reads = 0;
  uint64_t r1_C = 0, r1_T = 0, r1_G = 0, r1_A = 0;
  uint64_t r2_C = 0, r2_T = 0, r2_G = 0, r2_A = 0;
  std::vector<int> r1_lengths;
  std::vector<int> r2_lengths;
  uint64_t atac_adapters_found = 0;
  uint64_t poly_a_tails_found = 0;
  uint64_t rna_hits = 0;
  uint64_t atac_hits = 0;
  uint64_t intron_hits = 0;
  uint64_t total_sampled_kmers = 0;
  uint64_t headers_with_cb_tag = 0;
  uint64_t headers_with_umi_tag = 0;
};

struct AssayScoreBreakdown {
  double score = 0.0;
  std::vector<std::string> evidence;
};

struct BisulfiteDetectionScore {
  AssayScoreBreakdown breakdown;
  double c_ratio = 1.0;
  double g_ratio = 1.0;
  char depleted_base = 'N';
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

class AssayDetector {
public:
  AssayDetector() = default;

  struct DetectionResult {
    std::string assay;
    std::string confidence;
    double c_ratio = 1.0;
    double g_ratio = 1.0;
    char depleted_base = 'N';
  };

  struct StartupAnalysisResult {
    input_detection_summary input_summary;
    DetectionResult assay_result;
  };

  // Runs the 5-stage heuristic detection on the provided FASTQ files.
  // Returns the final predicted assay type and stats.
  DetectionResult detect(const std::string &r1_path, const std::string &r2_path,
                         const std::string &r3_path, const std::string &i1_path,
                         const std::string &i2_path);

  StartupAnalysisResult
  analyze_startup_sample(const std::string &r1_path, const std::string &r2_path,
                         const std::string &r3_path, const std::string &i1_path,
                         const std::string &i2_path, bool paired_end,
                         bool fasta_input);

private:
  enum class BlockID {
    NONE = 0,
    RNA_EXON = 1,
    ATAC_PROMOTER = 2,
    INTRON_CTRL = 3,
    GENOME_BACKBONE = 4
  };

  void load_reference();
  DetectionResult evaluate_stages(const AssayDetectionStats &stats,
                                  bool explicit_sc_layout);

  std::unordered_map<uint64_t, BlockID> kmer_index_;
  bool reference_loaded_ = false;
  static constexpr int kKmerSize = 17;
  static constexpr int kMaxReadsToScan = 10000;
};

} // namespace spring

#endif // SPRING_ASSAY_DETECTOR_H_
