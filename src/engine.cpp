#include "detail/internal.hpp"

#include <exception>

namespace preview {

struct Engine::Impl {
  ~Impl() { detail::release_pdfium(); }
};

Engine::Engine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

Result<Engine> Engine::create() noexcept {
  auto acquired = detail::acquire_pdfium();
  if (!acquired) return acquired.error();
  try {
    return Engine(std::make_unique<Impl>());
  } catch (const std::exception& error) {
    detail::release_pdfium();
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    detail::release_pdfium();
    return Error{Error::Code::backend_failure,
                 "unexpected error while creating preview engine"};
  }
}

Result<Preview> Engine::make_preview(const ByteSource& source,
                                     const Request& request) const noexcept {
  try {
    if (!impl_) {
      return Error{Error::Code::invalid_request,
                   "cannot use a moved-from preview engine"};
    }
    if (request.stop_token.stop_requested()) {
      return detail::cancelled_error();
    }
    if (request.limits.max_probe_bytes == 0 ||
        request.limits.max_total_bytes_read == 0 ||
        request.limits.max_output_bytes == 0 ||
        request.limits.max_text_lines == 0 ||
        request.limits.max_line_bytes == 0) {
      return Error{Error::Code::invalid_request,
                   "required preview limits must be non-zero"};
    }
    auto probe = detail::probe_source(source, request);
    if (!probe) return probe.error();

    if (request.mode == Mode::hex) {
      return detail::make_hex(source, request, probe.value());
    }
    using detail::Format;
    switch (probe.value().format) {
      case Format::text:
        if (request.mode == Mode::visual) {
          return Error{Error::Code::unsupported,
                       "text has no visual pixel provider"};
        }
        if (request.mode == Mode::metadata) {
          Preview result;
          result.detected_mime = "text/plain";
          result.detected_format = "text";
          result.metadata.items.push_back(
              {"size", std::to_string(probe.value().source_size)});
          return result;
        }
        return detail::make_text(source, request, probe.value());
      case Format::png:
      case Format::jpeg:
      case Format::gif:
      case Format::bmp:
        return detail::make_image(source, request, probe.value());
      case Format::pdf:
        return detail::make_pdf(source, request, probe.value());
      case Format::webp: {
        if (request.mode == Mode::visual) {
          return Error{Error::Code::unsupported,
                       "WebP support is intentionally disabled"};
        }
        Preview result;
        result.detected_mime = "image/webp";
        result.detected_format = "webp";
        result.content = UnsupportedContent{
            "WebP support is intentionally disabled"};
        return result;
      }
      case Format::binary:
        if (request.mode == Mode::visual) {
          return Error{Error::Code::unsupported,
                       "binary input has no visual provider"};
        }
        if (request.mode == Mode::metadata) {
          Preview result;
          result.detected_mime = "application/octet-stream";
          result.detected_format = "binary";
          result.metadata.items.push_back(
              {"size", std::to_string(probe.value().source_size)});
          return result;
        }
        return detail::make_hex(source, request, probe.value());
    }
    return Error{Error::Code::backend_failure, "unreachable provider state"};
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return Error{Error::Code::backend_failure,
                 "unexpected error while making preview"};
  }
}

}  // namespace preview
