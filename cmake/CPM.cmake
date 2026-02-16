# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2019-2023 Lars Melchior and contributors

set(CPM_DOWNLOAD_VERSION 0.42.1)
set(CPM_HASH_SUM "f3a6dcc6a04ce9e7f51a127307fa4f699fb2bade357a8eb4c5b45df76e1dc6a5")

if(CPM_SOURCE_CACHE)
  set(CPM_DOWNLOAD_LOCATION
      "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake"
  )
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_DOWNLOAD_LOCATION
      "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake"
  )
else()
  set(CPM_DOWNLOAD_LOCATION
      "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake"
  )
endif()

# Expand relative path. This is important if the provided path contains a tilde (~)
get_filename_component(CPM_DOWNLOAD_LOCATION ${CPM_DOWNLOAD_LOCATION} ABSOLUTE)

function(download_cpm)
  message(STATUS "Downloading CPM.cmake to ${CPM_DOWNLOAD_LOCATION}")
  file(
    DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
    ${CPM_DOWNLOAD_LOCATION}
    EXPECTED_HASH SHA256=${CPM_HASH_SUM}
  )
endfunction()

if(NOT (EXISTS ${CPM_DOWNLOAD_LOCATION}))
  download_cpm()
else()
  # If the file exists, verify its hash
  file(SHA256 ${CPM_DOWNLOAD_LOCATION} CPM_EXISTING_HASH)
  if(NOT CPM_EXISTING_HASH STREQUAL CPM_HASH_SUM)
    message(STATUS "CPM.cmake hash mismatch (cached: ${CPM_EXISTING_HASH}), re-downloading")
    download_cpm()
  endif()
endif()

include(${CPM_DOWNLOAD_LOCATION})
