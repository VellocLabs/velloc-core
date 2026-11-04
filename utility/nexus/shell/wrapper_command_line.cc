// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/wrapper_command_line.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace nexus {

namespace {

// What cmd.exe changes inside a quoted argument: `%` and `!` expand, `^` is
// doubled when a wrapper passes `%*` through `call`, `"` ends the quote, and
// a control character ends or splits the line.
const char* CmdChangeIn(std::string_view value) {
  for (const char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) {
      return "a control character";
    }
    switch (c) {
      case '%':
        return "a percent sign";
      case '!':
        return "an exclamation mark";
      case '^':
        return "a caret";
      case '"':
        return "a double quote";
      default:
        break;
    }
  }
  return nullptr;
}

// Left bare only when cmd.exe has nothing to parse in it.
bool NeedsQuotes(std::string_view arg) {
  if (arg.empty()) {
    return true;
  }
  for (const char c : arg) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '-' && c != '_' && c != '.' &&
        c != '/' && c != '\\' && c != ':' && c != '@' && c != '+') {
      return true;
    }
  }
  return false;
}

// The argument in double quotes. A run of backslashes before the closing
// quote is doubled so the program's own argv parser does not read `\"` as
// an escaped quote.
std::wstring Quoted(const std::wstring& value) {
  size_t trailing = 0;
  while (trailing < value.size() &&
         value[value.size() - 1 - trailing] == L'\\') {
    ++trailing;
  }
  return L"\"" + value + std::wstring(trailing, L'\\') + L"\"";
}

}  // namespace

bool IsBatchWrapper(const base::FilePath& exe) {
  base::FilePath::StringType name = exe.BaseName().value();
  while (!name.empty() && (name.back() == FILE_PATH_LITERAL('.') ||
                           name.back() == FILE_PATH_LITERAL(' '))) {
    name.pop_back();
  }
  const base::FilePath trimmed(name);
  return trimmed.MatchesFinalExtension(FILE_PATH_LITERAL(".cmd")) ||
         trimmed.MatchesFinalExtension(FILE_PATH_LITERAL(".bat"));
}

std::optional<WrapperArgProblem> FindWrapperArgProblem(
    const base::FilePath& wrapper,
    const std::vector<std::string>& args) {
  if (const char* what = CmdChangeIn(wrapper.AsUTF8Unsafe())) {
    return WrapperArgProblem{std::nullopt, what};
  }
  for (size_t i = 0; i < args.size(); ++i) {
    if (const char* what = CmdChangeIn(args[i])) {
      return WrapperArgProblem{i, what};
    }
  }
  return std::nullopt;
}

std::wstring BuildWrapperCommandLine(const base::FilePath& cmd_exe,
                                     const base::FilePath& wrapper,
                                     const std::vector<std::string>& args) {
  std::wstring inner = Quoted(wrapper.value());
  for (const std::string& arg : args) {
    const std::wstring wide = base::UTF8ToWide(arg);
    inner += L' ';
    inner += NeedsQuotes(arg) ? Quoted(wide) : wide;
  }
  return Quoted(cmd_exe.value()) + L" /D /V:OFF /S /C \"" + inner + L"\"";
}

}  // namespace nexus
