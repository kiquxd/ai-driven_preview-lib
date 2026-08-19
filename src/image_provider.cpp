#include "detail/internal.hpp"

#include <jpeglibmangler.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>

namespace preview::detail {
namespace {

struct ImageInfo {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t orientation = 1;
  bool animated = false;
};

class StbBudgetScope {
 public:
  explicit StbBudgetScope(std::size_t limit) {
    set_stb_allocation_budget(limit);
  }
  ~StbBudgetScope() { clear_stb_allocation_budget(); }
  StbBudgetScope(const StbBudgetScope&) = delete;
  StbBudgetScope& operator=(const StbBudgetScope&) = delete;
};

std::uint16_t be16(const Byte* data) {
  return static_cast<std::uint16_t>(
      (std::to_integer<unsigned char>(data[0]) << 8) |
      std::to_integer<unsigned char>(data[1]));
}

std::uint32_t be32(const Byte* data) {
  return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[2])) << 8) |
         std::to_integer<unsigned char>(data[3]);
}

std::uint32_t le32(const Byte* data) {
  return std::to_integer<unsigned char>(data[0]) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[1])) << 8) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[2])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[3])) << 24);
}

std::uint16_t le16(const Byte* data) {
  return static_cast<std::uint16_t>(
      std::to_integer<unsigned char>(data[0]) |
      (static_cast<std::uint16_t>(
           std::to_integer<unsigned char>(data[1])) << 8));
}

bool checked_raster(std::uint32_t width, std::uint32_t height,
                    std::size_t& result) {
  const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
  if (pixels > std::numeric_limits<std::size_t>::max() / 4) {
    return false;
  }
  result = static_cast<std::size_t>(pixels * 4);
  return true;
}

std::uint16_t parse_exif_orientation(std::span<const Byte> payload) {
  if (payload.size() < 14 || std::memcmp(payload.data(), "Exif\0\0", 6) != 0) {
    return 1;
  }
  const Byte* tiff = payload.data() + 6;
  const std::size_t size = payload.size() - 6;
  const bool little = size >= 8 && std::memcmp(tiff, "II", 2) == 0;
  const bool big = size >= 8 && std::memcmp(tiff, "MM", 2) == 0;
  if (!little && !big) {
    return 1;
  }
  auto u16 = [&](std::size_t offset) -> std::uint16_t {
    if (offset + 2 > size) return 0;
    return little
        ? static_cast<std::uint16_t>(std::to_integer<unsigned char>(tiff[offset]) |
              (std::to_integer<unsigned char>(tiff[offset + 1]) << 8))
        : be16(tiff + offset);
  };
  auto u32 = [&](std::size_t offset) -> std::uint32_t {
    if (offset + 4 > size) return 0;
    return little ? le32(tiff + offset) : be32(tiff + offset);
  };
  if (u16(2) != 42) return 1;
  const std::uint32_t ifd = u32(4);
  if (ifd > size || size - ifd < 2) return 1;
  const std::uint16_t entries = u16(ifd);
  for (std::uint16_t index = 0; index < entries; ++index) {
    const std::uint64_t entry = static_cast<std::uint64_t>(ifd) + 2 + index * 12;
    if (entry + 12 > size) break;
    if (u16(entry) == 0x0112 && u16(entry + 2) == 3 && u32(entry + 4) == 1) {
      const auto value = u16(entry + 8);
      return value >= 1 && value <= 8 ? value : 1;
    }
  }
  return 1;
}

