#[=======================================================================[.rst:
PrecompiledHeaders
------------------

Applies the project-wide precompiled header (PCH) set shared by the
gtopt library and its test targets.

Usage::

  include(PrecompiledHeaders)
  target_gtopt_pch(<target>)

The header list covers the most frequently included standard library
headers and spdlog.  All headers are added as ``PRIVATE`` so they do
not leak to downstream consumers of the target.

#]=======================================================================]

function(target_gtopt_pch target)
  target_precompile_headers(${target} PRIVATE
    <algorithm>
    <chrono>
    <cmath>
    <cstdint>
    <expected>
    <filesystem>
    <format>
    <fstream>
    <functional>
    <map>
    <memory>
    <numeric>
    <optional>
    <ranges>
    <set>
    <span>
    <string>
    <string_view>
    <thread>
    <unordered_map>
    <utility>
    <variant>
    <vector>
    <spdlog/spdlog.h>
  )
endfunction()
