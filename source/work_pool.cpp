// Standard library
#include <algorithm>
#include <utility>
#include <vector>

// Third-party
#include <spdlog/spdlog.h>

// Project headers
#include <gtopt/cpu_monitor.hpp>
#include <gtopt/work_pool.hpp>

// NOTE: All BasicWorkPool<>, AdaptiveWorkPool, and SDDPWorkPool methods are
// defined inline in work_pool.hpp (template class).  This translation unit
// exists only to ensure the headers compile and link correctly.

namespace gtopt
{
// Explicit instantiation declarations to reduce compile times.
// The definitions are inline in the header.
// (no explicit instantiations needed for inline template classes)
}  // namespace gtopt