Result<ImageInfo> parse_info(const Probe& probe) {
  const auto data = std::span<const Byte>(probe.bytes);
  ImageInfo info;
  switch (probe.format) {
    case Format::png:
      if (data.size() < 24 || std::memcmp(data.data() + 12, "IHDR", 4) != 0) {
        return Error{Error::Code::invalid_data, "truncated PNG header"};
      }
      info.width = be32(data.data() + 16);
      info.height = be32(data.data() + 20);
      break;
    case Format::gif:
      if (data.size() < 10) {
        return Error{Error::Code::invalid_data, "truncated GIF header"};
      }
      info.width = le16(data.data() + 6);
      info.height = le16(data.data() + 8);
      info.animated = std::search(data.begin(), data.end(),
          reinterpret_cast<const Byte*>("NETSCAPE2.0"),
          reinterpret_cast<const Byte*>("NETSCAPE2.0") + 11) != data.end();
      break;
    case Format::bmp: {
      if (data.size() < 26) {
        return Error{Error::Code::invalid_data, "truncated BMP header"};
      }
      const std::uint32_t dib = le32(data.data() + 14);
      if (dib == 12) {
        info.width = le16(data.data() + 18);
        info.height = le16(data.data() + 20);
      } else if (dib >= 40 && data.size() >= 26) {
        const auto width = static_cast<std::int32_t>(le32(data.data() + 18));
        const auto height = static_cast<std::int32_t>(le32(data.data() + 22));
        if (width <= 0 || height == 0 || height == std::numeric_limits<std::int32_t>::min()) {
          return Error{Error::Code::invalid_data, "invalid BMP dimensions"};
        }
        info.width = static_cast<std::uint32_t>(width);
        info.height = static_cast<std::uint32_t>(height < 0 ? -height : height);
      } else {
        return Error{Error::Code::unsupported, "unsupported BMP DIB header"};
      }
      break;
    }
    case Format::jpeg: {
      std::size_t position = 2;
      while (position + 4 <= data.size()) {
        if (std::to_integer<unsigned char>(data[position]) != 0xff) {
          ++position;
          continue;
        }
        while (position < data.size() &&
               std::to_integer<unsigned char>(data[position]) == 0xff) ++position;
        if (position >= data.size()) break;
        const auto marker = std::to_integer<unsigned char>(data[position++]);
        if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (position + 2 > data.size()) break;
        const std::uint16_t length = be16(data.data() + position);
        if (length < 2 || position + length > data.size()) break;
        const auto payload = data.subspan(position + 2, length - 2);
        if (marker == 0xe1) info.orientation = parse_exif_orientation(payload);
        if ((marker >= 0xc0 && marker <= 0xc3) ||
            (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) ||
            (marker >= 0xcd && marker <= 0xcf)) {
          if (payload.size() < 5) break;
          info.height = be16(payload.data() + 1);
          info.width = be16(payload.data() + 3);
        }
        position += length;
      }
      if (info.width == 0 || info.height == 0) {
        return Error{Error::Code::invalid_data,
                     "JPEG dimensions were not found in the probe window"};
      }
      break;
    }
    default:
      return Error{Error::Code::unsupported, "not a supported image"};
  }
  if (info.width == 0 || info.height == 0) {
    return Error{Error::Code::invalid_data, "image has zero dimensions"};
  }
  return info;
}

const char* format_name(Format format) {
  switch (format) {
    case Format::png: return "png";
    case Format::jpeg: return "jpeg";
    case Format::gif: return "gif";
    case Format::bmp: return "bmp";
    default: return "unknown";
  }
}

const char* mime_name(Format format) {
  switch (format) {
    case Format::png: return "image/png";
    case Format::jpeg: return "image/jpeg";
    case Format::gif: return "image/gif";
    case Format::bmp: return "image/bmp";
    default: return "application/octet-stream";
  }
}

struct JpegError {
  jpeg_error_mgr base;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX]{};
};

void jpeg_failure(j_common_ptr common) {
  auto* error = reinterpret_cast<JpegError*>(common->err);
  (*common->err->format_message)(common, error->message);
  longjmp(error->jump, 1);
}

