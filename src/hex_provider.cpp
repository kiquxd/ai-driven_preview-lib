#include "detail/internal.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <limits>

namespace preview::detail {

Result<Preview> make_hex(const ByteSource& source, const Request& request,
                         const Probe& probe) noexcept {
  try {
    if (request.byte_offset > probe.source_size) {
      return Error{Error::Code::invalid_request,
                   "hex offset is past the end of the source"};
    }
    const std::uint32_t rows = request.viewport.text_rows == 0
        ? request.limits.max_text_lines
        : std::min(request.viewport.text_rows, request.limits.max_text_lines);
    std::uint32_t bytes_per_row = 16;
    if (request.viewport.text_columns != 0) {
      const auto available = request.viewport.text_columns > 18
          ? (request.viewport.text_columns - 18) / 4
          : 1;
      bytes_per_row = std::clamp<std::uint32_t>(available, 1, 32);
    }
    const std::uint64_t requested =
        static_cast<std::uint64_t>(rows) * bytes_per_row;
    const std::uint64_t wanted = std::min(
        {requested, request.limits.max_text_bytes,
         request.limits.max_total_bytes_read,
         probe.source_size - request.byte_offset});
    if (wanted > std::numeric_limits<std::size_t>::max()) {
      return Error{Error::Code::limit_exceeded,
                   "hex window size is not representable"};
    }
    auto input = read_window_cached(source, probe, request.byte_offset,
                            static_cast<std::size_t>(wanted),
                            request.limits.max_total_bytes_read,
                            request.stop_token);
    if (!input) {
      return input.error();
    }
    TextPreview text;
    text.source_begin = request.byte_offset;
    for (std::size_t row = 0; row < input.value().size(); row += bytes_per_row) {
      char offset_buffer[24];
      std::snprintf(offset_buffer, sizeof(offset_buffer), "%016llx  ",
                    static_cast<unsigned long long>(request.byte_offset + row));
      std::string line(offset_buffer);
      const auto count = std::min<std::size_t>(bytes_per_row,
                                               input.value().size() - row);
      for (std::size_t index = 0; index < bytes_per_row; ++index) {
        if (index < count) {
          char byte_buffer[4];
          std::snprintf(byte_buffer, sizeof(byte_buffer), "%02x ",
                        std::to_integer<unsigned char>(input.value()[row + index]));
          line += byte_buffer;
        } else {
          line += "   ";
        }
      }
      line += " |";
      for (std::size_t index = 0; index < count; ++index) {
        const auto value = std::to_integer<unsigned char>(input.value()[row + index]);
        line.push_back(value >= 0x20 && value <= 0x7e
                           ? static_cast<char>(value)
                           : '.');
      }
      line += '|';
      if (line.size() > request.limits.max_line_bytes) {
        line.resize(request.limits.max_line_bytes);
      }
      text.lines.push_back({std::move(line)});
    }
    text.source_end = request.byte_offset + input.value().size();
    text.next_offset = text.source_end;
    text.has_more = text.source_end < probe.source_size;
    Preview preview;
    preview.detected_mime = "application/octet-stream";
    preview.detected_format = "hex";
    preview.truncated = text.has_more;
    preview.content = std::move(text);
    return preview;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while producing hex preview");
  }
}

}  // namespace preview::detail
