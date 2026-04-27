/**
 * @file      linear_interface_labels.cpp
 * @brief     LinearInterface label synthesis, compression, and write_lp.
 * @date      2026-04-26
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Extracted from `linear_interface.cpp` (the "Names & LP file output"
 * section).  Contains the on-demand label generator
 * (`generate_labels_from_maps`), its caching driver
 * (`materialize_labels`), the low-memory label-meta compress/decompress
 * pair, the duplicate-detection index rebuild, and the `write_lp`
 * entry point.
 */

#include <expected>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtopt/error.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/linear_interface_labels_codec.hpp>
#include <gtopt/memory_compress.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

void LinearInterface::generate_labels_from_maps(
    std::vector<std::string>& col_names,
    std::vector<std::string>& row_names) const
{
  // Lazy-lazy decompression: if `release_backend` stashed the
  // metadata into compressed buffers, rehydrate it now on first
  // read.  No-op in modes that never compressed.
  ensure_labels_meta_decompressed();

  // Forced-all LabelMaker: `m_label_maker_` is whatever flatten
  // captured (typically `none`).  Here we want real labels always,
  // synthesised on demand from metadata.
  const LabelMaker writer_labels {LpNamesLevel::all};

  const auto ncols = static_cast<size_t>(m_backend_->get_num_cols());
  col_names.assign(ncols, std::string {});

  // Keep the formatted-name cache sized with the live LP so
  // subsequent `generate_labels_from_maps` calls (after `add_col`
  // added new entries) hit the cache for already-formatted cols and
  // only format the tail.
  if (m_col_index_to_name_.size() < ncols) {
    m_col_index_to_name_.resize(ncols);
  }

  for (size_t i = 0; i < ncols; ++i) {
    const ColIndex ci {i};

    // Cache hit: label was populated by a legacy `add_col(string, …)`
    // overload (user-supplied name) or by a prior call to this method.
    if (!m_col_index_to_name_[ci].empty()) {
      col_names[i] = m_col_index_to_name_[ci];
      continue;
    }

    // Cache miss: synthesise from metadata and cache the result so
    // subsequent calls (repeat `write_lp`) avoid re-formatting.
    if (i >= m_col_labels_meta_.size()) {
      throw std::logic_error(std::format(
          "LinearInterface::generate_labels_from_maps: col {} has no "
          "entry in m_col_labels_meta_ (size {}) and no pre-formatted "
          "name — col was added without metadata tracking.",
          i,
          m_col_labels_meta_.size()));
    }
    const auto& meta = m_col_labels_meta_[i];
    SparseCol view {};
    view.class_name = meta.class_name;
    view.variable_name = meta.variable_name;
    view.variable_uid = meta.variable_uid;
    view.context = meta.context;
    auto label = writer_labels.make_col_label(view);
    if (label.empty()) {
      throw std::logic_error(
          std::format("LinearInterface::generate_labels_from_maps: col {} has "
                      "metadata without a class_name (unlabelable).",
                      i));
    }
    m_col_index_to_name_[ci] = label;  // cache in index→name
    if (auto [it, inserted] = m_col_names_.try_emplace(label, ci); !inserted) {
      throw std::runtime_error(std::format(
          "LinearInterface: duplicate col metadata — label '{}' synthesised "
          "by col {} was already emitted by col {}.  Two cols share "
          "(class_name, variable_name, uid, context).",
          label,
          i,
          it->second));
    }
    col_names[i] = std::move(label);
  }

  const auto nrows = static_cast<size_t>(m_backend_->get_num_rows());
  row_names.assign(nrows, std::string {});
  if (m_row_index_to_name_.size() < nrows) {
    m_row_index_to_name_.resize(nrows);
  }

  for (size_t i = 0; i < nrows; ++i) {
    const RowIndex ri {i};
    if (!m_row_index_to_name_[ri].empty()) {
      row_names[i] = m_row_index_to_name_[ri];
      continue;
    }
    if (i >= m_row_labels_meta_.size()) {
      throw std::logic_error(std::format(
          "LinearInterface::generate_labels_from_maps: row {} has no "
          "entry in m_row_labels_meta_ (size {}) and no pre-formatted "
          "name — row was added without metadata tracking.",
          i,
          m_row_labels_meta_.size()));
    }
    const auto& meta = m_row_labels_meta_[i];
    SparseRow view {};
    view.class_name = meta.class_name;
    view.constraint_name = meta.constraint_name;
    view.variable_uid = meta.variable_uid;
    view.context = meta.context;
    auto label = writer_labels.make_row_label(view);
    if (label.empty()) {
      throw std::logic_error(
          std::format("LinearInterface::generate_labels_from_maps: row {} has "
                      "metadata without a class_name (unlabelable).",
                      i));
    }
    m_row_index_to_name_[ri] = label;  // cache in index→name
    if (auto [it, inserted] = m_row_names_.try_emplace(label, ri); !inserted) {
      throw std::runtime_error(std::format(
          "LinearInterface: duplicate row metadata — label '{}' synthesised "
          "by row {} was already emitted by row {}.  Two rows share "
          "(class_name, constraint_name, uid, context).",
          label,
          i,
          it->second));
    }
    row_names[i] = std::move(label);
  }
}

