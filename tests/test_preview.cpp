#include <preview/preview.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class MemorySource final : public preview::ByteSource {
 public:
  explicit MemorySource(std::vector<preview::Byte> bytes,
                        std::string name = {},
                        std::size_t max_chunk = SIZE_MAX)
      : bytes_(std::move(bytes)), name_(std::move(name)),
        max_chunk_(max_chunk) {}

  preview::Result<std::uint64_t> size(
      std::stop_token stop) const noexcept override {
    if (stop.stop_requested()) {
      return preview::Error{preview::Error::Code::cancelled, "cancelled"};
    }
    return bytes_.size();
  }

  preview::Result<std::size_t> read_at(
      std::uint64_t offset, std::span<preview::Byte> destination,
      std::stop_token stop) const noexcept override {
    if (stop.stop_requested()) {
      return preview::Error{preview::Error::Code::cancelled, "cancelled"};
    }
    if (offset >= bytes_.size()) return std::size_t{0};
    const auto count = std::min<std::size_t>(
        {destination.size(), bytes_.size() - offset, max_chunk_});
    std::memcpy(destination.data(), bytes_.data() + offset, count);
    ++read_calls_;
    bytes_read_ += count;
    return count;
  }

  std::string_view name_hint() const noexcept override { return name_; }
  std::string_view mime_hint() const noexcept override { return {}; }
  std::size_t read_calls() const noexcept { return read_calls_; }
  std::size_t bytes_read() const noexcept { return bytes_read_; }

 private:
  std::vector<preview::Byte> bytes_;
  std::string name_;
  std::size_t max_chunk_;
  mutable std::size_t read_calls_ = 0;
  mutable std::size_t bytes_read_ = 0;
};

std::vector<preview::Byte> bytes(std::string_view input) {
  std::vector<preview::Byte> result(input.size());
  std::memcpy(result.data(), input.data(), input.size());
  return result;
}

std::vector<preview::Byte> file_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  const auto size = stream.tellg();
  std::vector<preview::Byte> result(static_cast<std::size_t>(size));
  stream.seekg(0);
  stream.read(reinterpret_cast<char*>(result.data()), size);
  return result;
}

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void test_text(preview::Engine& engine) {
  MemorySource source(bytes("alpha\r\nbeta\ngamma"), "sample.txt");
  preview::Request request;
  request.viewport.text_rows = 2;
  auto result = engine.make_preview(source, request);
  expect(result.has_value(), "text preview succeeds");
  if (!result) return;
  const auto* text = std::get_if<preview::TextPreview>(&result.value().content);
  expect(text != nullptr, "text preview returns TextPreview");
  if (!text) return;
  expect(text->lines.size() == 2, "text row limit is respected");
  expect(text->lines[0].text == "alpha", "CRLF is normalized");
  expect(text->lines[1].text == "beta", "second line is decoded");
  expect(text->next_offset == 12, "continuation offset is source based");
  expect(text->has_more, "continuation reports remaining data");
}

void test_hex(preview::Engine& engine) {
  std::vector<preview::Byte> input = {
      preview::Byte{0x00}, preview::Byte{0x41}, preview::Byte{0xff}};
  MemorySource source(std::move(input));
  preview::Request request;
  request.mode = preview::Mode::hex;
  request.viewport.text_rows = 1;
  auto result = engine.make_preview(source, request);
  expect(result.has_value(), "hex preview succeeds");
  if (!result) return;
  const auto* text = std::get_if<preview::TextPreview>(&result.value().content);
  expect(text && text->lines.size() == 1, "hex preview returns one row");
  if (text) {
    expect(text->lines[0].text.find("00 41 ff") != std::string::npos,
           "hex bytes are formatted");
    expect(text->lines[0].text.find(".A.") != std::string::npos,
           "hex ASCII gutter is formatted");
  }
}

void test_cancel(preview::Engine& engine) {
  MemorySource source(bytes("hello"));
  std::stop_source stop;
  stop.request_stop();
  preview::Request request;
  request.stop_token = stop.get_token();
  auto result = engine.make_preview(source, request);
  expect(!result && result.error().code == preview::Error::Code::cancelled,
         "pre-cancelled request returns cancelled");
}

