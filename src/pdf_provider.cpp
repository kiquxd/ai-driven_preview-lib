#include "detail/internal.hpp"

#include <fpdf_progressive.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>

namespace preview::detail {
namespace {

std::mutex pdf_mutex;
std::mutex lifetime_mutex;
std::size_t engine_count = 0;

struct DocumentCloser {
  void operator()(FPDF_DOCUMENT value) const { if (value) FPDF_CloseDocument(value); }
};
struct PageCloser {
  void operator()(FPDF_PAGE value) const { if (value) FPDF_ClosePage(value); }
};
struct BitmapCloser {
  void operator()(FPDF_BITMAP value) const { if (value) FPDFBitmap_Destroy(value); }
};
using Document = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, DocumentCloser>;
using Page = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, PageCloser>;
using Bitmap = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, BitmapCloser>;

struct CacheEntry {
  std::vector<Byte> bytes;
  std::list<std::uint64_t>::iterator age;
};

class PdfAccess {
 public:
  PdfAccess(const ByteSource& source, const Request& request,
            const Probe& probe)
      : source_(source), request_(request), source_size_(probe.source_size),
        bytes_read_(probe.bytes.size()) {
    const std::size_t first_block_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(block_size_, source_size_));
    if (probe.bytes.size() == first_block_size && first_block_size != 0 &&
        first_block_size <= request_.limits.max_input_cache_bytes) {
      ages_.push_front(0);
      cache_.emplace(0, CacheEntry{probe.bytes, ages_.begin()});
      cached_bytes_ = first_block_size;
    }
  }

  static int get_block(void* parameter, unsigned long position,
                       unsigned char* output, unsigned long size) {
    auto& self = *static_cast<PdfAccess*>(parameter);
    try {
      return self.read(position, std::span<unsigned char>(output, size)) ? 1 : 0;
    } catch (...) {
      self.callback_exception_ = true;
      return 0;
    }
  }

  bool read(std::uint64_t offset, std::span<unsigned char> output) {
    if (request_.stop_token.stop_requested()) {
      fail(cancelled_error());
      return false;
    }
    if (offset > source_size_ || output.size() > source_size_ - offset) {
      fail(Error{Error::Code::io, "PDFium requested an out-of-range block"});
      return false;
    }
    std::size_t done = 0;
    while (done < output.size()) {
      const std::uint64_t absolute = offset + done;
      const std::uint64_t block_number = absolute / block_size_;
      const std::size_t within = absolute % block_size_;
      auto block = obtain(block_number);
      if (!block) return false;
      if (within >= block->size()) {
        fail(Error{Error::Code::io, "PDF cache encountered premature EOF"});
        return false;
      }
      const std::size_t count = std::min(output.size() - done,
                                        block->size() - within);
      std::memcpy(output.data() + done, block->data() + within, count);
      done += count;
    }
    return true;
  }

  const Error* error() const {
    if (error_) return &*error_;
    return callback_exception_ ? &callback_error_ : nullptr;
  }

 private:
  std::vector<Byte>* obtain(std::uint64_t block_number) {
    if (auto found = cache_.find(block_number); found != cache_.end()) {
      ages_.erase(found->second.age);
      ages_.push_front(block_number);
      found->second.age = ages_.begin();
      return &found->second.bytes;
    }
    const std::uint64_t offset = block_number * block_size_;
    const std::uint64_t remaining = source_size_ - offset;
    const std::size_t wanted = static_cast<std::size_t>(
        std::min<std::uint64_t>(block_size_, remaining));
    const std::uint64_t read_limit = std::min(
        request_.limits.max_pdf_bytes_read,
        request_.limits.max_total_bytes_read);
    if (bytes_read_ > read_limit || wanted > read_limit - bytes_read_) {
      fail(Error{Error::Code::limit_exceeded,
                 "PDF source-read budget exceeded"});
      return nullptr;
    }
    auto block = read_range(source_, offset, wanted, request_.stop_token);
    if (!block) {
      fail(block.error());
      return nullptr;
    }
    bytes_read_ += block.value().size();
    if (block.value().size() != wanted) {
      fail(Error{Error::Code::io, "PDF source ended before its reported size"});
      return nullptr;
    }
    const std::size_t cache_limit = request_.limits.max_input_cache_bytes;
    while (!ages_.empty() && cached_bytes_ + block.value().size() > cache_limit) {
      const auto victim = ages_.back();
      cached_bytes_ -= cache_.at(victim).bytes.size();
      cache_.erase(victim);
      ages_.pop_back();
    }
    if (block.value().size() > cache_limit) {
      uncached_block_ = std::move(block.value());
      return &uncached_block_;
    }
    ages_.push_front(block_number);
    auto [entry, inserted] = cache_.emplace(
        block_number, CacheEntry{std::move(block.value()), ages_.begin()});
    cached_bytes_ += entry->second.bytes.size();
    return &entry->second.bytes;
  }

