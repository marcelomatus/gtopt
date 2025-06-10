#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/fmap.hpp>
#include <tuple>

namespace gtopt::input {

template<typename... Uid>
struct UidTraits {
    using Key = std::tuple<Uid...>;
    using ArrowMap = gtopt::flat_map<Key, gtopt::ArrowIndex>;
    using VectorMap = gtopt::flat_map<Key, gtopt::Index>;
    using ArrowMapPtr = std::shared_ptr<ArrowMap>;
    using VectorMapPtr = std::shared_ptr<VectorMap>;
};

} // namespace gtopt::input