void test_partial_reads_and_probe_reuse(preview::Engine& engine) {
  MemorySource partial(bytes("one\ntwo\nthree"), "partial.txt", 2);
  preview::Request request;
  auto result = engine.make_preview(partial, request);
  expect(result.has_value(), "partial positive reads are supported");
  expect(partial.bytes_read() == 13,
         "text body is reused from probe instead of being read twice");

  MemorySource bounded(bytes("abcdefghijklmnop"));
  request.limits.max_probe_bytes = 4;
  request.limits.max_total_bytes_read = 8;
  request.limits.max_text_bytes = 32;
  auto limited = engine.make_preview(bounded, request);
  expect(limited.has_value(), "bounded text preview succeeds");
  expect(bounded.bytes_read() == 8, "total source-read budget is exact");
  if (limited) {
    const auto* text = std::get_if<preview::TextPreview>(&limited.value().content);
    expect(text && text->next_offset == 8 && text->has_more,
           "bounded text returns a usable continuation offset");
  }
}

void test_encodings(preview::Engine& engine) {
  std::vector<preview::Byte> utf16 = {
      preview::Byte{0xff}, preview::Byte{0xfe}, preview::Byte{'A'},
      preview::Byte{0x00}, preview::Byte{'\r'}, preview::Byte{0x00},
      preview::Byte{'\n'}, preview::Byte{0x00}, preview::Byte{'B'},
      preview::Byte{0x00}};
  MemorySource utf16_source(std::move(utf16));
  preview::Request request;
  auto decoded = engine.make_preview(utf16_source, request);
  expect(decoded.has_value(), "UTF-16LE text decodes");
  if (decoded) {
    const auto* text = std::get_if<preview::TextPreview>(&decoded.value().content);
    expect(text && text->lines.size() == 2 && text->lines[0].text == "A" &&
               text->lines[1].text == "B",
           "UTF-16 CRLF decoding is semantic");
  }
  preview::Request odd_offset;
  odd_offset.byte_offset = 1;
  auto aligned = engine.make_preview(utf16_source, odd_offset);
  expect(aligned.has_value(), "odd UTF-16 offset is adjusted");
  if (aligned) {
    const auto* text = std::get_if<preview::TextPreview>(&aligned.value().content);
    expect(text && text->source_begin == 2,
           "UTF-16 continuation starts on a code-unit boundary");
  }

  const char invalid_data[] = {'A', static_cast<char>(0xff), 'B'};
  MemorySource invalid(bytes(std::string_view(invalid_data,
                                               sizeof(invalid_data))));
  auto replaced = engine.make_preview(invalid, request);
  expect(replaced.has_value(), "invalid UTF-8 is replaced, not rejected");
  if (replaced) {
    expect(!replaced.value().warnings.empty() &&
               replaced.value().warnings[0].code == "invalid_encoding",
           "invalid UTF-8 emits a stable warning");
  }

  const char split_data[] = {'A', static_cast<char>(0xe2),
                             static_cast<char>(0x82),
                             static_cast<char>(0xac), 'B'};
  MemorySource split(bytes(std::string_view(split_data, sizeof(split_data))));
  preview::Request split_request;
  split_request.limits.max_probe_bytes = 2;
  split_request.limits.max_total_bytes_read = 2;
  auto window = engine.make_preview(split, split_request);
  expect(window.has_value(), "UTF-8 split-boundary preview succeeds");
  if (window) {
    const auto* text = std::get_if<preview::TextPreview>(&window.value().content);
    expect(text && text->next_offset == 1,
           "continuation never points inside a UTF-8 code point");
  }
}

bool has_token(const preview::TextPreview& text, std::string_view fragment,
               preview::SyntaxToken token) {
  for (const auto& line : text.lines) {
    for (const auto& style : line.styles) {
      if (style.token == token && style.byte_begin <= style.byte_end &&
          style.byte_end <= line.text.size() &&
          std::string_view(line.text).substr(
              style.byte_begin, style.byte_end - style.byte_begin) == fragment) {
        return true;
      }
    }
  }
  return false;
}

