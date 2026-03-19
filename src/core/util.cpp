// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/util.h"
#include "core/packman.h"
#include <openssl/md5.h>
#include <zlib.h>

namespace fs = std::filesystem;

using hash_pair = std::pair<std::string, std::string>;
static std::vector<hash_pair> lua_hashes, qml_hashes, js_hashes;

// Read file content, normalize \r\n → \n, compute MD5
std::string computeFileMD5(const std::string &fname) {
  std::ifstream file(fname, std::ios::binary);
  if (!file.is_open()) {
    return std::string(32, '0'); // Return 32-char zero hash if fail
  }

  // 边读边算md5 且遇到\r\n时跳过\r
  MD5_CTX ctx;
  MD5_Init(&ctx);

  char buffer[8192];
  bool pending_cr = false; // 上一个 buffer 结尾是 '\r'
  size_t start = 0;

  // \r\n只是为了严格贴合原版的逻辑，据我所知代码文件没几个有\r\n的，所以unlikely

  while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
    size_t n = file.gcount();
    start = 0;

    for (size_t i = 0; i < n; ++i) {
      char c = buffer[i];

      [[unlikely]] if (pending_cr) {
        if (c == '\n') {
          // \r\n -> 批量前面内容
          if (i > 0) MD5_Update(&ctx, buffer, i);
          MD5_Update(&ctx, "\n", 1);
          start = i + 1;
        } else {
          // \r 单独处理
          MD5_Update(&ctx, "\r", 1);
          start = i;
        }
        pending_cr = false;
      }

      [[unlikely]] if (c == '\r') {
        // 批量更新 [start, i)
        if (i > start) MD5_Update(&ctx, buffer + start, i - start);

        // 判断 \r 是否在 buffer 末尾
        if (i + 1 < n) {
          if (buffer[i + 1] == '\n') {
            // CRLF -> 写 \n
            MD5_Update(&ctx, "\n", 1);
            i++; // 跳过 \n
            start = i + 1;
          } else {
            // 单独 \r
            MD5_Update(&ctx, "\r", 1);
            start = i + 1;
          }
        } else {
          // buffer 最后一个字符，延迟处理
          pending_cr = true;
          start = i + 1;
        }
      }
    }

    // 批量更新 buffer 中剩余普通字符
    if (n > start) {
      MD5_Update(&ctx, buffer + start, n - start);
    }
  }

  // 文件结尾如果还有一个孤立 \r
  [[unlikely]] if (pending_cr) {
    MD5_Update(&ctx, "\r", 1);
  }

  unsigned char digest[MD5_DIGEST_LENGTH];
  MD5_Final(digest, &ctx);

  return toHex({ (char*)digest, MD5_DIGEST_LENGTH });
}

// Recursively write all files matching regex (sorted by name), dirs first in name order
void writeDirMD5(const std::string &dir) {
  fs::path path(dir);

  // map天生有序 正好如同QDir那样字母序排序
  std::map<std::string, fs::directory_entry> entries;
  for (const auto& entry : fs::directory_iterator(path)) {
    entries.emplace(entry.path().filename().string(), entry);
  }

  for (const auto& [filename, entry] : entries) {
    if (entry.is_directory()) {
      writeDirMD5(entry.path().string());
    } else if (entry.is_regular_file()) {
      auto path = entry.path().string();
      if (filename.ends_with(".lua")) {
        lua_hashes.push_back({ path, computeFileMD5(path) });
      } else if (filename.ends_with(".qml")) {
        qml_hashes.push_back({ path, computeFileMD5(path) });
      } else if (filename.ends_with(".js")) {
        js_hashes.push_back({ path, computeFileMD5(path) });
      }
    }
  }
}

// Handle packages: scan top-level dirs under "packages", skip .disabled, disabled packs, and built-ins
void writePkgsMD5() {
  lua_hashes.clear();
  qml_hashes.clear();
  js_hashes.clear();

  fs::path path("packages");
  auto disabled = PackMan::instance().getDisabledPacks();
  static const std::set<std::string> builtinPkgs = {
    "standard", "standard_cards", "maneuvering", "test"
  };

  std::map<std::string, fs::directory_entry> entries;

  for (const auto& entry : fs::directory_iterator(path)) {
    if (entry.is_directory()) {
      entries.emplace(entry.path().filename().string(), entry);
    }
  }

  for (const auto& [filename, entry] : entries) {
    // Skip .disabled directories
    if (filename.ends_with(".disabled")) continue;
    if (std::ranges::find(disabled, filename) != disabled.end()) continue;
    if (builtinPkgs.contains(filename)) continue;

    writeDirMD5(entry.path().string());
  }
}

// Main function: generate flist.txt, then return its MD5
std::string calcFileMD5() {
  const std::string flist_path = "flist.txt";

  writePkgsMD5();

  std::string flist;
  size_t total = lua_hashes.size() + qml_hashes.size() + js_hashes.size();
  flist.reserve(total * 120);

  auto append = [&](const std::vector<hash_pair>& files){
    for (const auto& hp : files) {
      flist += hp.first;
      flist += '=';
      flist += hp.second;
      flist += ';';
    }
  };
  append(lua_hashes);
  append(qml_hashes);
  append(js_hashes);

  std::ofstream flist_file(flist_path, std::ios::out | std::ios::trunc);
  if (!flist_file.is_open()) {
    spdlog::warn("Cannot open flist.txt!");
  } else {
    flist_file << flist;
    flist_file.close();
  }

  // Now compute MD5 of the generated flist.txt
  MD5_CTX md5_ctx;
  MD5_Init(&md5_ctx);
  MD5_Update(&md5_ctx, flist.data(), flist.size());

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

static inline void append_be32(std::string &out, uint32_t v) {
  out.push_back(char((v >> 24) & 0xff));
  out.push_back(char((v >> 16) & 0xff));
  out.push_back(char((v >>  8) & 0xff));
  out.push_back(char((v      ) & 0xff));
}

std::string qCompress_std(const std::string_view &data, int level) {
  if (data.empty())
    return std::string("\0\0\0\0", 4);

  uLong srcLen = data.size();
  uLong destLen = compressBound(srcLen);

  std::string out;
  out.reserve(4 + destLen);

  // Qt 风格：先写原始长度（大端）
  append_be32(out, static_cast<uint32_t>(srcLen));

  out.resize(4 + destLen);

  int ret = compress2(
    reinterpret_cast<Bytef*>(&out[4]),
    &destLen,
    reinterpret_cast<const Bytef*>(data.data()),
    srcLen,
    level
  );

  if (ret != Z_OK)
    throw std::runtime_error("qCompress_std: compress failed");

  out.resize(4 + destLen);
  return out;
}

static inline uint32_t read_be32(const char *p) {
  return (uint32_t(uint8_t(p[0])) << 24) |
  (uint32_t(uint8_t(p[1])) << 16) |
  (uint32_t(uint8_t(p[2])) <<  8) |
  (uint32_t(uint8_t(p[3])));
}

std::string qUncompress_std(const std::string_view &data) {
  if (data.size() < 4)
    return {};

  uint32_t expectedSize = read_be32(data.data());
  if (expectedSize == 0)
    return {};

  std::string out;
  out.resize(expectedSize);

  uLongf destLen = expectedSize;
  int ret = uncompress(
    reinterpret_cast<Bytef*>(&out[0]),
    &destLen,
    reinterpret_cast<const Bytef*>(data.data() + 4),
    data.size() - 4
  );

  if (ret != Z_OK || destLen != expectedSize)
    throw std::runtime_error("qUncompress_std: uncompress failed");

  return out;
}
