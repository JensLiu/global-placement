#pragma once
#include <Eigen/Dense>
#include <Eigen/Sparse>

#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

template <typename K, typename V, typename Hash = std::hash<K>>
using Map = std::unordered_map<K, V, Hash>;
template <typename T> using Set = std::unordered_set<T>;
template <typename T> using Pair = std::pair<T, T>;
template <typename T> using Vector = std::vector<T>;
