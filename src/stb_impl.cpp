#include "detail/internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace preview::detail {
namespace {

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size;
};

struct AllocationState {
  std::size_t limit = 0;
  std::size_t used = 0;
  bool active = false;
};

thread_local AllocationState allocation_state;

void* stb_allocate(std::size_t size) noexcept {
  if (!allocation_state.active ||
      size > allocation_state.limit -
                 std::min(allocation_state.used, allocation_state.limit) ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    return nullptr;
  }
  auto* header = static_cast<AllocationHeader*>(
      std::malloc(sizeof(AllocationHeader) + size));
  if (!header) return nullptr;
  header->size = size;
  allocation_state.used += size;
  return header + 1;
}

void stb_deallocate(void* pointer) noexcept {
  if (!pointer) return;
  auto* header = static_cast<AllocationHeader*>(pointer) - 1;
  if (allocation_state.active && header->size <= allocation_state.used) {
    allocation_state.used -= header->size;
  }
  std::free(header);
}

void* stb_reallocate(void* pointer, std::size_t size) noexcept {
  if (!pointer) return stb_allocate(size);
  auto* old_header = static_cast<AllocationHeader*>(pointer) - 1;
  const std::size_t old_size = old_header->size;
  if (!allocation_state.active || old_size > allocation_state.used ||
      size > allocation_state.limit - (allocation_state.used - old_size) ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    return nullptr;
  }
  auto* new_header = static_cast<AllocationHeader*>(
      std::realloc(old_header, sizeof(AllocationHeader) + size));
  if (!new_header) return nullptr;
  new_header->size = size;
  allocation_state.used = allocation_state.used - old_size + size;
  return new_header + 1;
}

}  // namespace

void set_stb_allocation_budget(std::size_t limit) noexcept {
  allocation_state = AllocationState{limit, 0, true};
}

void clear_stb_allocation_budget() noexcept {
  allocation_state = {};
}

}  // namespace preview::detail

#define STBI_MALLOC(size) preview::detail::stb_allocate(size)
#define STBI_REALLOC(pointer, size) \
  preview::detail::stb_reallocate(pointer, size)
#define STBI_REALLOC_SIZED(pointer, old_size, new_size) \
  preview::detail::stb_reallocate(pointer, new_size)
#define STBI_FREE(pointer) preview::detail::stb_deallocate(pointer)
#define STBIR_MALLOC(size, user_data) preview::detail::stb_allocate(size)
#define STBIR_FREE(pointer, user_data) preview::detail::stb_deallocate(pointer)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