void expect_valid_styles(const preview::TextPreview& text,
                         std::string_view language) {
  for (const auto& line : text.lines) {
    std::uint32_t previous_end = 0;
    for (const auto& style : line.styles) {
      expect(style.byte_begin < style.byte_end,
             std::string(language) + " style is non-empty");
      expect(style.byte_begin >= previous_end,
             std::string(language) + " styles are sorted and non-overlapping");
      expect(style.byte_end <= line.text.size(),
             std::string(language) + " style stays inside its UTF-8 line");
      previous_end = style.byte_end;
    }
  }
}

const preview::TextPreview* highlighted_text(
    preview::Engine& engine, std::string_view source_text,
    std::string name, preview::Request request, preview::Preview& storage) {
  MemorySource source(bytes(source_text), std::move(name));
  auto result = engine.make_preview(source, request);
  expect(result.has_value(), "highlighted text preview succeeds");
  if (!result) return nullptr;
  storage = std::move(result).value();
  return std::get_if<preview::TextPreview>(&storage.content);
}

void test_syntax_highlighting(preview::Engine& engine) {
  preview::Request request;
  preview::Preview storage;

  auto* cpp = highlighted_text(
      engine,
      "#include <string>\nint main() { const char* value = \"ok\"; return 7; } // note\n",
      "sample.cpp", request, storage);
  expect(cpp && cpp->syntax_language == preview::SyntaxLanguage::cpp,
         "C++ is detected from extension");
  if (cpp) {
    expect(has_token(*cpp, "#include <string>",
                     preview::SyntaxToken::preprocessor),
           "C++ preprocessor is highlighted");
    expect(has_token(*cpp, "int", preview::SyntaxToken::type),
           "C++ type is highlighted");
    expect(has_token(*cpp, "main", preview::SyntaxToken::function),
           "C++ function is highlighted");
    expect(has_token(*cpp, "// note", preview::SyntaxToken::comment),
           "C++ comment is highlighted");
    expect_valid_styles(*cpp, "C++");
  }

  auto* python = highlighted_text(
      engine, "def greet(name):\n    # note\n    return f\"hello {name}\"\n",
      "sample.py", request, storage);
  expect(python && python->syntax_language == preview::SyntaxLanguage::python,
         "Python is detected from extension");
  if (python) {
    expect(has_token(*python, "def", preview::SyntaxToken::keyword),
           "Python keyword is highlighted");
    expect(has_token(*python, "greet", preview::SyntaxToken::function),
           "Python definition is highlighted");
    expect(has_token(*python, "# note", preview::SyntaxToken::comment),
           "Python comment is highlighted");
    expect_valid_styles(*python, "Python");
  }

  auto* bash = highlighted_text(
      engine, "#!/usr/bin/env bash\nif test -n $HOME; then echo ok; fi\n",
      "script", request, storage);
  expect(bash && bash->syntax_language == preview::SyntaxLanguage::bash,
         "Bash is detected from shebang");
  if (bash) {
    expect(has_token(*bash, "if", preview::SyntaxToken::keyword),
           "Bash keyword is highlighted");
    expect(has_token(*bash, "$HOME", preview::SyntaxToken::property),
           "Bash variable is highlighted");
    expect_valid_styles(*bash, "Bash");
  }

  auto* json = highlighted_text(
      engine, "{\"name\": \"preview\", \"enabled\": true, \"count\": 3}\n",
      "settings.json", request, storage);
  expect(json && json->syntax_language == preview::SyntaxLanguage::json,
         "JSON is detected from extension");
  if (json) {
    expect(has_token(*json, "\"name\"", preview::SyntaxToken::property),
           "JSON property is highlighted");
    expect(has_token(*json, "true", preview::SyntaxToken::keyword),
           "JSON literal is highlighted");
    expect(has_token(*json, "3", preview::SyntaxToken::number),
           "JSON number is highlighted");
    expect_valid_styles(*json, "JSON");
  }

  auto* cmake = highlighted_text(
      engine, "cmake_minimum_required(VERSION 3.24)\nset(NAME \"preview\") # note\n",
      "CMakeLists.txt", request, storage);
  expect(cmake && cmake->syntax_language == preview::SyntaxLanguage::cmake,
         "CMake is detected from filename");
  if (cmake) {
    expect(has_token(*cmake, "cmake_minimum_required",
                     preview::SyntaxToken::function),
           "CMake command is highlighted");
    expect(has_token(*cmake, "# note", preview::SyntaxToken::comment),
           "CMake comment is highlighted");
    expect_valid_styles(*cmake, "CMake");
  }

  auto* markdown = highlighted_text(
      engine, "# Heading\nUse [preview](https://example.test) and `code`.\n",
      "README.md", request, storage);
  expect(markdown &&
             markdown->syntax_language == preview::SyntaxLanguage::markdown,
         "Markdown is detected from extension");
  if (markdown) {
    expect(has_token(*markdown, "# Heading", preview::SyntaxToken::heading),
           "Markdown heading is highlighted");
    expect(has_token(*markdown, "[preview](https://example.test)",
                     preview::SyntaxToken::link),
           "Markdown link is highlighted");
    expect(has_token(*markdown, "`code`", preview::SyntaxToken::code_literal),
           "Markdown inline code is highlighted");
    expect_valid_styles(*markdown, "Markdown");
  }

  auto* plain = highlighted_text(engine, "ordinary text\n", "notes.txt",
                                 request, storage);
  expect(plain &&
             plain->syntax_language == preview::SyntaxLanguage::plain_text &&
             plain->lines[0].styles.empty(),
         "unknown text remains unstyled");

  request.syntax_language_hint = "cpp";
  auto* hinted = highlighted_text(engine, "return 1;\n", "no-extension",
                                  request, storage);
  expect(hinted && hinted->syntax_language == preview::SyntaxLanguage::cpp &&
             has_token(*hinted, "return", preview::SyntaxToken::keyword),
         "explicit language hint overrides missing filename information");

  request = {};
  request.limits.max_syntax_spans = 1;
  auto limited_source = MemorySource(bytes("int main() { return 1; }\n"),
                                     "limited.cpp");
  auto limited = engine.make_preview(limited_source, request);
  expect(limited.has_value(), "span-limited highlighting succeeds");
  if (limited) {
    const auto* text = std::get_if<preview::TextPreview>(&limited.value().content);
    expect(text && text->lines[0].styles.size() == 1,
           "syntax span limit is respected");
    expect(!limited.value().warnings.empty() &&
               limited.value().warnings.back().code == "syntax_span_limit",
           "syntax span limit emits a stable warning");
  }

  request = {};
  request.byte_offset = 1;
  auto continuation_source = MemorySource(bytes("int value = 1;\n"),
                                          "continuation.cpp");
  auto continuation = engine.make_preview(continuation_source, request);
  expect(continuation.has_value(), "syntax continuation preview succeeds");
  if (continuation) {
    const auto* text = std::get_if<preview::TextPreview>(
        &continuation.value().content);
    expect(text && text->syntax_language == preview::SyntaxLanguage::cpp &&
               text->lines[0].styles.empty(),
           "continuation remains plain without lexical context");
    expect(!continuation.value().warnings.empty() &&
               continuation.value().warnings.back().code ==
                   "syntax_context_unavailable",
           "continuation context warning is stable");
  }
}

