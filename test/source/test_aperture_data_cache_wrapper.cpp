// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file      test_aperture_data_cache_wrapper.cpp
 * @brief     Compilation unit for aperture_data_cache.hpp
 * @date      2026-04-05
 *
 * aperture_data_cache.hpp is kept as a header because it defines
 * helper utilities (TmpDir, write_test_parquet) used by
 * test_aperture_cache_advanced.cpp.  This wrapper pulls it into the build.
 */

#include <filesystem>
#include <fstream>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <doctest/doctest.h>
#include <gtopt/aperture_data_cache.hpp>
#include <parquet/arrow/writer.h>

using namespace gtopt;  // NOLINT(google-global-names-in-headers)

#include "aperture_data_cache.hpp"