void LinearInterface::materialize_labels() const
{
  // Trigger `generate_labels_from_maps` so caches
  // (`m_col_index_to_name_` / `m_row_index_to_name_`) and name→index
  // maps (`m_col_names_` / `m_row_names_`) are populated.  Discards
  // the returned vectors — callers that need them directly should
  // call `generate_labels_from_maps` themselves.
  std::vector<std::string> col_names;
  std::vector<std::string> row_names;
  generate_labels_from_maps(col_names, row_names);
}

void LinearInterface::compress_labels_meta_if_needed()
{
  if (m_low_memory_mode_ == LowMemoryMode::off) {
    return;
  }
  // Always re-compress whenever live data is present.  A stale
  // compressed buffer may already exist from a previous release
  // cycle; the recompressed buffer supersedes it so growth between
  // release cycles (cuts appended on iter N, α re-appended on
  // reload, etc.) is captured.  When live is empty (already
  // compressed and no post-reload mutations) this is a no-op.
  if (!m_col_labels_meta_.empty()) {
    m_col_labels_meta_count_ = m_col_labels_meta_.size();
    const auto bytes = serialize_labels_meta(m_col_labels_meta_);
    const auto codec = select_codec(m_memory_codec_);
    m_col_labels_meta_compressed_ =
        compress_buffer({bytes.data(), bytes.size()}, codec);
    // Drop the live vector; `string_view`s in `m_col_labels_meta_`
    // are now invalidated.  The string pool stays alive until the
    // next decompression cycle reseeds it with fresh strings.
    m_col_labels_meta_.clear();
    m_col_labels_meta_.shrink_to_fit();
  }
  if (!m_row_labels_meta_.empty()) {
    m_row_labels_meta_count_ = m_row_labels_meta_.size();
    const auto bytes = serialize_labels_meta(m_row_labels_meta_);
    const auto codec = select_codec(m_memory_codec_);
    m_row_labels_meta_compressed_ =
        compress_buffer({bytes.data(), bytes.size()}, codec);
    m_row_labels_meta_.clear();
    m_row_labels_meta_.shrink_to_fit();
  }

  // Duplicate-detection maps hold string_views into the live vectors
  // we just emptied; they'd dangle if left populated.  Rebuilt in
  // `ensure_labels_meta_decompressed` on the next read.
  m_col_meta_index_.clear();
  m_row_meta_index_.clear();
}

