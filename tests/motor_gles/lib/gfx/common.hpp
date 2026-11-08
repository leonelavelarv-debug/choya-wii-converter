#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <dolphin/gx/GXEnum.h>
namespace aurora {
using HashType = uint64_t;
template <typename T> static inline HashType xxh3_hash(const T& input, HashType seed = 0) {
  static_assert(std::has_unique_object_representations_v<T>);
  const unsigned char* p = reinterpret_cast<const unsigned char*>(&input); HashType h = seed;
  for (size_t i = 0; i < sizeof(T); ++i) h = h * 131 + p[i]; return h; }
namespace webgpu { struct Viewport { float left, top, width, height, znear, zfar;
  bool operator==(const Viewport& rhs) const { return left == rhs.left; } }; }
namespace gfx {
using PipelineRef = HashType;
struct Range { uint32_t offset = 0; uint32_t size = 0; };
struct ClipRect { int32_t x, y, width, height; bool operator==(const ClipRect& r) const { return memcmp(this,&r,sizeof(*this))==0; } };
using webgpu::Viewport;
}
}