void test_malformed(preview::Engine& engine) {
  std::vector<preview::Byte> png = {
      preview::Byte{0x89}, preview::Byte{'P'}, preview::Byte{'N'},
      preview::Byte{'G'}, preview::Byte{0x0d}, preview::Byte{0x0a},
      preview::Byte{0x1a}, preview::Byte{0x0a},
      preview::Byte{0x00}, preview::Byte{0x00}, preview::Byte{0x00},
      preview::Byte{0x0d}, preview::Byte{'I'}, preview::Byte{'H'},
      preview::Byte{'D'}, preview::Byte{'R'},
      preview::Byte{0x00}, preview::Byte{0x00}, preview::Byte{0x00},
      preview::Byte{0x01}, preview::Byte{0x00}, preview::Byte{0x00},
      preview::Byte{0x00}, preview::Byte{0x01}};
  MemorySource source(std::move(png));
  preview::Request request;
  request.mode = preview::Mode::visual;
  request.viewport.target_pixel_width = 8;
  request.viewport.target_pixel_height = 8;
  auto result = engine.make_preview(source, request);
  expect(!result && result.error().code == preview::Error::Code::invalid_data,
         "truncated PNG fails as typed invalid data");
}

void test_webp_disabled(preview::Engine& engine) {
  const char fixture[] = {'R', 'I', 'F', 'F', 4, 0, 0, 0,
                          'W', 'E', 'B', 'P'};
  MemorySource source(bytes(std::string_view(fixture, sizeof(fixture))),
                      "image.webp");
  preview::Request request;
  auto result = engine.make_preview(source, request);
  expect(result.has_value(), "disabled WebP is a structured automatic result");
  if (result) {
    expect(result.value().detected_format == "webp", "WebP is detected");
    expect(std::holds_alternative<preview::UnsupportedContent>(
               result.value().content),
           "disabled WebP returns UnsupportedContent");
  }
}

