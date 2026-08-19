#pragma once

#include <preview/preview.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace preview::detail {

enum class Format { text, binary, png, jpeg, gif, bmp, pdf, webp };

struct Probe {
  Format format = Format::binary;
  std::vector<Byte> bytes;
  std::uint64_t source_size = 0;
};

Result<Probe> probe_source(const ByteSource&, const Request&) noexcept;
Result<Preview> make_text(const ByteSource&, const Request&, const Probe&) noexcept;
Result<Preview> make_hex(const ByteSource&, const Request&, const Probe&) noexcept;
Result<Preview> make_image(const ByteSource&, const Request&, const Probe&) noexcept;
Result<Preview> make_pdf(const ByteSource&, const Request&, const Probe&) noexcept;
Result<bool> acquire_pdfium() noexcept;
void release_pdfium() noexcept;
void set_stb_allocation_budget(std::size_t) noexcept;
void clear_stb_allocation_budget() noexcept;

Result<std::vector<Byte>> read_range(const ByteSource&, std::uint64_t,
                                     std::size_t, std::stop_token) noexcept;
Result<std::vector<Byte>> read_window_cached(
    const ByteSource&, const Probe&, std::uint64_t, std::size_t,
    std::uint64_t, std::stop_token) noexcept;
Result<std::vector<Byte>> read_all_cached(
    const ByteSource&, const Probe&, std::uint64_t, std::stop_token) noexcept;
Error cancelled_error();
Error backend_error(std::string_view context);

}  // namespace preview::detail