Result<std::vector<Byte>> decode_jpeg(std::span<const Byte> encoded,
                                     std::uint32_t target_width,
                                     std::uint32_t target_height,
                                     std::uint32_t& width,
                                     std::uint32_t& height,
                                     std::stop_token stop) {
  jpeg_decompress_struct decoder{};
  JpegError error{};
  unsigned char* raw_pixels = nullptr;
  decoder.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpeg_failure;
  if (setjmp(error.jump) != 0) {
    std::free(raw_pixels);
    jpeg_destroy_decompress(&decoder);
    return Error{Error::Code::invalid_data,
                 std::string("JPEG decode failed: ") + error.message};
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder,
      reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
  jpeg_read_header(&decoder, TRUE);
  for (unsigned denominator : {8u, 4u, 2u, 1u}) {
    decoder.scale_num = 1;
    decoder.scale_denom = denominator;
    jpeg_calc_output_dimensions(&decoder);
    if (decoder.output_width >= target_width && decoder.output_height >= target_height) {
      break;
    }
  }
  decoder.out_color_space = JCS_EXT_RGBA;
  jpeg_start_decompress(&decoder);
  width = decoder.output_width;
  height = decoder.output_height;
  std::size_t bytes = 0;
  if (!checked_raster(width, height, bytes)) {
    jpeg_destroy_decompress(&decoder);
    return Error{Error::Code::limit_exceeded, "JPEG raster size overflow"};
  }
  raw_pixels = static_cast<unsigned char*>(std::malloc(bytes));
  if (raw_pixels == nullptr) {
    jpeg_destroy_decompress(&decoder);
    return Error{Error::Code::backend_failure,
                 "memory allocation failed while decoding JPEG"};
  }
  while (decoder.output_scanline < decoder.output_height) {
    if (stop.stop_requested()) {
      jpeg_abort_decompress(&decoder);
      jpeg_destroy_decompress(&decoder);
      std::free(raw_pixels);
      return cancelled_error();
    }
    auto* row = reinterpret_cast<JSAMPLE*>(raw_pixels +
        static_cast<std::size_t>(decoder.output_scanline) * width * 4);
    JSAMPROW rows[] = {row};
    jpeg_read_scanlines(&decoder, rows, 1);
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  std::vector<Byte> pixels(bytes);
  std::memcpy(pixels.data(), raw_pixels, bytes);
  std::free(raw_pixels);
  return pixels;
}

Result<std::vector<Byte>> decode_stb(std::span<const Byte> encoded,
                                    std::uint32_t& width,
                                    std::uint32_t& height) {
  if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Error{Error::Code::limit_exceeded, "encoded image is too large for decoder"};
  }
  int decoded_width = 0;
  int decoded_height = 0;
  int channels = 0;
  stbi_uc* raw = stbi_load_from_memory(
      reinterpret_cast<const stbi_uc*>(encoded.data()),
      static_cast<int>(encoded.size()), &decoded_width, &decoded_height,
      &channels, 4);
  if (raw == nullptr) {
    return Error{Error::Code::invalid_data,
                 std::string("image decode failed: ") + stbi_failure_reason()};
  }
  if (decoded_width <= 0 || decoded_height <= 0) {
    stbi_image_free(raw);
    return Error{Error::Code::invalid_data, "decoder returned invalid dimensions"};
  }
  width = static_cast<std::uint32_t>(decoded_width);
  height = static_cast<std::uint32_t>(decoded_height);
  std::size_t bytes = 0;
  if (!checked_raster(width, height, bytes)) {
    stbi_image_free(raw);
    return Error{Error::Code::limit_exceeded, "decoded raster size overflow"};
  }
  std::vector<Byte> pixels(bytes);
  std::memcpy(pixels.data(), raw, bytes);
  stbi_image_free(raw);
  return pixels;
}

Result<std::vector<Byte>> orient_pixels(std::vector<Byte> source,
                                        std::uint32_t& width,
                                        std::uint32_t& height,
                                        std::uint16_t orientation) {
  if (orientation == 1) return source;
  const bool swap_dimensions = orientation >= 5;
  const std::uint32_t output_width = swap_dimensions ? height : width;
  const std::uint32_t output_height = swap_dimensions ? width : height;
  std::vector<Byte> output(source.size());
  for (std::uint32_t y = 0; y < output_height; ++y) {
    for (std::uint32_t x = 0; x < output_width; ++x) {
      std::uint32_t source_x = x;
      std::uint32_t source_y = y;
      switch (orientation) {
        case 2: source_x = width - 1 - x; break;
        case 3: source_x = width - 1 - x; source_y = height - 1 - y; break;
        case 4: source_y = height - 1 - y; break;
        case 5: source_x = y; source_y = x; break;
        case 6: source_x = y; source_y = height - 1 - x; break;
        case 7: source_x = width - 1 - y; source_y = height - 1 - x; break;
        case 8: source_x = width - 1 - y; source_y = x; break;
        default: break;
      }
      const std::size_t source_offset =
          (static_cast<std::size_t>(source_y) * width + source_x) * 4;
      const std::size_t output_offset =
          (static_cast<std::size_t>(y) * output_width + x) * 4;
      std::memcpy(output.data() + output_offset,
                  source.data() + source_offset, 4);
    }
  }
  width = output_width;
  height = output_height;
  return output;
}

}  // namespace

