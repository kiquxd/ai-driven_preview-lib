#include "detail/internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace preview::detail {
namespace {

using Language = SyntaxLanguage;
using Token = SyntaxToken;

struct SpanBudget {
  std::uint32_t remaining;
  bool exhausted = false;
};

bool ascii_identifier_start(char value) {
  const auto ch = static_cast<unsigned char>(value);
  return std::isalpha(ch) != 0 || value == '_';
}

bool ascii_identifier(char value) {
  const auto ch = static_cast<unsigned char>(value);
  return std::isalnum(ch) != 0 || value == '_';
}

bool ascii_digit(char value) {
  return std::isdigit(static_cast<unsigned char>(value)) != 0;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });
  return result;
}

template <std::size_t Size>
bool contains(const std::array<std::string_view, Size>& values,
              std::string_view candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

void add_span(TextLine& line, std::size_t begin, std::size_t end, Token token,
              SpanBudget& budget) {
  if (begin >= end || begin > UINT32_MAX || end > UINT32_MAX) return;
  if (budget.remaining == 0) {
    budget.exhausted = true;
    return;
  }
  if (!line.styles.empty() && begin < line.styles.back().byte_end) return;
  line.styles.push_back({static_cast<std::uint32_t>(begin),
                         static_cast<std::uint32_t>(end), token});
  --budget.remaining;
}

std::size_t skip_space(std::string_view line, std::size_t position) {
  while (position < line.size() &&
         std::isspace(static_cast<unsigned char>(line[position])) != 0) {
    ++position;
  }
  return position;
}

std::size_t scan_number(std::string_view line, std::size_t position) {
  std::size_t end = position;
  while (end < line.size()) {
    const char ch = line[end];
    if (!ascii_identifier(ch) && ch != '.' && ch != '+' && ch != '-') break;
    ++end;
  }
  return end;
}

std::size_t scan_quoted(std::string_view line, std::size_t position,
                        char quote) {
  std::size_t end = position + 1;
  bool escaped = false;
  while (end < line.size()) {
    const char ch = line[end++];
    if (escaped) {
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == quote) {
      break;
    }
  }
  return end;
}

std::string_view basename(std::string_view name) {
  const auto slash = name.find_last_of("/\\");
  return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

std::string_view extension(std::string_view name) {
  const auto base = basename(name);
  const auto dot = base.find_last_of('.');
  return dot == std::string_view::npos ? std::string_view{} : base.substr(dot);
}

Language language_from_value(std::string_view raw) {
  const std::string value = lowercase(raw);
  if (value == "c" || value == "cc" || value == "cpp" || value == "c++" ||
      value == "h" || value == "hpp" || value == "text/x-c" ||
      value == "text/x-c++") {
    return Language::cpp;
  }
  if (value == "py" || value == "python" || value == "text/x-python") {
    return Language::python;
  }
  if (value == "sh" || value == "shell" || value == "bash" ||
      value == "zsh" || value == "application/x-sh" ||
      value == "text/x-shellscript") {
    return Language::bash;
  }
  if (value == "json" || value == "application/json") return Language::json;
  if (value == "cmake" || value == "text/x-cmake") return Language::cmake;
  if (value == "md" || value == "markdown" || value == "text/markdown") {
    return Language::markdown;
  }
  return Language::plain_text;
}

Language detect_language(std::string_view name, std::string_view mime,
                         std::string_view hint, const TextPreview& text) {
  if (!hint.empty()) {
    const auto explicit_language = language_from_value(hint);
    if (explicit_language != Language::plain_text) return explicit_language;
  }
  const std::string base = lowercase(basename(name));
  const std::string ext = lowercase(extension(base));
  static constexpr auto cpp_extensions = std::to_array<std::string_view>({
      std::string_view{".c"},   ".h",   ".cc",  ".hh",  ".cpp", ".hpp",
      ".cxx",                  ".hxx", ".ipp", ".tpp"});
  if (contains(cpp_extensions, ext)) return Language::cpp;
  if (ext == ".py" || ext == ".pyw") return Language::python;
  if (ext == ".sh" || ext == ".bash" || ext == ".zsh" ||
      base == ".bashrc" || base == ".zshrc" || base == ".profile") {
    return Language::bash;
  }
  if (ext == ".json") return Language::json;
  if (ext == ".cmake" || base == "cmakelists.txt") return Language::cmake;
  if (ext == ".md" || ext == ".markdown" || ext == ".mdown") {
    return Language::markdown;
  }
  const auto mime_language = language_from_value(mime);
  if (mime_language != Language::plain_text) return mime_language;
  if (!text.lines.empty() && text.lines.front().text.starts_with("#!")) {
    const std::string first = lowercase(text.lines.front().text);
    if (first.find("python") != std::string::npos) return Language::python;
    if (first.find("bash") != std::string::npos ||
        first.find("/sh") != std::string::npos ||
        first.find("zsh") != std::string::npos) {
      return Language::bash;
    }
  }
  return Language::plain_text;
}

void highlight_cpp(TextPreview& text, SpanBudget& budget) {
  static constexpr auto keywords = std::to_array<std::string_view>({
      std::string_view{"alignas"}, "alignof", "asm", "auto", "break",
      "case", "catch", "class", "concept", "const", "consteval",
      "constexpr", "constinit", "const_cast", "continue", "co_await",
      "co_return", "co_yield", "decltype", "default", "delete", "do",
      "dynamic_cast", "else", "enum", "explicit", "export", "extern",
      "for", "friend", "goto", "if", "inline", "mutable", "namespace",
      "new", "noexcept", "operator", "private", "protected", "public",
      "register", "reinterpret_cast", "requires", "return", "sizeof",
      "static", "static_assert", "static_cast", "struct", "switch",
      "template", "this", "thread_local", "throw", "try", "typedef",
      "typeid", "typename", "union", "using", "virtual", "volatile",
      "while"});
  static constexpr auto types = std::to_array<std::string_view>({
      std::string_view{"bool"}, "char", "char8_t", "char16_t", "char32_t",
      "double", "float", "int", "long", "short", "signed", "unsigned",
      "void", "wchar_t", "size_t", "string", "string_view"});
  bool block_comment = false;
  for (auto& output : text.lines) {
    const std::string_view line = output.text;
    std::size_t position = 0;
    while (position < line.size()) {
      if (block_comment) {
        const auto close = line.find("*/", position);
        const auto end = close == std::string_view::npos ? line.size() : close + 2;
        add_span(output, position, end, Token::comment, budget);
        position = end;
        if (close == std::string_view::npos) break;
        block_comment = false;
        continue;
      }
      if (line.compare(position, 2, "//") == 0) {
        add_span(output, position, line.size(), Token::comment, budget);
        break;
      }
      if (line.compare(position, 2, "/*") == 0) {
        const auto close = line.find("*/", position + 2);
        const auto end = close == std::string_view::npos ? line.size() : close + 2;
        add_span(output, position, end, Token::comment, budget);
        position = end;
        block_comment = close == std::string_view::npos;
        continue;
      }
      if (line[position] == '#' && skip_space(line, 0) == position) {
        add_span(output, position, line.size(), Token::preprocessor, budget);
        break;
      }
      if (line[position] == '"' || line[position] == '\'') {
        const auto end = scan_quoted(line, position, line[position]);
        add_span(output, position, end, Token::string_literal, budget);
        position = end;
        continue;
      }
      if (ascii_digit(line[position])) {
        const auto end = scan_number(line, position);
        add_span(output, position, end, Token::number, budget);
        position = end;
        continue;
      }
      if (ascii_identifier_start(line[position])) {
        std::size_t end = position + 1;
        while (end < line.size() && ascii_identifier(line[end])) ++end;
        const auto word = line.substr(position, end - position);
        if (contains(keywords, word)) {
          add_span(output, position, end, Token::keyword, budget);
        } else if (contains(types, word)) {
          add_span(output, position, end, Token::type, budget);
        } else if (skip_space(line, end) < line.size() &&
                   line[skip_space(line, end)] == '(') {
          add_span(output, position, end, Token::function, budget);
        }
        position = end;
        continue;
      }
      ++position;
    }
  }
}

bool python_string_prefix(std::string_view word) {
  const std::string value = lowercase(word);
  return value == "r" || value == "u" || value == "b" || value == "f" ||
         value == "br" || value == "rb" || value == "fr" || value == "rf";
}

void highlight_python(TextPreview& text, SpanBudget& budget) {
  static constexpr auto keywords = std::to_array<std::string_view>({
      std::string_view{"and"}, "as", "assert", "async", "await", "break",
      "case", "class", "continue", "def", "del", "elif", "else", "except",
      "finally", "for", "from", "global", "if", "import", "in", "is",
      "lambda", "match", "nonlocal", "not", "or", "pass", "raise",
      "return", "try", "while", "with", "yield"});
  static constexpr auto types = std::to_array<std::string_view>({
      std::string_view{"bool"}, "bytes", "dict", "float", "frozenset",
      "int", "list", "object", "set", "str", "tuple", "type"});
  char triple_quote = 0;
  for (auto& output : text.lines) {
    const std::string_view line = output.text;
    std::size_t position = 0;
    bool expect_function = false;
    while (position < line.size()) {
      if (triple_quote != 0) {
        const std::string marker(3, triple_quote);
        const auto close = line.find(marker, position);
        const auto end = close == std::string_view::npos ? line.size() : close + 3;
        add_span(output, position, end, Token::string_literal, budget);
        position = end;
        if (close == std::string_view::npos) break;
        triple_quote = 0;
        continue;
      }
      if (line[position] == '#') {
        add_span(output, position, line.size(), Token::comment, budget);
        break;
      }
      std::size_t string_begin = position;
      if (ascii_identifier_start(line[position])) {
        std::size_t prefix_end = position + 1;
        while (prefix_end < line.size() && ascii_identifier(line[prefix_end])) {
          ++prefix_end;
        }
        if (prefix_end < line.size() && python_string_prefix(
                line.substr(position, prefix_end - position)) &&
            (line[prefix_end] == '"' || line[prefix_end] == '\'')) {
          position = prefix_end;
        }
      }
      if (line[position] == '"' || line[position] == '\'') {
        const char quote = line[position];
        if (line.substr(position, 3) == std::string(3, quote)) {
          const auto close = line.find(std::string(3, quote), position + 3);
          const auto end = close == std::string_view::npos ? line.size() : close + 3;
          add_span(output, string_begin, end, Token::string_literal, budget);
          position = end;
          triple_quote = close == std::string_view::npos ? quote : 0;
        } else {
          const auto end = scan_quoted(line, position, quote);
          add_span(output, string_begin, end, Token::string_literal, budget);
          position = end;
        }
        continue;
      }
      position = string_begin;
      if (ascii_digit(line[position])) {
        const auto end = scan_number(line, position);
        add_span(output, position, end, Token::number, budget);
        position = end;
        continue;
      }
      if (ascii_identifier_start(line[position])) {
        std::size_t end = position + 1;
        while (end < line.size() && ascii_identifier(line[end])) ++end;
        const auto word = line.substr(position, end - position);
        if (expect_function) {
          add_span(output, position, end, Token::function, budget);
          expect_function = false;
        } else if (contains(keywords, word)) {
          add_span(output, position, end, Token::keyword, budget);
          expect_function = word == "def" || word == "class";
        } else if (contains(types, word)) {
          add_span(output, position, end, Token::type, budget);
        } else if (word == "True" || word == "False" || word == "None") {
          add_span(output, position, end, Token::keyword, budget);
        } else if (skip_space(line, end) < line.size() &&
                   line[skip_space(line, end)] == '(') {
          add_span(output, position, end, Token::function, budget);
        }
        position = end;
        continue;
      }
      ++position;
    }
  }
}

void highlight_bash(TextPreview& text, SpanBudget& budget) {
  static constexpr auto keywords = std::to_array<std::string_view>({
      std::string_view{"case"}, "coproc", "do", "done", "elif", "else",
      "esac", "fi", "for", "function", "if", "in", "select", "then",
      "time", "until", "while"});
  for (auto& output : text.lines) {
    const std::string_view line = output.text;
    std::size_t position = 0;
    bool command_boundary = true;
    bool expect_function = false;
    while (position < line.size()) {
      if (line[position] == '#' &&
          (position == 0 || std::isspace(
              static_cast<unsigned char>(line[position - 1])) != 0)) {
        add_span(output, position, line.size(), Token::comment, budget);
        break;
      }
      if (line[position] == '"' || line[position] == '\'') {
        const auto end = scan_quoted(line, position, line[position]);
        add_span(output, position, end, Token::string_literal, budget);
        position = end;
        command_boundary = false;
        continue;
      }
      if (line[position] == '$') {
        std::size_t end = position + 1;
        if (end < line.size() && line[end] == '{') {
          const auto close = line.find('}', end + 1);
          end = close == std::string_view::npos ? line.size() : close + 1;
        } else {
          while (end < line.size() && ascii_identifier(line[end])) ++end;
        }
        add_span(output, position, end, Token::property, budget);
        position = end;
        continue;
      }
      if (ascii_digit(line[position])) {
        const auto end = scan_number(line, position);
        add_span(output, position, end, Token::number, budget);
        position = end;
        continue;
      }
      if (ascii_identifier_start(line[position])) {
        std::size_t end = position + 1;
        while (end < line.size() &&
               (ascii_identifier(line[end]) || line[end] == '-')) ++end;
        const auto word = line.substr(position, end - position);
        if (expect_function || (skip_space(line, end) + 1 < line.size() &&
            line.substr(skip_space(line, end), 2) == "()")) {
          add_span(output, position, end, Token::function, budget);
          expect_function = false;
        } else if (contains(keywords, word)) {
          add_span(output, position, end, Token::keyword, budget);
          expect_function = word == "function";
        } else if (command_boundary) {
          add_span(output, position, end, Token::function, budget);
        }
        position = end;
        command_boundary = false;
        continue;
      }
      if (line[position] == ';' || line[position] == '|' ||
          line[position] == '&') {
        command_boundary = true;
      }
      ++position;
    }
  }
}

void highlight_json(TextPreview& text, SpanBudget& budget) {
  for (auto& output : text.lines) {
    const std::string_view line = output.text;
    std::size_t position = 0;
    while (position < line.size()) {
      if (line[position] == '"') {
        const auto end = scan_quoted(line, position, '"');
        const auto next = skip_space(line, end);
        add_span(output, position, end,
                 next < line.size() && line[next] == ':' ? Token::property
                                                         : Token::string_literal,
                 budget);
        position = end;
        continue;
      }
      if (line[position] == '-' || ascii_digit(line[position])) {
        const auto end = scan_number(line, position);
        add_span(output, position, end, Token::number, budget);
        position = end;
        continue;
      }
      if (ascii_identifier_start(line[position])) {
        std::size_t end = position + 1;
        while (end < line.size() && ascii_identifier(line[end])) ++end;
        const auto word = line.substr(position, end - position);
        if (word == "true" || word == "false" || word == "null") {
          add_span(output, position, end, Token::keyword, budget);
        }
        position = end;
        continue;
      }
      if (std::string_view("{}[]:,").find(line[position]) !=
          std::string_view::npos) {
        add_span(output, position, position + 1, Token::operator_symbol, budget);
      }
      ++position;
    }
  }
}

void highlight_cmake(TextPreview& text, SpanBudget& budget) {
  static constexpr auto keywords = std::to_array<std::string_view>({
      std::string_view{"and"}, "else", "elseif", "endforeach", "endfunction",
      "endif", "endmacro", "endwhile", "false", "foreach", "function", "if",
      "macro", "not", "off", "on", "or", "true", "while"});
  bool bracket_comment = false;
  for (auto& output : text.lines) {
    const std::string_view line = output.text;
    std::size_t position = 0;
    while (position < line.size()) {
      if (bracket_comment) {
        const auto close = line.find("]]", position);
        const auto end = close == std::string_view::npos ? line.size() : close + 2;
        add_span(output, position, end, Token::comment, budget);
        position = end;
        if (close == std::string_view::npos) break;
        bracket_comment = false;
        continue;
      }
      if (line.compare(position, 3, "#[[") == 0) {
        const auto close = line.find("]]", position + 3);
        const auto end = close == std::string_view::npos ? line.size() : close + 2;
        add_span(output, position, end, Token::comment, budget);
        position = end;
        bracket_comment = close == std::string_view::npos;
        continue;
      }
      if (line[position] == '#') {
        add_span(output, position, line.size(), Token::comment, budget);
        break;
      }
      if (line[position] == '"') {
        const auto end = scan_quoted(line, position, '"');
        add_span(output, position, end, Token::string_literal, budget);
        position = end;
        continue;
      }
      if (line.compare(position, 2, "${") == 0) {
        const auto close = line.find('}', position + 2);
        const auto end = close == std::string_view::npos ? line.size() : close + 1;
        add_span(output, position, end, Token::property, budget);
        position = end;
        continue;
      }
      if (ascii_digit(line[position])) {
        const auto end = scan_number(line, position);
        add_span(output, position, end, Token::number, budget);
        position = end;
        continue;
      }
      if (ascii_identifier_start(line[position])) {
        std::size_t end = position + 1;
        while (end < line.size() &&
               (ascii_identifier(line[end]) || line[end] == '-')) ++end;
        const auto word = lowercase(line.substr(position, end - position));
        if (contains(keywords, std::string_view(word))) {
          add_span(output, position, end, Token::keyword, budget);
        } else if (skip_space(line, end) < line.size() &&
                   line[skip_space(line, end)] == '(') {
          add_span(output, position, end, Token::function, budget);
        }
        position = end;
        continue;
      }
      ++position;
    }
  }
}

void highlight_markdown(TextPreview& text, SpanBudget& budget) {
  bool fenced = false;
  char fence_char = 0;
  for (auto& output : text.lines) {
    const std::string_view line = output.text;
    const auto first = skip_space(line, 0);
    if (first < line.size() && (line[first] == '`' || line[first] == '~')) {
      std::size_t count = 0;
      while (first + count < line.size() && line[first + count] == line[first]) {
        ++count;
      }
      if (count >= 3 && (!fenced || line[first] == fence_char)) {
        add_span(output, first, line.size(), Token::code_literal, budget);
        fenced = !fenced;
        fence_char = fenced ? line[first] : 0;
        continue;
      }
    }
    if (fenced) {
      add_span(output, 0, line.size(), Token::code_literal, budget);
      continue;
    }
    if (first < line.size() && line[first] == '#') {
      std::size_t end = first;
      while (end < line.size() && line[end] == '#') ++end;
      if (end < line.size() && line[end] == ' ') {
        add_span(output, first, line.size(), Token::heading, budget);
        continue;
      }
    }
    std::size_t position = 0;
    while (position < line.size()) {
      if (line.compare(position, 4, "<!--") == 0) {
        const auto close = line.find("-->", position + 4);
        const auto end = close == std::string_view::npos ? line.size() : close + 3;
        add_span(output, position, end, Token::comment, budget);
        position = end;
        continue;
      }
      if (line[position] == '`') {
        const auto close = line.find('`', position + 1);
        const auto end = close == std::string_view::npos ? line.size() : close + 1;
        add_span(output, position, end, Token::code_literal, budget);
        position = end;
        continue;
      }
      if (line[position] == '[') {
        const auto label_end = line.find(']', position + 1);
        if (label_end != std::string_view::npos && label_end + 1 < line.size() &&
            line[label_end + 1] == '(') {
          const auto close = line.find(')', label_end + 2);
          const auto end = close == std::string_view::npos ? line.size() : close + 1;
          add_span(output, position, end, Token::link, budget);
          position = end;
          continue;
        }
      }
      ++position;
    }
  }
}

}  // namespace

HighlightResult highlight_text(TextPreview& text, std::string_view name,
                               std::string_view mime, std::string_view hint,
                               std::uint32_t max_spans, bool apply_styles) {
  HighlightResult result;
  result.language = detect_language(name, mime, hint, text);
  text.syntax_language = result.language;
  if (!apply_styles || result.language == Language::plain_text) return result;

  SpanBudget budget{max_spans};
  switch (result.language) {
    case Language::cpp: highlight_cpp(text, budget); break;
    case Language::python: highlight_python(text, budget); break;
    case Language::bash: highlight_bash(text, budget); break;
    case Language::json: highlight_json(text, budget); break;
    case Language::cmake: highlight_cmake(text, budget); break;
    case Language::markdown: highlight_markdown(text, budget); break;
    case Language::plain_text: break;
  }
  result.span_limit_reached = budget.exhausted;
  return result;
}

}  // namespace preview::detail
