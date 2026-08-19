#include <preview/preview.hpp>

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

struct Options {
  std::filesystem::path path;
  preview::Mode mode = preview::Mode::automatic;
  std::uint32_t columns = 0;
  std::uint32_t rows = 0;
};

struct TerminalSize {
  std::uint32_t columns = 80;
  std::uint32_t rows = 24;
};

TerminalSize terminal_size() {
  winsize size{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
      size.ws_col != 0 && size.ws_row != 0) {
    return {size.ws_col, size.ws_row};
  }
  return {};
}

std::optional<std::uint32_t> parse_number(std::string_view value) {
  std::uint32_t result = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

void usage(std::string_view executable) {
  std::cerr
      << "Usage: " << executable << " FILE [options]\n\n"
      << "Options:\n"
      << "  --mode auto|visual|metadata|hex\n"
      << "  --width N         terminal columns used for output\n"
      << "  --height N        terminal rows used for output\n"
      << "  --help\n";
}

std::optional<Options> parse_options(int argc, char** argv) {
  if (argc < 2) {
    usage(argv[0]);
    return std::nullopt;
  }
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      usage(argv[0]);
      return std::nullopt;
    }
    if (!argument.starts_with("--")) {
      if (!options.path.empty()) {
        std::cerr << "Only one input file may be specified.\n";
        return std::nullopt;
      }
      options.path = argument;
      continue;
    }
    if (index + 1 >= argc) {
      std::cerr << "Missing value after " << argument << ".\n";
      return std::nullopt;
    }
    const std::string_view value(argv[++index]);
    if (argument == "--mode") {
      if (value == "auto") options.mode = preview::Mode::automatic;
      else if (value == "visual") options.mode = preview::Mode::visual;
      else if (value == "metadata") options.mode = preview::Mode::metadata;
      else if (value == "hex") options.mode = preview::Mode::hex;
      else {
        std::cerr << "Unknown mode: " << value << "\n";
        return std::nullopt;
      }
    } else if (argument == "--width" || argument == "--height") {
      const auto number = parse_number(value);
      if (!number) {
        std::cerr << "Invalid number for " << argument << ": " << value << "\n";
        return std::nullopt;
      }
      if (argument == "--width") options.columns = *number;
      if (argument == "--height") options.rows = *number;
    } else {
      std::cerr << "Unknown option: " << argument << "\n";
      return std::nullopt;
    }
  }
  if (options.path.empty()) {
    std::cerr << "Input file is required.\n";
    return std::nullopt;
  }
  return options;
}

std::string_view error_code(preview::Error::Code code) {
  using Code = preview::Error::Code;
  switch (code) {
    case Code::io: return "io";
    case Code::invalid_request: return "invalid_request";
    case Code::invalid_data: return "invalid_data";
    case Code::unsupported: return "unsupported";
    case Code::cancelled: return "cancelled";
    case Code::limit_exceeded: return "limit_exceeded";
    case Code::backend_failure: return "backend_failure";
  }
  return "unknown";
}

struct Rgb {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;

  friend bool operator==(Rgb, Rgb) = default;
};

Rgb pixel_at(const preview::PixelPreview& image, std::uint32_t x,
             std::uint32_t y, Rgb background) {
  if (y >= image.height) return background;
  const std::size_t offset = static_cast<std::size_t>(y) * image.stride +
                             static_cast<std::size_t>(x) * 4;
  const auto* data = image.pixels.data() + offset;
  const auto first = std::to_integer<std::uint8_t>(data[0]);
  const auto green = std::to_integer<std::uint8_t>(data[1]);
  const auto third = std::to_integer<std::uint8_t>(data[2]);
  const auto alpha = std::to_integer<std::uint8_t>(data[3]);
  const std::uint8_t red = image.format == preview::PixelFormat::rgba8
      ? first : third;
  const std::uint8_t blue = image.format == preview::PixelFormat::rgba8
      ? third : first;
  const auto blend = [alpha](std::uint8_t foreground,
                             std::uint8_t backdrop) -> std::uint8_t {
    return static_cast<std::uint8_t>(
        (static_cast<unsigned>(foreground) * alpha +
         static_cast<unsigned>(backdrop) * (255 - alpha) + 127) / 255);
  };
  return {blend(red, background.red), blend(green, background.green),
          blend(blue, background.blue)};
}

