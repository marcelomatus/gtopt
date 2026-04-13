/**
 * @file      hardware_info.cpp
 * @brief     Physical core detection via Linux sysfs topology
 */

#include <gtopt/hardware_info.hpp>

#ifdef __linux__
#  include <charconv>
#  include <filesystem>
#  include <fstream>
#  include <set>
#  include <string>
#  include <utility>

#  include <spdlog/spdlog.h>
#endif

namespace gtopt
{

[[nodiscard]] unsigned physical_concurrency() noexcept
{
#ifdef __linux__
  try {
    namespace fs = std::filesystem;

    // Count unique (package_id, core_id) pairs across all online CPUs.
    std::set<std::pair<int, int>> physical_cores;

    constexpr std::string_view cpu_base = "/sys/devices/system/cpu";

    for (const auto& entry : fs::directory_iterator(cpu_base)) {
      const auto name = entry.path().filename().string();

      // Match cpu0, cpu1, ... (skip cpufreq, cpuidle, etc.)
      if (name.size() < 4 || name[0] != 'c' || name[1] != 'p' || name[2] != 'u'
          || name[3] < '0' || name[3] > '9')
      {
        continue;
      }

      const auto topo_dir = entry.path() / "topology";
      const auto pkg_path = topo_dir / "physical_package_id";
      const auto core_path = topo_dir / "core_id";

      if (!fs::exists(pkg_path) || !fs::exists(core_path)) {
        continue;
      }

      auto read_int = [](const fs::path& path) -> int
      {
        std::ifstream ifs(path);
        std::string buf;
        if (!std::getline(ifs, buf) || buf.empty()) {
          return -1;
        }
        int val = -1;
        std::from_chars(buf.data(), buf.data() + buf.size(), val);
        return val;
      };

      const int pkg_id = read_int(pkg_path);
      const int core_id = read_int(core_path);

      if (pkg_id >= 0 && core_id >= 0) {
        physical_cores.emplace(pkg_id, core_id);
      }
    }

    if (!physical_cores.empty()) {
      const auto count = static_cast<unsigned>(physical_cores.size());
      SPDLOG_DEBUG(
          "Detected {} physical cores, {} logical cores (SMT ratio {})",
          count,
          std::thread::hardware_concurrency(),
          std::thread::hardware_concurrency() / count);
      return count;
    }
  } catch (...) {
    // Fall through to default
  }
#endif

  return std::thread::hardware_concurrency();
}

}  // namespace gtopt
