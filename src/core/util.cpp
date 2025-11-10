// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/util.h"
#include "core/packman.h"
#include <openssl/md5.h>

namespace fs = std::filesystem;

// Read file content, normalize \r\n → \n, compute MD5
std::string computeFileMD5(const std::string &fname) {
  std::ifstream file(fname, std::ios::binary);
  if (!file.is_open()) {
    return std::string(32, '0'); // Return 32-char zero hash if fail
  }

  std::vector<char> data;
  char buffer[4096];

  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    data.insert(data.end(), buffer, buffer + file.gcount());
  }

  // Normalize line endings: \r\n → \n
  std::vector<char> normalized;
  normalized.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] == '\r' && i + 1 < data.size() && data[i+1] == '\n') {
      continue; // skip \r, keep \n
    }
    normalized.push_back(data[i]);
  }

  unsigned char digest[MD5_DIGEST_LENGTH];
  MD5(reinterpret_cast<const unsigned char*>(normalized.data()), normalized.size(), digest);

  return toHex({ (char*)digest, MD5_DIGEST_LENGTH });
}

// Write file MD5: "filename=md5;"
void writeFileMD5(std::ostringstream &dest, const std::string &fname) {
  std::string hash = computeFileMD5(fname);
  dest << fname << '=' << hash << ';';
}

// Recursively write all files matching regex (sorted by name), dirs first in name order
void writeDirMD5(std::ostringstream &dest, const std::string &dir, const std::regex &filter_re) {
  fs::path path(dir);

  if (!fs::exists(path) || !fs::is_directory(path)) {
    return;
  }

  // Collect entries to sort by filename
  std::vector<fs::directory_entry> entries;
  for (const auto& entry : fs::directory_iterator(path)) {
    entries.push_back(entry);
  }

  // Sort by filename (lexicographical, like Qt QDir::Name)
  std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
    return a.path().filename() < b.path().filename();
  });

  for (const auto& entry : entries) {
    if (entry.is_directory()) {
      writeDirMD5(dest, entry.path().string(), filter_re);
    } else if (entry.is_regular_file()) {
      std::string filename = entry.path().filename().string();
      if (std::regex_match(filename, filter_re)) {
        writeFileMD5(dest, entry.path().string());
      }
    }
  }
}

// Handle packages: scan top-level dirs under "packages", skip .disabled, disabled packs, and built-ins
void writePkgsMD5(std::ostringstream &dest, const std::string &base_dir, const std::string &filter_pattern) {
  fs::path path(base_dir);
  if (!fs::exists(path) || !fs::is_directory(path)) {
    return;
  }

  auto disabled = PackMan::instance().getDisabledPacks();
  const std::set<std::string> builtinPkgs = {
    "standard", "standard_cards", "maneuvering", "test"
  };

  std::vector<fs::directory_entry> entries;
  for (const auto& entry : fs::directory_iterator(path)) {
    if (entry.is_directory()) {
      entries.push_back(entry);
    }
  }

  // Sort by name
  std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
    return a.path().filename() < b.path().filename();
  });

  for (const auto& entry : entries) {
    std::string dirname = entry.path().filename().string();

    // Skip .disabled directories
    if (dirname.ends_with(".disabled")) continue;
    if (std::ranges::find(disabled, dirname) != disabled.end()) continue;
    if (builtinPkgs.contains(dirname)) continue;

    writeDirMD5(dest, entry.path().string(), std::regex { filter_pattern });
  }
}

// Main function: generate flist.txt, then return its MD5
std::string calcFileMD5() {
  const std::string flist_path = "flist.txt";

  std::ostringstream flist;

  writePkgsMD5(flist, "packages", "^.*\\.lua$");
  writePkgsMD5(flist, "packages", "^.*\\.qml$");
  writePkgsMD5(flist, "packages", "^.*\\.js$");

  std::ofstream flist_file(flist_path, std::ios::out | std::ios::trunc);
  if (!flist_file.is_open()) {
    spdlog::warn("Cannot open flist.txt. Quitting.");
  } else {
    flist_file << flist.str();
    flist_file.close();
  }

  // Now compute MD5 of the generated flist.txt
  std::string content = flist.str();
  MD5_CTX md5_ctx;
  MD5_Init(&md5_ctx);
  MD5_Update(&md5_ctx, content.data(), content.size());

  unsigned char digest[MD5_DIGEST_LENGTH];
  MD5_Final(digest, &md5_ctx);

  return toHex({ (char*)digest, MD5_DIGEST_LENGTH });
}

std::string Color(const std::string &raw, fkShell::TextColor color,
              fkShell::TextType type) {
  static const char *suffix = "\e[0;0m";
  int col = 30 + color;
  int t = type == fkShell::Bold ? 1 : 0;
  auto prefix = fmt::format("\e[{};{}m", t, col);

  return prefix + raw + suffix;
}

std::string toHex(std::string_view sv) {
  std::ostringstream oss;
  for (unsigned char c : sv) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  }
  return oss.str();
}
