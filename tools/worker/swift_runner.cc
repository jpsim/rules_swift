// Copyright 2019 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tools/worker/swift_runner.h"

#include <fstream>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "tools/common/bazel_substitutions.h"
#include "tools/common/file_system.h"
#include "tools/common/process.h"
#include "tools/common/temp_file.h"

namespace {

// Creates a temporary file and writes the given arguments to it, one per line.
static std::unique_ptr<TempFile> WriteResponseFile(
    const std::vector<std::string> &args) {
  std::unique_ptr<TempFile> response_file =
      TempFile::Create("swiftc_params.XXXXXX");
  std::ofstream response_file_stream(std::string(response_file->GetPath()));

  for (absl::string_view arg : args) {
    // When Clang/Swift write out a response file to communicate from driver to
    // frontend, they just quote every argument to be safe; we duplicate that
    // instead of trying to be "smarter" and only quoting when necessary.
    response_file_stream << '"';
    for (char ch : arg) {
      if (ch == '"' || ch == '\\') {
        response_file_stream << '\\';
      }
      response_file_stream << ch;
    }
    response_file_stream << "\"\n";
  }

  response_file_stream.close();
  return response_file;
}

// Unescape and unquote an argument read from a line of a response file.
static std::string Unescape(absl::string_view arg) {
  std::string result;
  size_t length = arg.size();
  for (size_t i = 0; i < length; ++i) {
    char ch = arg[i];

    // If it's a backslash, consume it and append the character that follows.
    if (ch == '\\' && i + 1 < length) {
      ++i;
      result.push_back(arg[i]);
      continue;
    }

    // If it's a quote, process everything up to the matching quote, unescaping
    // backslashed characters as needed.
    if (ch == '"' || ch == '\'') {
      char quote = ch;
      ++i;
      while (i != length && arg[i] != quote) {
        if (arg[i] == '\\' && i + 1 < length) {
          ++i;
        }
        result.push_back(arg[i]);
        ++i;
      }
      if (i == length) {
        break;
      }
      continue;
    }

    // It's a regular character.
    result.push_back(ch);
  }

  return result;
}

}  // namespace

SwiftRunner::SwiftRunner(const std::vector<std::string> &args,
                         bool force_response_file)
    : force_response_file_(force_response_file) {
  args_ = ProcessArguments(args);
}

int SwiftRunner::Run(std::ostream &stderr_stream, bool stdout_to_stderr) {
  int exit_code = RunSubProcess(args_, stderr_stream, stdout_to_stderr);
  if (exit_code != 0) {
    return exit_code;
  }

  if (!generated_header_rewriter_path_.empty()) {
#if __APPLE__
    // Skip the `xcrun` argument that's added when running on Apple platforms.
    int initial_args_to_skip = 1;
#else
    int initial_args_to_skip = 0;
#endif

    std::vector<std::string> rewriter_args;
    rewriter_args.reserve(args_.size() + 2 - initial_args_to_skip);
    rewriter_args.push_back(generated_header_rewriter_path_);
    rewriter_args.push_back("--");
    rewriter_args.insert(rewriter_args.end(),
                         args_.begin() + initial_args_to_skip, args_.end());

    exit_code = RunSubProcess(rewriter_args, stderr_stream, stdout_to_stderr);
  }

  return exit_code;
}

bool SwiftRunner::ProcessPossibleResponseFile(
    absl::string_view arg, std::function<void(absl::string_view)> consumer) {
  absl::string_view path = arg.substr(1);
  std::ifstream original_file((std::string(path)));
  // If we couldn't open it, maybe it's not a file; maybe it's just some other
  // argument that starts with "@". (Unlikely, but it's safer to check.)
  if (!original_file.good()) {
    consumer(arg);
    return false;
  }

  // If we're forcing response files, process and send the arguments from this
  // file directly to the consumer; they'll all get written to the same response
  // file at the end of processing all the arguments.
  if (force_response_file_) {
    std::string arg_from_file;
    while (std::getline(original_file, arg_from_file)) {
      // Arguments in response files might be quoted/escaped, so we need to
      // unescape them ourselves.
      ProcessArgument(Unescape(arg_from_file), consumer);
    }
    return true;
  }

  // Otherwise, open the file, process the arguments, and rewrite it if any of
  // them have changed.
  bool changed = false;
  std::string arg_from_file;
  std::vector<std::string> new_args;

  while (std::getline(original_file, arg_from_file)) {
    changed |= ProcessArgument(arg_from_file,
                               [&new_args](absl::string_view processed_arg) {
                                 new_args.push_back(std::string(processed_arg));
                               });
  }

  if (changed) {
    std::unique_ptr<TempFile> new_file = WriteResponseFile(new_args);
    consumer(absl::StrCat("@", new_file->GetPath()));
    temp_files_.push_back(std::move(new_file));
  } else {
    // If none of the arguments changed, just keep the original response file
    // argument.
    consumer(arg);
  }

  return changed;
}

