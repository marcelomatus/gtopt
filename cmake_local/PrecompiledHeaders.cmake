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
    # --- containers & data structures ---
    <array>
    <map>
    <set>
    <span>
    <string>
    <string_view>
    <tuple>
    <unordered_map>
    <variant>
    <vector>
    # --- algorithms & numerics ---
    <algorithm>
    <charconv>
    <cmath>
    <cstdint>
    <limits>
    <numeric>
    <ranges>
    # --- utilities ---
    <expected>
    <functional>
    <memory>
    <optional>
    <type_traits>
    <utility>
    # --- I/O & filesystem ---
    <filesystem>
    <format>
    <fstream>
    <sstream>
    # --- concurrency ---
    <atomic>
    <chrono>
    <future>
    <mutex>
    <thread>
    # --- error handling ---
    <stdexcept>
    # --- third-party ---
    <spdlog/spdlog.h>
  )
endfunction()