void test_images(preview::Engine& engine) {
  for (const std::string_view name : {"sample.png", "sample.jpg", "sample.gif",
                                      "sample.bmp"}) {
    auto source = preview::open_local_file(
        std::filesystem::path(PREVIEW_TEST_CORPUS) / name);
    expect(source.has_value(), std::string("open image fixture: ") +
                                   std::string(name));
    if (!source) continue;
    preview::Request metadata_request;
    metadata_request.mode = preview::Mode::metadata;
    auto metadata = engine.make_preview(*source.value(), metadata_request);
    expect(metadata.has_value(), std::string("image metadata: ") +
                                     std::string(name));
    if (metadata) {
      expect(std::holds_alternative<std::monostate>(metadata.value().content),
             "metadata mode omits image pixels");
    }
    preview::Request render_request;
    render_request.mode = preview::Mode::visual;
    render_request.viewport.target_pixel_width = 12;
    render_request.viewport.target_pixel_height = 12;
    render_request.pixel_format = preview::PixelFormat::rgba8;
    auto rendered = engine.make_preview(*source.value(), render_request);
    expect(rendered.has_value(), std::string("image render: ") +
                                     std::string(name));
    if (!rendered) continue;
    const auto* pixels = std::get_if<preview::PixelPreview>(
        &rendered.value().content);
    expect(pixels != nullptr, "image render returns pixels");
    if (pixels) {
      expect(pixels->width == 3 && pixels->height == 2,
             "images are not enlarged beyond source dimensions");
      expect(pixels->pixels.size() == 24, "image output size is exact");
      expect(pixels->format == preview::PixelFormat::rgba8,
             "requested pixel format is reported");
    }
  }
}

void test_jpeg_orientation(preview::Engine& engine) {
  auto jpeg = file_bytes(
      std::filesystem::path(PREVIEW_TEST_CORPUS) / "sample.jpg");
  const std::vector<preview::Byte> exif = {
      preview::Byte{0xff}, preview::Byte{0xe1}, preview::Byte{0x00},
      preview::Byte{0x22}, preview::Byte{'E'}, preview::Byte{'x'},
      preview::Byte{'i'}, preview::Byte{'f'}, preview::Byte{0x00},
      preview::Byte{0x00}, preview::Byte{'I'}, preview::Byte{'I'},
      preview::Byte{0x2a}, preview::Byte{0x00}, preview::Byte{0x08},
      preview::Byte{0x00}, preview::Byte{0x00}, preview::Byte{0x00},
      preview::Byte{0x01}, preview::Byte{0x00}, preview::Byte{0x12},
      preview::Byte{0x01}, preview::Byte{0x03}, preview::Byte{0x00},
      preview::Byte{0x01}, preview::Byte{0x00}, preview::Byte{0x00},
      preview::Byte{0x00}, preview::Byte{0x06}, preview::Byte{0x00},
      preview::Byte{0x00}, preview::Byte{0x00}, preview::Byte{0x00},
      preview::Byte{0x00}, preview::Byte{0x00}, preview::Byte{0x00}};
  jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
  MemorySource source(std::move(jpeg), "oriented.jpg");
  preview::Request request;
  request.mode = preview::Mode::visual;
  request.viewport.target_pixel_width = 12;
  request.viewport.target_pixel_height = 12;
  auto rendered = engine.make_preview(source, request);
  expect(rendered.has_value(), "EXIF-oriented JPEG renders");
  if (rendered) {
    const auto* pixels = std::get_if<preview::PixelPreview>(
        &rendered.value().content);
    expect(pixels && pixels->width == 2 && pixels->height == 3,
           "EXIF orientation 6 rotates dimensions clockwise");
  }
}

