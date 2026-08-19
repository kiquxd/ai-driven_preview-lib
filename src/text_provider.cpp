#include "detail/internal.hpp"

#include <algorithm>
#include <exception>
#include <limits>

namespace preview::detail {
namespace {

void append_utf8(std::string& output, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

struct Decoded {
  std::uint32_t code_point;
  std::size_t bytes;
  bool valid;
};

Decoded decode_utf8(std::span<const Byte> input, std::size_t position) {
  const auto first = std::to_integer<unsigned char>(input[position]);
  if (first < 0x80) {
    return {first, 1, true};
  }
  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    value = first & 0x1f;
    minimum = 0x80;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    value = first & 0x0f;
    minimum = 0x800;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    value = first & 0x07;
    minimum = 0x10000;
  } else {
    return {0xfffd, 1, false};
  }
  if (input.size() - position < length) {
    return {0xfffd, 1, false};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const auto next = std::to_integer<unsigned char>(input[position + index]);
    if ((next & 0xc0) != 0x80) {
      return {0xfffd, 1, false};
    }
    value = (value << 6) | (next & 0x3f);
  }
  if (value < minimum || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) {
    return {0xfffd, 1, false};
  }
  return {value, length, true};
}

std::uint16_t read_u16(std::span<const Byte> input, std::size_t position,
                       bool little_endian) {
  const auto a = std::to_integer<unsigned char>(input[position]);
  const auto b = std::to_integer<unsigned char>(input[position + 1]);
  return little_endian ? static_cast<std::uint16_t>(a | (b << 8))
                       : static_cast<std::uint16_t>((a << 8) | b);
}

void add_warning_once(Preview& preview, std::string code, std::string message) {
  for (const auto& warning : preview.warnings) {
    if (warning.code == code) {
      return;
    }
  }
  preview.warnings.push_back({std::move(code), std::move(message)});
}

}  // namespace

Result<Preview> make_text(const ByteSource& source, const Request& request,
                          const Probe& probe) noexcept {
  try {
    if (request.stop_token.stop_requested()) {
      return cancelled_error();
    }
    if (request.byte_offset > probe.source_size) {
      return Error{Error::Code::invalid_request,
                   "text offset is past the end of the source"};
    }
    const bool utf16_le = probe.bytes.size() >= 2 &&
        std::to_integer<unsigned char>(probe.bytes[0]) == 0xff &&
        std::to_integer<unsigned char>(probe.bytes[1]) == 0xfe;
    const bool utf16_be = probe.bytes.size() >= 2 &&
        std::to_integer<unsigned char>(probe.bytes[0]) == 0xfe &&
        std::to_integer<unsigned char>(probe.bytes[1]) == 0xff;

    std::uint64_t begin = request.byte_offset;
    Preview preview;
    preview.detected_mime = "text/plain";
    preview.detected_format = utf16_le ? "utf-16le" : utf16_be ? "utf-16be" : "utf-8";
    if (begin == 0) {
      if (utf16_le || utf16_be) {
        begin = 2;
      } else if (probe.bytes.size() >= 3 &&
                 std::to_integer<unsigned char>(probe.bytes[0]) == 0xef &&
                 std::to_integer<unsigned char>(probe.bytes[1]) == 0xbb &&
                 std::to_integer<unsigned char>(probe.bytes[2]) == 0xbf) {
        begin = 3;
      }
    }
    if (utf16_le || utf16_be) {
      const std::uint64_t aligned = begin < 2 ? 2 : begin + ((begin - 2) & 1u);
      if (aligned != begin) {
        begin = aligned;
        add_warning_once(preview, "offset_adjusted",
                         "offset was advanced to a UTF-16 code-unit boundary");
      }
    }
    const std::uint64_t available = probe.source_size - begin;
    const std::uint64_t byte_limit = std::min(
        {request.limits.max_text_bytes, request.limits.max_total_bytes_read,
         available});
    if (byte_limit > std::numeric_limits<std::size_t>::max()) {
      return Error{Error::Code::limit_exceeded,
                   "text window size is not representable"};
    }
    auto input_result = read_window_cached(source, probe, begin,
        static_cast<std::size_t>(byte_limit),
        request.limits.max_total_bytes_read, request.stop_token);
    if (!input_result) {
      return input_result.error();
    }
    auto& input = input_result.value();
    std::size_t position = 0;
    if (!utf16_le && !utf16_be && request.byte_offset != 0) {
      while (position < input.size() &&
             (std::to_integer<unsigned char>(input[position]) & 0xc0) == 0x80) {
        ++position;
      }
      if (position != 0) {
        add_warning_once(preview, "offset_adjusted",
                         "offset was advanced to a UTF-8 code-point boundary");
      }
    }

    TextPreview text;
    text.source_begin = begin + position;
    std::string line;
    bool line_truncated = false;
    std::size_t output_bytes = 0;
    const std::uint32_t row_limit = request.viewport.text_rows == 0
        ? request.limits.max_text_lines
        : std::min(request.viewport.text_rows, request.limits.max_text_lines);

    auto finish_line = [&]() -> bool {
      if (text.lines.size() >= row_limit) {
        return false;
      }
      output_bytes += line.size();
      text.lines.push_back({std::move(line)});
      line.clear();
      if (line_truncated) {
        add_warning_once(preview, "line_truncated",
                         "one or more lines exceeded max_line_bytes");
        line_truncated = false;
      }
      return true;
    };

    while (position < input.size() && text.lines.size() < row_limit) {
      if (request.stop_token.stop_requested()) {
        return cancelled_error();
      }
      const std::size_t unit_begin = position;
      Decoded decoded{};
      if (utf16_le || utf16_be) {
        if (input.size() - position < 2) {
          if (begin + input.size() < probe.source_size) break;
          decoded = {0xfffd, input.size() - position, false};
        } else {
          const auto first = read_u16(input, position, utf16_le);
          decoded = {first, 2, true};
          if (first >= 0xd800 && first <= 0xdbff && input.size() - position >= 4) {
            const auto second = read_u16(input, position + 2, utf16_le);
            if (second >= 0xdc00 && second <= 0xdfff) {
              decoded.code_point = 0x10000 +
                  ((static_cast<std::uint32_t>(first) - 0xd800) << 10) +
                  (static_cast<std::uint32_t>(second) - 0xdc00);
              decoded.bytes = 4;
            } else {
              decoded = {0xfffd, 2, false};
            }
          } else if (first >= 0xd800 && first <= 0xdbff &&
                     begin + input.size() < probe.source_size) {
            break;
          } else if (first >= 0xd800 && first <= 0xdfff) {
            decoded = {0xfffd, 2, false};
          }
        }
      } else {
        const auto first = std::to_integer<unsigned char>(input[position]);
        const std::size_t expected = first < 0x80 ? 1
            : (first & 0xe0) == 0xc0 ? 2
            : (first & 0xf0) == 0xe0 ? 3
            : (first & 0xf8) == 0xf0 ? 4 : 1;
        if (input.size() - position < expected &&
            begin + input.size() < probe.source_size) {
          break;
        }
        decoded = decode_utf8(input, position);
      }
      position += decoded.bytes;
      if (!decoded.valid) {
        add_warning_once(preview, "invalid_encoding",
                         "invalid input sequences were replaced with U+FFFD");
      }
      if (decoded.code_point == '\r') {
        if (utf16_le || utf16_be) {
          if (input.size() - position >= 2 &&
              read_u16(input, position, utf16_le) == '\n') {
            position += 2;
          }
        } else if (position < input.size() &&
                   std::to_integer<unsigned char>(input[position]) == '\n') {
          ++position;
        }
        if (!finish_line()) {
          position = unit_begin;
          break;
        }
        continue;
      }
      if (decoded.code_point == '\n') {
        if (!finish_line()) {
          position = unit_begin;
          break;
        }
        continue;
      }
      std::string encoded;
      append_utf8(encoded, decoded.code_point);
      if (line.size() + encoded.size() <= request.limits.max_line_bytes &&
          output_bytes + line.size() + encoded.size() <=
              request.limits.max_output_bytes) {
        line += encoded;
      } else {
        line_truncated = true;
        preview.truncated = true;
      }
    }
    if ((position > 0 || input.empty()) && !line.empty() &&
        text.lines.size() < row_limit) {
      finish_line();
    }
    text.source_end = begin + position;
    text.next_offset = text.source_end;
    text.has_more = text.source_end < probe.source_size;
    preview.truncated = preview.truncated || text.has_more;
    preview.content = std::move(text);
    return preview;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while decoding text");
  }
}

}  // namespace preview::detail