bool SwiftRunner::ProcessArgument(
    absl::string_view arg, std::function<void(absl::string_view)> consumer) {
  if (arg[0] == '@') {
    return ProcessPossibleResponseFile(arg, consumer);
  }

  absl::string_view trimmed_arg = arg;
  if (absl::ConsumePrefix(&trimmed_arg, "-Xwrapped-swift=")) {
    if (trimmed_arg == "-debug-prefix-pwd-is-dot") {
      // Get the actual current working directory (the workspace root), which
      // we didn't know at analysis time.
      consumer("-debug-prefix-map");
      consumer(GetCurrentDirectory() + "=.");
      return true;
    }

    if (trimmed_arg == "-ephemeral-module-cache") {
      // Create a temporary directory to hold the module cache, which will be
      // deleted after compilation is finished.
      std::unique_ptr<TempDirectory> module_cache_dir =
          TempDirectory::Create("swift_module_cache.XXXXXX");
      consumer("-module-cache-path");
      consumer(module_cache_dir->GetPath());
      temp_directories_.push_back(std::move(module_cache_dir));
      return true;
    }

    if (absl::ConsumePrefix(&trimmed_arg, "-generated-header-rewriter=")) {
      generated_header_rewriter_path_ = std::string(trimmed_arg);
      return true;
    }

    if (absl::ConsumePrefix(&trimmed_arg, "-bazel-target-label=")) {
      target_label_ = std::string(trimmed_arg);
      return true;
    }

    // TODO(allevato): Report that an unknown wrapper arg was found and give
    // the caller a way to exit gracefully.
    return true;
  }

  // Apply any other text substitutions needed in the argument (i.e., for
  // Apple toolchains).
  //
  // Bazel doesn't quote arguments in multi-line params files, so we need to
  // ensure that our defensive quoting kicks in if an argument contains a
  // space, even if no other changes would have been made.
  std::string new_arg(arg);
  bool changed = bazel_placeholder_substitutions_.Apply(new_arg) ||
                 absl::StrContains(new_arg, ' ');
  consumer(new_arg);
  return changed;
}

std::vector<std::string> SwiftRunner::ProcessArguments(
    const std::vector<std::string> &args) {
  std::vector<std::string> new_args;
  std::vector<std::string> response_file_args;

#if __APPLE__
  // On Apple platforms, inject `/usr/bin/xcrun` in front of our command
  // invocation.
  new_args.push_back("/usr/bin/xcrun");
#endif

  // The tool is assumed to be the first argument. Push it directly.
  auto it = args.begin();
  new_args.push_back(*it++);

  // If we're forcing response files, push the remaining processed args onto a
  // different vector that we write out below. If not, push them directly onto
  // the vector being returned.
  std::vector<std::string> &args_destination =
      force_response_file_ ? response_file_args : new_args;
  while (it != args.end()) {
    ProcessArgument(*it, [&args_destination](absl::string_view arg) {
      args_destination.push_back(std::string(arg));
    });
    ++it;
  }

  if (force_response_file_) {
    // Write the processed args to the response file, and push the path to that
    // file (preceded by '@') onto the arg list being returned.
    std::unique_ptr<TempFile> new_file = WriteResponseFile(response_file_args);
    new_args.push_back(absl::StrCat("@", new_file->GetPath()));
    temp_files_.push_back(std::move(new_file));
  }

  return new_args;
}