void set_foreground(Rgb color) {
  std::cout << "\x1b[38;2;" << static_cast<unsigned>(color.red) << ';'
            << static_cast<unsigned>(color.green) << ';'
            << static_cast<unsigned>(color.blue) << 'm';
}

void set_background(Rgb color) {
  std::cout << "\x1b[48;2;" << static_cast<unsigned>(color.red) << ';'
            << static_cast<unsigned>(color.green) << ';'
            << static_cast<unsigned>(color.blue) << 'm';
}

void render_pixels(const preview::PixelPreview& image) {
  constexpr Rgb background{0, 0, 0};
  Rgb previous_top{};
  Rgb previous_bottom{};
  bool have_previous = false;
  for (std::uint32_t y = 0; y < image.height; y += 2) {
    have_previous = false;
    for (std::uint32_t x = 0; x < image.width; ++x) {
      const Rgb top = pixel_at(image, x, y, background);
      const Rgb bottom = pixel_at(image, x, y + 1, background);
      if (!have_previous || top != previous_top) set_foreground(top);
      if (!have_previous || bottom != previous_bottom) set_background(bottom);
      std::cout << "▀";
      previous_top = top;
      previous_bottom = bottom;
      have_previous = true;
    }
    std::cout << "\x1b[0m\n";
  }
}

void print_metadata(const preview::Preview& result) {
  std::cout << "format: " << result.detected_format << " ("
            << result.detected_mime << ")\n";
  for (const auto& item : result.metadata.items) {
    std::cout << item.key << ": " << item.value << '\n';
  }
  for (const auto& warning : result.warnings) {
    std::cout << "warning[" << warning.code << "]: " << warning.message << '\n';
  }
  if (result.truncated) std::cout << "truncated: true\n";
}

void render(const preview::Preview& result) {
  print_metadata(result);
  if (!std::holds_alternative<std::monostate>(result.content)) {
    std::cout << '\n';
  }
  if (const auto* text = std::get_if<preview::TextPreview>(&result.content)) {
    for (const auto& line : text->lines) std::cout << line.text << '\n';
    if (text->has_more) {
      std::cout << "\n[next byte offset: " << text->next_offset << "]\n";
    }
  } else if (const auto* image =
                 std::get_if<preview::PixelPreview>(&result.content)) {
    render_pixels(*image);
  } else if (const auto* unsupported =
                 std::get_if<preview::UnsupportedContent>(&result.content)) {
    std::cout << "unsupported: " << unsupported->reason << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (!options) return argc >= 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;

  auto engine_result = preview::Engine::create();
  if (!engine_result) {
    std::cerr << "Engine initialization failed ["
              << error_code(engine_result.error().code) << "]: "
              << engine_result.error().message << '\n';
    return 1;
  }
  auto source_result = preview::open_local_file(options->path);
  if (!source_result) {
    std::cerr << "Cannot open file [" << error_code(source_result.error().code)
              << "]: " << source_result.error().message << '\n';
    return 1;
  }

  const auto detected_terminal = terminal_size();
  const std::uint32_t columns = options->columns == 0
      ? detected_terminal.columns : options->columns;
  const std::uint32_t rows = options->rows == 0
      ? (detected_terminal.rows > 4 ? detected_terminal.rows - 4 : 1)
      : options->rows;
  if (rows > std::numeric_limits<std::uint32_t>::max() / 2) {
    std::cerr << "Height is too large.\n";
    return 2;
  }

  preview::Request request;
  request.mode = options->mode;
  request.pixel_format = preview::PixelFormat::rgba8;
  request.viewport.text_columns = columns;
  request.viewport.text_rows = rows;
  request.viewport.target_pixel_width = columns;
  request.viewport.target_pixel_height = rows * 2;

  auto result = engine_result.value().make_preview(*source_result.value(), request);
  if (!result) {
    std::cerr << "Preview failed [" << error_code(result.error().code)
              << "]: " << result.error().message << '\n';
    return 1;
  }
  render(result.value());
  return 0;
}
