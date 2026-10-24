#include "../support/doctest.h"

#include "assay_rna.h"
#include "assay_sc_rna.h"
#include "params.h"

#include <stdexcept>

using spring::append_grouped_sc_rna_index_suffix_to_id;
using spring::compression_params;
using spring::detect_and_strip_rna_tail;
using spring::has_poly_a_tail;
using spring::should_enable_rna_tail_stripping;
using spring::should_enable_sc_rna_cb_prefix_stripping;
using spring::strip_grouped_sc_rna_index_suffix_from_id;

namespace {

compression_params make_sc_rna_cp(const std::string &confidence,
                                  bool cb_len_was_explicit,
                                  bool preserve_order = true) {
  compression_params cp{};
  cp.encoding.preserve_order = preserve_order;
  cp.encoding.long_flag = false;
  cp.encoding.cb_len = 16;
  cp.encoding.cb_len_was_explicit = cb_len_was_explicit;
  cp.encoding.cb_prefix_source_external = false;
  cp.read_info.assay = "sc-rna";
  cp.read_info.assay_confidence = confidence;
  return cp;
}

} // namespace

TEST_CASE("Explicit sc-rna enables CB strip with default cb_len") {
  CHECK(should_enable_sc_rna_cb_prefix_stripping(
      make_sc_rna_cp("N/A", /*cb_len_was_explicit=*/false)));
}

TEST_CASE("Auto high-confidence sc-rna requires explicit cb_len to strip") {
  CHECK_FALSE(should_enable_sc_rna_cb_prefix_stripping(
      make_sc_rna_cp("high (poly-A)", /*cb_len_was_explicit=*/false)));
  CHECK(should_enable_sc_rna_cb_prefix_stripping(
      make_sc_rna_cp("high (poly-A)", /*cb_len_was_explicit=*/true)));
}

TEST_CASE("Auto medium-confidence sc-rna never strips CB prefix") {
  CHECK_FALSE(should_enable_sc_rna_cb_prefix_stripping(
      make_sc_rna_cp("medium (weak)", /*cb_len_was_explicit=*/true)));
  CHECK_FALSE(should_enable_sc_rna_cb_prefix_stripping(
      make_sc_rna_cp("medium (weak)", /*cb_len_was_explicit=*/false)));
}

TEST_CASE("External CB source disables R1 CB strip") {
  compression_params cp =
      make_sc_rna_cp("N/A", /*cb_len_was_explicit=*/true);
  cp.encoding.cb_prefix_source_external = true;
  CHECK_FALSE(should_enable_sc_rna_cb_prefix_stripping(cp));
}

TEST_CASE("Index-group archives do not strip R1 CB prefix") {
  compression_params cp =
      make_sc_rna_cp("N/A", /*cb_len_was_explicit=*/true);
  cp.read_info.note = "index-group";
  CHECK_FALSE(should_enable_sc_rna_cb_prefix_stripping(cp));
}

TEST_CASE("Read3-group archives do not strip R1 CB prefix") {
  compression_params cp =
      make_sc_rna_cp("N/A", /*cb_len_was_explicit=*/true);
  cp.read_info.note = "read3-group";
  CHECK_FALSE(should_enable_sc_rna_cb_prefix_stripping(cp));
}

TEST_CASE("Grouped secondary notes disable poly-A strip despite inherited assay") {
  compression_params cp{};
  cp.encoding.long_flag = false;
  cp.encoding.preserve_order = false;
  cp.read_info.assay = "sc-rna";
  cp.read_info.assay_confidence = "N/A";
  CHECK(should_enable_rna_tail_stripping(cp));

  cp.read_info.note = "index-group";
  CHECK_FALSE(should_enable_rna_tail_stripping(cp));
  cp.read_info.note = "read3-group";
  CHECK_FALSE(should_enable_rna_tail_stripping(cp));
}

TEST_CASE("Poly-A detection matches contiguous strip, not composition window") {
  // 15 A's in the last 20 bases but broken by other bases — old detector
  // fired; strip requires a contiguous run > 20.
  const std::string composition_only =
      "GCTAGCTAGCTAGCTAGCTAGCTAGCTAGCAAACAAAACAAAACAAAACAAAACAAAA";
  CHECK_FALSE(has_poly_a_tail(composition_only));

  const std::string contiguous_a =
      "GCTAGCTAGCTAGCTAGCTAGCTAGCTAGCAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  CHECK(has_poly_a_tail(contiguous_a));
  std::string stripped = contiguous_a;
  uint32_t len = static_cast<uint32_t>(stripped.size());
  CHECK(detect_and_strip_rna_tail(stripped, len) != 0);
  CHECK(len < contiguous_a.size());

  const std::string short_run(20, 'A'); // run == MIN_LEN is not stripped
  CHECK_FALSE(has_poly_a_tail(short_run));
}

TEST_CASE("Grouped index-ID strip matches paired I1+I2 suffix") {
  std::string id = "@r 1:N:0:ACGT+TGCA";
  const std::string i1 = "ACGT";
  const std::string i2 = "TGCA";
  CHECK(strip_grouped_sc_rna_index_suffix_from_id(id, i1, &i2));
  CHECK(id == "@r 1:N:0:");
  append_grouped_sc_rna_index_suffix_to_id(id, i1, &i2);
  CHECK(id == "@r 1:N:0:ACGT+TGCA");
}

TEST_CASE("Grouped index-ID strip rejects I2 mismatch") {
  std::string id = "@r 1:N:0:ACGT+TGCA";
  const std::string i1 = "ACGT";
  const std::string i2 = "AAAA";
  CHECK_FALSE(strip_grouped_sc_rna_index_suffix_from_id(id, i1, &i2));
  CHECK(id == "@r 1:N:0:ACGT+TGCA");
}

TEST_CASE("Grouped index-ID restore is no-op for unstripped IDs") {
  std::string id = "@r 1:N:0:ACGT+TGCA";
  const std::string i1 = "ACGT";
  const std::string i2 = "TGCA";
  append_grouped_sc_rna_index_suffix_to_id(id, i1, &i2);
  CHECK(id == "@r 1:N:0:ACGT+TGCA");
}

TEST_CASE("Grouped index-ID restore throws on empty ID") {
  std::string id;
  const std::string i1 = "ACGT";
  CHECK_THROWS_AS(append_grouped_sc_rna_index_suffix_to_id(id, i1, nullptr),
                  std::runtime_error);
}
