#pragma once
#include <unordered_map>
namespace absl { template <class K, class V> using flat_hash_map = std::unordered_map<K, V>; }
