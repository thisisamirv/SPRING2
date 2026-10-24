#include "../support/doctest.h"
#include "assay_detector.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace spring;

namespace {

TEST_CASE("Specialty assay ties prefer ATAC/RNA over bisulfite") {
  CHECK(std::string(pick_specialty_assay(80.0, 80.0, 40.0)) == "atac");
  CHECK(std::string(pick_specialty_assay(80.0, 40.0, 80.0)) == "rna");
  CHECK(std::string(pick_specialty_assay(80.0, 40.0, 40.0)) == "bisulfite");
  CHECK(std::string(pick_specialty_assay(90.0, 80.0, 80.0)) == "bisulfite");
  CHECK(pick_specialty_assay(10.0, 10.0, 10.0) == nullptr);
}

void create_sampling_fastq(const std::string &path, int total_reads,
                           int short_reads, int short_len, int long_len) {
  std::ofstream out(path, std::ios::binary);
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
  for (int i = 0; i < total_reads; ++i) {
    const int read_len = i < short_reads ? short_len : long_len;
    std::string read;
    read.reserve(static_cast<size_t>(read_len));
    for (int j = 0; j < read_len; ++j) {
      read.push_back(kBaseCycle[(i + j) % 4]);
    }
    out << "@sample_" << i << "\n";
    out << read << "\n";
    out << "+\n";
    out << std::string(static_cast<size_t>(read_len), 'I') << "\n";
  }
}

TEST_CASE("Testing AssayDetector") {
  AssayDetector detector;

  SUBCASE("Startup sampling stops at 10,000 fragments") {
    std::string test_fq = "test_startup_sample_limit.fq";
    create_sampling_fastq(test_fq, 12050, 10000, 80, 700);

    AssayDetector::StartupAnalysisResult res =
        detector.analyze_startup_sample(test_fq, "", "", "", "", false, false);
    CHECK(res.input_summary.sampled_fragments == 10000);
    CHECK(res.input_summary.max_read_length == 80);
    CHECK_FALSE(res.input_summary.requires_long_mode());

    std::filesystem::remove(test_fq);
  }

  SUBCASE("No reads parsed") {
    AssayDetector::DetectionResult res = detector.detect("", "", "", "", "");
    CHECK(res.assay == "dna");
    CHECK(res.confidence == "low (no reads parsed)");
  }

  SUBCASE("Test poly-A detection signature directly") {
    std::string test_fq = "test_sc_rna.fq";
    std::ofstream out(test_fq);
    for (int i = 0; i < 100; ++i) {
      out << "@read" << i << "\n";
      out << "GCTAGCTAGCTAGCTAGCTAGCTAGCTAGCAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "", "");
    CHECK(res.assay == "rna");
    CHECK(res.confidence.find("poly-A/T tails") != std::string::npos);

    std::filesystem::remove(test_fq);
  }

  SUBCASE("Test ATAC Tn5 detection signature directly") {
    std::string test_fq = "test_sc_atac.fq";
    std::string test_i1 = "test_sc_atac_I1.fq";
    std::ofstream out(test_fq);
    std::ofstream out_i1(test_i1);
    // Terminal mosaic-end overlap (matches strip); internal-only 12-mers must
    // not classify ATAC.
    for (int i = 0; i < 100; ++i) {
      out << "@read" << i << "\n";
      out << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACTGTCTCTTATACACATCT\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
      out_i1 << "@i1_" << i << "\n";
      out_i1 << "ACGTACGTACGTACGT\n";
      out_i1 << "+\n";
      out_i1 << "IIIIIIIIIIIIIIII\n";
    }
    out.close();
    out_i1.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", test_i1, "");
    CHECK(res.assay == "sc-atac");
    CHECK(res.confidence.find("Tn5 adapters") != std::string::npos);
    CHECK(res.confidence.find("explicit lanes") != std::string::npos);

    std::filesystem::remove(test_fq);
    std::filesystem::remove(test_i1);
  }

  SUBCASE("Missing index path does not force sc-atac") {
    std::string test_fq = "test_atac_missing_i1.fq";
    std::ofstream out(test_fq);
    for (int i = 0; i < 100; ++i) {
      out << "@read" << i << "\n";
      out << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACTGTCTCTTATACACATCT\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "definitely_missing_i1.fq", "");
    CHECK(res.assay == "atac");
    CHECK(res.confidence.find("explicit lanes") == std::string::npos);

    std::filesystem::remove(test_fq);
  }

  SUBCASE("Bulk DNA with real index lane stays dna not sc-dna") {
    std::string test_fq = "test_bulk_dna_with_i1.fq";
    std::string test_i1 = "test_bulk_dna_I1.fq";
    std::ofstream out(test_fq);
    std::ofstream out_i1(test_i1);
    for (int i = 0; i < 100; ++i) {
      out << "@read" << i << "\n";
      out << "ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
      out_i1 << "@i1_" << i << "\n";
      out_i1 << "ACGTACGTACGTACGT\n";
      out_i1 << "+\n";
      out_i1 << "IIIIIIIIIIIIIIII\n";
    }
    out.close();
    out_i1.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", test_i1, "");
    CHECK(res.assay == "dna");
    CHECK(res.assay != "sc-dna");
    CHECK(res.confidence.find("sc:") == std::string::npos);

    std::filesystem::remove(test_fq);
    std::filesystem::remove(test_i1);
  }

  SUBCASE("Test Bisulfite signature directly") {
    std::string test_fq = "test_bisulfite.fq";
    std::ofstream out(test_fq);
    for (int i = 0; i < 1001; ++i) {
      out << "@read" << i << "\n";
      out << "GTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTT\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "", "");
    CHECK(res.assay == "bisulfite");
    CHECK(res.confidence.find("-depletion") != std::string::npos);
    CHECK(res.depleted_base_by_stream[0] == 'C');
    CHECK(res.depleted_base_by_stream[1] == 'C');

    std::filesystem::remove(test_fq);
  }

  SUBCASE("Small clear bisulfite library is not forced to dna") {
    std::string test_fq = "test_bisulfite_small.fq";
    std::ofstream out(test_fq);
    for (int i = 0; i < 40; ++i) {
      out << "@read" << i << "\n";
      out << "GTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTT\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "", "");
    CHECK(res.assay == "bisulfite");
    CHECK(res.confidence.find("-depletion") != std::string::npos);

    std::filesystem::remove(test_fq);
  }

  SUBCASE("Infer sc-Bisulfite from paired layout heuristic") {
    std::string test_r1 = "test_sc_bisulfite_R1.fq";
    std::string test_r2 = "test_sc_bisulfite_R2.fq";
    std::ofstream out_r1(test_r1);
    std::ofstream out_r2(test_r2);

    for (int i = 0; i < 1001; ++i) {
      out_r1 << "@read" << i << "\n";
      out_r1 << "GTTTGTTTGTTTGTTTGTTTGTTTGTTTGT\n";
      out_r1 << "+\n";
      out_r1 << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";

      out_r2 << "@read" << i << "\n";
      out_r2 << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
      out_r2 << "+\n";
      out_r2 << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII"
                "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }

    out_r1.close();
    out_r2.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_r1, test_r2, "", "", "");
    CHECK(res.assay == "sc-bisulfite");
    CHECK(res.confidence.find("-depletion") != std::string::npos);

    std::filesystem::remove(test_r1);
    std::filesystem::remove(test_r2);
  }
}

} // namespace