void test_pdf_external_only(preview::Engine& engine) {
  MemorySource source(bytes("%PDF-1.7\nminimal probe data"), "document.pdf");
  preview::Request request;
  request.mode = preview::Mode::visual;
  auto result = engine.make_preview(source, request);
  expect(result.has_value(), "PDF detection returns a structured preview");
  if (result) {
    expect(result.value().detected_format == "pdf", "PDF is detected");
    expect(result.value().detected_mime == "application/pdf",
           "PDF MIME is reported");
    expect(std::holds_alternative<preview::UnsupportedContent>(
               result.value().content),
           "PDF rendering is delegated to the application");
  }
  expect(source.read_calls() == 1, "PDF detection only performs the bounded probe");
}

void test_concurrent_engine(preview::Engine& engine) {
  std::atomic<int> errors{0};
  std::vector<std::jthread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < 25; ++iteration) {
        MemorySource source(bytes("alpha\nbeta\n"));
        preview::Request request;
        auto result = engine.make_preview(source, request);
        if (!result ||
            !std::holds_alternative<preview::TextPreview>(
                result.value().content)) {
          ++errors;
        }
      }
    });
  }
  workers.clear();
  expect(errors.load() == 0,
         "one Engine supports concurrent calls with distinct sources");
}

void test_deterministic_mutation_corpus(preview::Engine& engine) {
  for (const std::string_view name : {"sample.png", "sample.jpg", "sample.gif",
                                      "sample.bmp"}) {
    const auto original = file_bytes(
        std::filesystem::path(PREVIEW_TEST_CORPUS) / name);
    const std::vector<std::size_t> cuts = {
        0, 1, std::min<std::size_t>(2, original.size()),
        std::min<std::size_t>(8, original.size()), original.size() / 2,
        original.empty() ? 0 : original.size() - 1};
    for (const auto cut : cuts) {
      MemorySource source(std::vector<preview::Byte>(
          original.begin(), original.begin() + static_cast<std::ptrdiff_t>(cut)),
          std::string(name));
      preview::Request request;
      request.viewport.target_pixel_width = 32;
      request.viewport.target_pixel_height = 32;
      request.limits.max_working_bytes = 2 * 1024 * 1024;
      (void)engine.make_preview(source, request);
    }
    const std::size_t mutations = std::min<std::size_t>(16, original.size());
    for (std::size_t index = 0; index < mutations; ++index) {
      auto mutated = original;
      const std::size_t position = index * original.size() / mutations;
      mutated[position] ^= preview::Byte{0x5a};
      MemorySource source(std::move(mutated), std::string(name));
      preview::Request request;
      request.viewport.target_pixel_width = 32;
      request.viewport.target_pixel_height = 32;
      request.limits.max_working_bytes = 2 * 1024 * 1024;
      (void)engine.make_preview(source, request);
    }
  }
  expect(true, "deterministic malformed corpus completed without a crash");
}

}  // namespace

int main() {
  auto engine_result = preview::Engine::create();
  if (!engine_result) {
    std::cerr << "Engine initialization failed: "
              << engine_result.error().message << '\n';
    return 1;
  }
  auto engine = std::move(engine_result).value();
  test_text(engine);
  test_hex(engine);
  test_cancel(engine);
  test_partial_reads_and_probe_reuse(engine);
  test_encodings(engine);
  test_syntax_highlighting(engine);
  test_malformed(engine);
  test_webp_disabled(engine);
  test_images(engine);
  test_jpeg_orientation(engine);
  test_pdf_external_only(engine);
  test_concurrent_engine(engine);
  test_deterministic_mutation_corpus(engine);
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "all tests passed\n";
  return 0;
}