Result<Preview> make_image(const ByteSource& source, const Request& request,
                           const Probe& probe) noexcept {
  try {
    auto info_result = parse_info(probe);
    if (!info_result) return info_result.error();
    const auto info = info_result.value();
    Preview preview;
    preview.detected_format = format_name(probe.format);
    preview.detected_mime = mime_name(probe.format);
    preview.metadata.items.push_back({"width", std::to_string(info.width)});
    preview.metadata.items.push_back({"height", std::to_string(info.height)});
    if (probe.format == Format::jpeg) {
      preview.metadata.items.push_back({"orientation", std::to_string(info.orientation)});
    }
    if (probe.format == Format::gif) {
      preview.metadata.items.push_back({"animated", info.animated ? "true" : "false"});
      preview.warnings.push_back({"first_frame_only",
                                  "GIF preview renders only the first frame"});
    }
    if (request.mode == Mode::metadata ||
        request.viewport.target_pixel_width == 0 ||
        request.viewport.target_pixel_height == 0) {
      if (request.mode == Mode::visual) {
        return Error{Error::Code::invalid_request,
                     "visual image preview requires non-zero pixel dimensions"};
      }
      return preview;
    }
    if (info.width > request.limits.max_pixel_dimension ||
        info.height > request.limits.max_pixel_dimension) {
      if (probe.format != Format::jpeg) {
        return Error{Error::Code::limit_exceeded,
                     "source image dimensions exceed configured limit"};
      }
    }
    const bool swaps_orientation = info.orientation >= 5;
    const std::uint32_t display_width = swaps_orientation ? info.height : info.width;
    const std::uint32_t display_height = swaps_orientation ? info.width : info.height;
    const double scale = std::min(
        static_cast<double>(request.viewport.target_pixel_width) / display_width,
        static_cast<double>(request.viewport.target_pixel_height) / display_height);
    const double bounded_scale = std::min(1.0, scale);
    const auto target_width = std::max<std::uint32_t>(1,
        static_cast<std::uint32_t>(std::floor(display_width * bounded_scale)));
    const auto target_height = std::max<std::uint32_t>(1,
        static_cast<std::uint32_t>(std::floor(display_height * bounded_scale)));
    const std::uint64_t target_pixels =
        static_cast<std::uint64_t>(target_width) * target_height;
    if (target_pixels > std::numeric_limits<std::uint64_t>::max() / 4 ||
        target_width > request.limits.max_pixel_dimension ||
        target_height > request.limits.max_pixel_dimension ||
        target_pixels > request.limits.max_pixels ||
        target_pixels * 4 > request.limits.max_output_bytes) {
      return Error{Error::Code::limit_exceeded,
                   "requested image output exceeds configured limits"};
    }
    std::size_t source_raster_bytes = 0;
    if (!checked_raster(info.width, info.height, source_raster_bytes)) {
      return Error{Error::Code::limit_exceeded, "source raster size overflow"};
    }
    std::uint64_t estimated_decode_bytes = source_raster_bytes;
    if (probe.format == Format::jpeg) {
      const std::uint32_t decode_target_width = swaps_orientation
          ? target_height : target_width;
      const std::uint32_t decode_target_height = swaps_orientation
          ? target_width : target_height;
      unsigned denominator = 1;
      for (const unsigned candidate : {8u, 4u, 2u, 1u}) {
        const auto scaled_width = (info.width + candidate - 1) / candidate;
        const auto scaled_height = (info.height + candidate - 1) / candidate;
        denominator = candidate;
        if (scaled_width >= decode_target_width &&
            scaled_height >= decode_target_height) break;
      }
      std::size_t estimated = 0;
      if (!checked_raster((info.width + denominator - 1) / denominator,
                          (info.height + denominator - 1) / denominator,
                          estimated)) {
        return Error{Error::Code::limit_exceeded,
                     "scaled JPEG raster size overflow"};
      }
      estimated_decode_bytes = estimated;
    }
    constexpr std::uint64_t decoder_copies = 2;
    const std::uint64_t orientation_copy = info.orientation == 1 ? 0 : estimated_decode_bytes;
    std::uint64_t estimated_working = probe.source_size;
    bool working_overflow = false;
    auto add_working = [&](std::uint64_t amount) {
      if (amount > std::numeric_limits<std::uint64_t>::max() -
                       estimated_working) {
        working_overflow = true;
      } else {
        estimated_working += amount;
      }
    };
    add_working(probe.bytes.size());
    if (estimated_decode_bytes >
        std::numeric_limits<std::uint64_t>::max() / decoder_copies) {
      working_overflow = true;
    } else {
      add_working(estimated_decode_bytes * decoder_copies);
    }
    add_working(orientation_copy);
    add_working(target_pixels * 4);
    if (working_overflow || estimated_working > request.limits.max_working_bytes) {
      return Error{Error::Code::limit_exceeded,
                   "image decode would exceed max_working_bytes"};
    }
    auto encoded = read_all_cached(source, probe,
        std::min(request.limits.max_encoded_image_bytes,
                 request.limits.max_total_bytes_read), request.stop_token);
    if (!encoded) return encoded.error();
    if (request.stop_token.stop_requested()) return cancelled_error();

    std::uint32_t decoded_width = 0;
    std::uint32_t decoded_height = 0;
    Result<std::vector<Byte>> decoded = Error{
        Error::Code::backend_failure, "image decoder was not selected"};
    if (probe.format == Format::jpeg) {
      decoded = decode_jpeg(encoded.value(),
                            swaps_orientation ? target_height : target_width,
                            swaps_orientation ? target_width : target_height,
                            decoded_width, decoded_height, request.stop_token);
    } else {
      const auto available_budget = request.limits.max_working_bytes >
              encoded.value().size()
          ? request.limits.max_working_bytes - encoded.value().size()
          : 0;
      StbBudgetScope budget(available_budget);
      decoded = decode_stb(encoded.value(), decoded_width, decoded_height);
    }
    if (!decoded) return decoded.error();
    if (request.stop_token.stop_requested()) return cancelled_error();
    if (decoded_width != info.width || decoded_height != info.height) {
      if (probe.format != Format::jpeg) {
        return Error{Error::Code::invalid_data,
                     "decoded dimensions disagree with image header"};
      }
    }
    auto oriented = orient_pixels(std::move(decoded.value()), decoded_width,
                                  decoded_height, info.orientation);
    if (!oriented) return oriented.error();
    decoded = std::move(oriented.value());

    std::vector<Byte> output(static_cast<std::size_t>(target_pixels) * 4);
    if (decoded_width == target_width && decoded_height == target_height) {
      output = std::move(decoded.value());
    } else {
      const auto occupied = encoded.value().size() + decoded.value().size() +
                            output.size();
      const auto resize_budget = request.limits.max_working_bytes > occupied
          ? request.limits.max_working_bytes - occupied
          : 0;
      StbBudgetScope budget(resize_budget);
      const auto result = stbir_resize_uint8_srgb(
          reinterpret_cast<const unsigned char*>(decoded.value().data()),
          static_cast<int>(decoded_width), static_cast<int>(decoded_height), 0,
          reinterpret_cast<unsigned char*>(output.data()),
          static_cast<int>(target_width), static_cast<int>(target_height), 0,
          STBIR_RGBA);
      if (result == nullptr) {
        return Error{Error::Code::backend_failure, "image resize failed"};
      }
    }
    if (request.pixel_format == PixelFormat::bgra8) {
      for (std::size_t index = 0; index < output.size(); index += 4) {
        std::swap(output[index], output[index + 2]);
      }
    }
    PixelPreview pixels;
    pixels.width = target_width;
    pixels.height = target_height;
    pixels.stride = target_width * 4;
    pixels.format = request.pixel_format;
    pixels.pixels = std::move(output);
    preview.content = std::move(pixels);
    return preview;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while decoding image");
  }
}

}  // namespace preview::detail
