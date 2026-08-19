#include "detail/internal.hpp"

#include <algorithm>
#include <exception>
#include <limits>

namespace preview::detail {

Error cancelled_error() {
  return Error{Error::Code::cancelled, "operation cancelled"};
}

Error backend_error(std::string_view context) {
  return Error{Error::Code::backend_failure, std::string(context)};
}

Result<std::vector<Byte>> read_range(const ByteSource& source,
                                     std::uint64_t offset,
                                     std::size_t count,
                                     std::stop_token stop) noexcept {
  try {
    if (stop.stop_requested()) {
      return cancelled_error();
    }
    std::vector<Byte> result(count);
    std::size_t done = 0;
    while (done < count) {
      if (stop.stop_requested()) {
        return cancelled_error();
      }
      if (offset > std::numeric_limits<std::uint64_t>::max() - done) {
        return Error{Error::Code::invalid_request, "read offset overflow"};
      }
      auto read = source.read_at(offset + done,
                                 std::span<Byte>(result).subspan(done), stop);
      if (!read) {
        return read.error();
      }
      if (stop.stop_requested()) {
        return cancelled_error();
      }
      if (read.value() > count - done) {
        return Error{Error::Code::io,
                     "byte source returned more bytes than requested"};
      }
      if (read.value() == 0) {
        break;
      }
      done += read.value();
    }
    result.resize(done);
    return result;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while reading source");
  }
}

Result<std::vector<Byte>> read_window_cached(
    const ByteSource& source, const Probe& probe, std::uint64_t offset,
    std::size_t count, std::uint64_t total_limit,
    std::stop_token stop) noexcept {
  try {
    if (stop.stop_requested()) return cancelled_error();
    if (probe.bytes.size() > total_limit) {
      return Error{Error::Code::limit_exceeded,
                   "probe already exceeds total read budget"};
    }
    std::vector<Byte> result;
    result.reserve(count);
    std::uint64_t cursor = offset;
    if (cursor < probe.bytes.size()) {
      const auto cached = std::min<std::size_t>(
          count, probe.bytes.size() - static_cast<std::size_t>(cursor));
      result.insert(result.end(), probe.bytes.begin() +
          static_cast<std::ptrdiff_t>(cursor), probe.bytes.begin() +
          static_cast<std::ptrdiff_t>(cursor + cached));
      cursor += cached;
    }
    const std::size_t missing = count - result.size();
    const std::uint64_t additional_budget = total_limit - probe.bytes.size();
    const std::size_t permitted = static_cast<std::size_t>(std::min<std::uint64_t>(
        missing, std::min<std::uint64_t>(additional_budget,
            std::numeric_limits<std::size_t>::max())));
    if (permitted != 0) {
      auto tail = read_range(source, cursor, permitted, stop);
      if (!tail) return tail.error();
      if (tail.value().size() != permitted &&
          cursor + tail.value().size() < probe.source_size) {
        return Error{Error::Code::io,
                     "source ended before its reported size"};
      }
      result.insert(result.end(), tail.value().begin(), tail.value().end());
    }
    return result;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while reading cached source window");
  }
}

Result<std::vector<Byte>> read_all_cached(
    const ByteSource& source, const Probe& probe, std::uint64_t limit,
    std::stop_token stop) noexcept {
  if (probe.source_size > limit ||
      probe.source_size > std::numeric_limits<std::size_t>::max()) {
    return Error{Error::Code::limit_exceeded,
                 "encoded input exceeds configured byte limit"};
  }
  auto result = read_window_cached(source, probe, 0,
      static_cast<std::size_t>(probe.source_size), limit, stop);
  if (!result) return result.error();
  if (result.value().size() != probe.source_size) {
    return Error{Error::Code::io, "source ended before its reported size"};
  }
  return result;
}

}  // namespace preview::detail
