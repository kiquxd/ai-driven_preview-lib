#include "detail/internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>

namespace preview::detail {
namespace {

bool starts_with(std::span<const Byte> data,
                 std::initializer_list<unsigned char> bytes) {
  if (data.size() < bytes.size()) {
    return false;
  }
  std::size_t index = 0;
  for (const auto value : bytes) {
    if (std::to_integer<unsigned char>(data[index++]) != value) {
      return false;
    }
  }
  return true;
}

bool is_likely_text(std::span<const Byte> bytes) {
  if (bytes.empty()) {
    return true;
  }
  if (starts_with(bytes, {0xef, 0xbb, 0xbf}) ||
      starts_with(bytes, {0xff, 0xfe}) || starts_with(bytes, {0xfe, 0xff})) {
    return true;
  }
  std::size_t suspicious = 0;
  for (const Byte byte : bytes) {
    const auto value = std::to_integer<unsigned char>(byte);
    if (value == 0) {
      return false;
    }
    if (value < 0x20 && value != '\n' && value != '\r' && value != '\t' &&
        value != '\f' && value != '\b') {
      ++suspicious;
    }
  }
  return suspicious * 50 <= bytes.size();
}

}  // namespace

Result<Probe> probe_source(const ByteSource& source,
                           const Request& request) noexcept {
  try {
    if (request.stop_token.stop_requested()) {
      return cancelled_error();
    }
    auto size = source.size(request.stop_token);
    if (!size) {
      return size.error();
    }
    const std::uint64_t probe_limit =
        std::min(request.limits.max_probe_bytes,
                 request.limits.max_total_bytes_read);
    const std::uint64_t wanted64 = std::min(size.value(), probe_limit);
    if (wanted64 > std::numeric_limits<std::size_t>::max()) {
      return Error{Error::Code::limit_exceeded, "probe size is not representable"};
    }
    auto bytes = read_range(source, 0, static_cast<std::size_t>(wanted64),
                            request.stop_token);
    if (!bytes) {
      return bytes.error();
    }
    if (bytes.value().size() != wanted64) {
      return Error{Error::Code::io,
                   "source ended before its reported size during probe"};
    }
    Probe probe;
    probe.source_size = size.value();
    probe.bytes = std::move(bytes.value());
    const std::span<const Byte> data(probe.bytes);

    if (starts_with(data, {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a})) {
      probe.format = Format::png;
    } else if (starts_with(data, {0xff, 0xd8, 0xff})) {
      probe.format = Format::jpeg;
    } else if (starts_with(data, {'G', 'I', 'F', '8', '7', 'a'}) ||
               starts_with(data, {'G', 'I', 'F', '8', '9', 'a'})) {
      probe.format = Format::gif;
    } else if (starts_with(data, {'B', 'M'})) {
      probe.format = Format::bmp;
    } else if (starts_with(data, {'%', 'P', 'D', 'F', '-'})) {
      probe.format = Format::pdf;
    } else if (data.size() >= 12 &&
               starts_with(data, {'R', 'I', 'F', 'F'}) &&
               std::memcmp(data.data() + 8, "WEBP", 4) == 0) {
      probe.format = Format::webp;
    } else {
      probe.format = is_likely_text(data) ? Format::text : Format::binary;
    }
    return probe;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while probing source");
  }
}

}  // namespace preview::detail
