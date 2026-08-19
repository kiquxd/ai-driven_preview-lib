#include <preview/preview.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>

namespace preview {
namespace {

class LocalFileSource final : public ByteSource {
 public:
  LocalFileSource(int fd, std::uint64_t size, std::string name)
      : fd_(fd), size_(size), name_(std::move(name)) {}
  ~LocalFileSource() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Result<std::uint64_t> size(std::stop_token stop) const noexcept override {
    if (stop.stop_requested()) {
      return Error{Error::Code::cancelled, "operation cancelled"};
    }
    return size_;
  }

  Result<std::size_t> read_at(std::uint64_t offset,
                              std::span<Byte> destination,
                              std::stop_token stop) const noexcept override {
    if (stop.stop_requested()) {
      return Error{Error::Code::cancelled, "operation cancelled"};
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      return Error{Error::Code::io, "file offset is not representable"};
    }
    for (;;) {
      const auto count = ::pread(fd_, destination.data(), destination.size(),
                                 static_cast<off_t>(offset));
      if (count >= 0) {
        if (stop.stop_requested()) {
          return Error{Error::Code::cancelled, "operation cancelled"};
        }
        return static_cast<std::size_t>(count);
      }
      if (errno != EINTR) {
        return Error{Error::Code::io,
                     std::string("pread failed: ") + std::strerror(errno)};
      }
      if (stop.stop_requested()) {
        return Error{Error::Code::cancelled, "operation cancelled"};
      }
    }
  }

  std::string_view name_hint() const noexcept override { return name_; }
  std::string_view mime_hint() const noexcept override { return {}; }

 private:
  int fd_;
  std::uint64_t size_;
  std::string name_;
};

}  // namespace

Result<std::unique_ptr<ByteSource>> open_local_file(
    const std::filesystem::path& path) noexcept {
  try {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return Error{Error::Code::io,
                   std::string("open failed: ") + std::strerror(errno)};
    }
    struct stat stat_buffer {};
    if (::fstat(fd, &stat_buffer) != 0) {
      const int saved_errno = errno;
      ::close(fd);
      return Error{Error::Code::io,
                   std::string("fstat failed: ") + std::strerror(saved_errno)};
    }
    if (!S_ISREG(stat_buffer.st_mode)) {
      ::close(fd);
      return Error{Error::Code::unsupported,
                   "local source must be a regular file"};
    }
    return std::unique_ptr<ByteSource>(new LocalFileSource(
        fd, static_cast<std::uint64_t>(stat_buffer.st_size),
        path.filename().string()));
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return Error{Error::Code::backend_failure,
                 "unexpected error while opening local file"};
  }
}

}  // namespace preview