  void fail(Error error) {
    if (!error_) error_ = std::move(error);
  }

  static constexpr std::uint64_t block_size_ = 64 * 1024;
  const ByteSource& source_;
  const Request& request_;
  std::uint64_t source_size_;
  std::uint64_t bytes_read_ = 0;
  std::size_t cached_bytes_ = 0;
  std::unordered_map<std::uint64_t, CacheEntry> cache_;
  std::list<std::uint64_t> ages_;
  std::vector<Byte> uncached_block_;
  std::optional<Error> error_;
  Error callback_error_{Error::Code::backend_failure,
                        "unexpected exception from PDF read callback"};
  bool callback_exception_ = false;
};

FPDF_BOOL should_pause(IFSDK_PAUSE* pause) {
  const auto* stop = static_cast<const std::stop_token*>(pause->user);
  return stop->stop_requested() ? 1 : 0;
}

std::uint32_t pdfium_color(std::uint32_t rgba) {
  const std::uint32_t red = (rgba >> 24) & 0xff;
  const std::uint32_t green = (rgba >> 16) & 0xff;
  const std::uint32_t blue = (rgba >> 8) & 0xff;
  const std::uint32_t alpha = rgba & 0xff;
  return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

}  // namespace

Result<bool> acquire_pdfium() noexcept {
  try {
    std::lock_guard lock(lifetime_mutex);
    if (engine_count++ == 0) {
      FPDF_LIBRARY_CONFIG config{};
      config.version = 2;
      FPDF_InitLibraryWithConfig(&config);
      FPDF_SetSandBoxPolicy(FPDF_POLICY_MACHINETIME_ACCESS, false);
    }
    return true;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("failed to initialize PDFium");
  }
}

void release_pdfium() noexcept {
  std::lock_guard lock(lifetime_mutex);
  if (engine_count != 0 && --engine_count == 0) {
    std::lock_guard render_lock(pdf_mutex);
    FPDF_DestroyLibrary();
  }
}

Result<Preview> make_pdf(const ByteSource& source, const Request& request,
                         const Probe& probe) noexcept {
  try {
    if (probe.source_size > request.limits.max_pdf_source_size ||
        probe.source_size > std::numeric_limits<unsigned long>::max()) {
      return Error{Error::Code::limit_exceeded,
                   "PDF source exceeds configured size limit"};
    }
    if (request.mode == Mode::visual &&
        (request.viewport.target_pixel_width == 0 ||
         request.viewport.target_pixel_height == 0)) {
      return Error{Error::Code::invalid_request,
                   "visual PDF preview requires non-zero pixel dimensions"};
    }
    std::lock_guard render_lock(pdf_mutex);
    if (request.stop_token.stop_requested()) return cancelled_error();

    PdfAccess access(source, request, probe);
    FPDF_FILEACCESS file_access{};
    file_access.m_FileLen = static_cast<unsigned long>(probe.source_size);
    file_access.m_GetBlock = &PdfAccess::get_block;
    file_access.m_Param = &access;
    const std::string password(request.pdf_password);
    Document document(FPDF_LoadCustomDocument(
        &file_access, password.empty() ? nullptr : password.c_str()));
    if (!document) {
      if (access.error()) return *access.error();
      const auto code = FPDF_GetLastError();
      if (code == FPDF_ERR_PASSWORD) {
        return Error{password.empty() ? Error::Code::password_required
                                     : Error::Code::wrong_password,
                     password.empty() ? "PDF requires a password"
                                      : "incorrect PDF password"};
      }
      return Error{code == FPDF_ERR_FORMAT ? Error::Code::invalid_data
                                           : Error::Code::backend_failure,
                   "PDFium could not load the document"};
    }
    const int page_count = FPDF_GetPageCount(document.get());
    if (page_count <= 0) {
      return Error{Error::Code::invalid_data, "PDF contains no pages"};
    }
    if (request.page_index >= static_cast<std::uint32_t>(page_count)) {
      return Error{Error::Code::invalid_request,
                   "PDF page index is out of range"};
    }
    Page page(FPDF_LoadPage(document.get(), static_cast<int>(request.page_index)));
    if (!page) {
      if (access.error()) return *access.error();
      return Error{Error::Code::invalid_data, "PDFium could not load the page"};
    }
    const float page_width = FPDF_GetPageWidthF(page.get());
    const float page_height = FPDF_GetPageHeightF(page.get());
    if (!(page_width > 0.0f) || !(page_height > 0.0f) ||
        !std::isfinite(page_width) || !std::isfinite(page_height)) {
      return Error{Error::Code::invalid_data, "PDF page has invalid dimensions"};
    }
    Preview preview;
    preview.detected_mime = "application/pdf";
    preview.detected_format = "pdf";
    preview.metadata.items.push_back({"page_count", std::to_string(page_count)});
    preview.metadata.items.push_back({"page_index", std::to_string(request.page_index)});
    preview.metadata.items.push_back({"page_width_points", std::to_string(page_width)});
    preview.metadata.items.push_back({"page_height_points", std::to_string(page_height)});
    if (request.mode == Mode::metadata ||
        request.viewport.target_pixel_width == 0 ||
        request.viewport.target_pixel_height == 0) {
      return preview;
    }
    const double scale = std::min(
        request.viewport.target_pixel_width / static_cast<double>(page_width),
        request.viewport.target_pixel_height / static_cast<double>(page_height));
    const double width_value = std::max(1.0, std::floor(page_width * scale));
    const double height_value = std::max(1.0, std::floor(page_height * scale));
    if (width_value > std::numeric_limits<int>::max() ||
        height_value > std::numeric_limits<int>::max()) {
      return Error{Error::Code::limit_exceeded,
                   "requested PDF dimensions are not representable"};
    }
    const auto width = static_cast<int>(width_value);
    const auto height = static_cast<int>(height_value);
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(width) *
                                      static_cast<std::uint64_t>(height);
    if (width > static_cast<int>(request.limits.max_pixel_dimension) ||
        height > static_cast<int>(request.limits.max_pixel_dimension) ||
        pixel_count > request.limits.max_pixels ||
        pixel_count * 4 > request.limits.max_output_bytes) {
      return Error{Error::Code::limit_exceeded,
                   "requested PDF output exceeds configured limits"};
    }
    std::vector<Byte> output(static_cast<std::size_t>(pixel_count) * 4);
    Bitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA,
                                      output.data(), width * 4));
    if (!bitmap) {
      return Error{Error::Code::backend_failure,
                   "PDFium could not create the output bitmap"};
    }
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, width, height,
                        pdfium_color(request.background_rgba));
    IFSDK_PAUSE pause{1, &should_pause,
                      const_cast<std::stop_token*>(&request.stop_token)};
    int status = FPDF_RenderPageBitmap_Start(bitmap.get(), page.get(), 0, 0,
                                              width, height, 0, 0, &pause);
    while (status == FPDF_RENDER_TOBECONTINUED &&
           !request.stop_token.stop_requested()) {
      status = FPDF_RenderPage_Continue(page.get(), &pause);
    }
    FPDF_RenderPage_Close(page.get());
    if (request.stop_token.stop_requested()) return cancelled_error();
    if (access.error()) return *access.error();
    if (status != FPDF_RENDER_DONE) {
      return Error{Error::Code::backend_failure,
                   "PDFium progressive render failed"};
    }
    if (request.pixel_format == PixelFormat::rgba8) {
      for (std::size_t index = 0; index < output.size(); index += 4) {
        std::swap(output[index], output[index + 2]);
      }
    }
    PixelPreview pixels;
    pixels.width = static_cast<std::uint32_t>(width);
    pixels.height = static_cast<std::uint32_t>(height);
    pixels.stride = static_cast<std::uint32_t>(width * 4);
    pixels.format = request.pixel_format;
    pixels.pixels = std::move(output);
    preview.content = std::move(pixels);
    return preview;
  } catch (const std::exception& error) {
    return Error{Error::Code::backend_failure, error.what()};
  } catch (...) {
    return backend_error("unexpected error while rendering PDF");
  }
}

}  // namespace preview::detail
