#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace preview {

#if defined(_WIN32)
#  if defined(PREVIEW_BUILDING_LIBRARY)
#    define PREVIEW_API __declspec(dllexport)
#  else
#    define PREVIEW_API __declspec(dllimport)
#  endif
#else
#  define PREVIEW_API __attribute__((visibility("default")))
#endif

using Byte = std::byte;

struct Error {
  enum class Code {
    io,
    invalid_request,
    invalid_data,
    unsupported,
    cancelled,
    limit_exceeded,
    backend_failure,
  };

  Code code = Code::backend_failure;
  std::string message;
};

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept {
    return std::holds_alternative<T>(storage_);
  }
  explicit operator bool() const noexcept { return has_value(); }

  // Calling value() on an error, or error() on a value, is a programmer error.
  T& value() & noexcept { return std::get<T>(storage_); }
  const T& value() const& noexcept { return std::get<T>(storage_); }
  T&& value() && noexcept { return std::get<T>(std::move(storage_)); }
  Error& error() & noexcept { return std::get<Error>(storage_); }
  const Error& error() const& noexcept { return std::get<Error>(storage_); }

 private:
  std::variant<T, Error> storage_;
};

class ByteSource {
 public:
  virtual ~ByteSource() = default;

  virtual Result<std::uint64_t> size(
      std::stop_token stop_token) const noexcept = 0;
  virtual Result<std::size_t> read_at(
      std::uint64_t offset,
      std::span<Byte> destination,
      std::stop_token stop_token) const noexcept = 0;
  virtual std::string_view name_hint() const noexcept = 0;
  virtual std::string_view mime_hint() const noexcept = 0;
};

struct Limits {
  std::uint64_t max_probe_bytes = 64 * 1024;
  std::uint64_t max_text_bytes = 2 * 1024 * 1024;
  std::uint64_t max_encoded_image_bytes = 32 * 1024 * 1024;
  std::uint64_t max_total_bytes_read = 64 * 1024 * 1024;
  std::size_t max_input_cache_bytes = 8 * 1024 * 1024;
  std::size_t max_working_bytes = 128 * 1024 * 1024;
  std::size_t max_output_bytes = 16 * 1024 * 1024;
  std::uint64_t max_pixels = 4 * 1024 * 1024;
  std::uint32_t max_pixel_dimension = 4096;
  std::uint32_t max_text_lines = 500;
  std::uint32_t max_line_bytes = 64 * 1024;
  std::uint32_t max_syntax_spans = 16 * 1024;
};

enum class PixelFormat { bgra8, rgba8 };

struct Viewport {
  std::uint32_t text_columns = 0;
  std::uint32_t text_rows = 0;
  std::uint32_t target_pixel_width = 0;
  std::uint32_t target_pixel_height = 0;
};

enum class Mode { automatic, visual, metadata, hex };

struct Request {
  Viewport viewport;
  Limits limits;
  Mode mode = Mode::automatic;
  std::uint64_t byte_offset = 0;
  PixelFormat pixel_format = PixelFormat::bgra8;
  std::string_view syntax_language_hint;
  bool syntax_highlighting = true;
  std::stop_token stop_token;
};

enum class SyntaxLanguage {
  plain_text,
  cpp,
  python,
  bash,
  json,
  cmake,
  markdown,
};

enum class SyntaxToken {
  keyword,
  type,
  function,
  string_literal,
  number,
  comment,
  preprocessor,
  property,
  heading,
  link,
  code_literal,
  operator_symbol,
};

struct StyledRange {
  std::uint32_t byte_begin = 0;
  std::uint32_t byte_end = 0;
  SyntaxToken token = SyntaxToken::keyword;
};

struct TextLine {
  std::string text;
  // Sorted, non-overlapping UTF-8 byte ranges. Gaps use the consumer's normal
  // text style; colors deliberately remain a UI concern.
  std::vector<StyledRange> styles;
};

struct TextPreview {
  std::vector<TextLine> lines;
  SyntaxLanguage syntax_language = SyntaxLanguage::plain_text;
  std::uint64_t source_begin = 0;
  std::uint64_t source_end = 0;
  std::uint64_t next_offset = 0;
  bool has_more = false;
};

struct PixelPreview {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  PixelFormat format = PixelFormat::bgra8;
  std::vector<Byte> pixels;
};

struct MetadataItem {
  std::string key;
  std::string value;
};

struct Metadata {
  std::vector<MetadataItem> items;
};

struct UnsupportedContent {
  std::string reason;
};

struct Warning {
  std::string code;
  std::string message;
};

using Content = std::variant<std::monostate, TextPreview, PixelPreview,
                             UnsupportedContent>;

struct Preview {
  std::string detected_mime;
  std::string detected_format;
  Content content;
  Metadata metadata;
  std::vector<Warning> warnings;
  bool truncated = false;
};

class Engine {
 public:
  PREVIEW_API static Result<Engine> create() noexcept;
  PREVIEW_API ~Engine();
  PREVIEW_API Engine(Engine&&) noexcept;
  PREVIEW_API Engine& operator=(Engine&&) noexcept;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  PREVIEW_API Result<Preview> make_preview(
      const ByteSource& source, const Request& request) const noexcept;

 private:
  struct Impl;
  explicit Engine(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

PREVIEW_API Result<std::unique_ptr<ByteSource>> open_local_file(
    const std::filesystem::path& path) noexcept;

}  // namespace preview