void LinearInterface::ensure_labels_meta_decompressed() const
{
  bool decompressed_any = false;
  // Decompress on first read / write after `compress_labels_meta_if_
  // needed` fired.  Idempotent: non-empty live vectors mean we've
  // already decompressed (or never compressed in the first place).
  if (!m_col_labels_meta_compressed_.empty() && m_col_labels_meta_.empty()) {
    const auto bytes = m_col_labels_meta_compressed_.decompress_data();
    // Reserve the pool to hold 2 string_views per entry (class_name
    // + variable_name) so `push_back` never reallocates and the
    // revived `string_view`s stay valid.
    m_label_string_pool_.clear();
    m_label_string_pool_.reserve((m_col_labels_meta_count_ * 2)
                                 + (m_row_labels_meta_count_ * 2));
    m_col_labels_meta_ =
        deserialize_col_labels_meta({bytes.data(), bytes.size()},
                                    m_col_labels_meta_count_,
                                    m_label_string_pool_);
    m_col_labels_meta_compressed_ = {};
    m_col_labels_meta_count_ = 0;
    decompressed_any = true;
  }
  if (!m_row_labels_meta_compressed_.empty() && m_row_labels_meta_.empty()) {
    const auto bytes = m_row_labels_meta_compressed_.decompress_data();
    // Keep existing col-pool entries — if col was decompressed first
    // the string_views still point into their original pool slots.
    // The row pass just appends.
    if (m_label_string_pool_.capacity() == 0) {
      m_label_string_pool_.reserve(m_row_labels_meta_count_ * 2);
    }
    m_row_labels_meta_ =
        deserialize_row_labels_meta({bytes.data(), bytes.size()},
                                    m_row_labels_meta_count_,
                                    m_label_string_pool_);
    m_row_labels_meta_compressed_ = {};
    m_row_labels_meta_count_ = 0;
    decompressed_any = true;
  }
  // Rehydrate the duplicate-detection maps so add_col / add_row
  // after a reload keep enforcing uniqueness against the whole
  // history, not just post-reload additions.
  if (decompressed_any) {
    rebuild_meta_indexes();
  }
}

void LinearInterface::rebuild_meta_indexes() const
{
  m_col_meta_index_.clear();
  m_col_meta_index_.reserve(m_col_labels_meta_.size());
  for (const auto [i, label] : enumerate<ColIndex>(m_col_labels_meta_)) {
    if (!is_empty_col_label(label)) {
      m_col_meta_index_.emplace(label, i);
    }
  }
  m_row_meta_index_.clear();
  m_row_meta_index_.reserve(m_row_labels_meta_.size());
  for (const auto [i, label] : enumerate<RowIndex>(m_row_labels_meta_)) {
    if (!is_empty_row_label(label)) {
      m_row_meta_index_.emplace(label, i);
    }
  }
}

void LinearInterface::push_names_to_solver() const
{
  std::vector<std::string> col_names;
  std::vector<std::string> row_names;
  generate_labels_from_maps(col_names, row_names);
  m_backend_->push_names(col_names, row_names);
}

auto LinearInterface::write_lp(const std::string& filename) const
    -> std::expected<void, Error>
{
  if (filename.empty()) {
    return {};
  }

  // Names may be missing entirely (flatten ran without
  // `LpMatrixOptions::{col,row}_with_names`) or populated only for a
  // prefix of cols/rows.  `push_names_to_solver` fills the gaps with
  // generic `c<index>` / `r<index>` labels so the backend always
  // receives a fully-named LP.  Real gtopt names (e.g.
  // `Bus.theta.s0.p0.b0`) still require names enabled at flatten
  // time — run with `--lp-debug` or the equivalent
  // `LpMatrixOptions{col_with_names, row_with_names} = true` for
  // those.  The generic fallback guarantees `write_lp` never fails
  // on a well-formed backend, which is a hard requirement for the
  // SDDP error-LP dump path.
  push_names_to_solver();
  m_backend_->write_lp(filename.c_str());
  return {};
}

}  // namespace gtopt
