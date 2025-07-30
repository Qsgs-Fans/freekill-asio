// SPDX-License-Identifier: GPL-3.0-or-later

#include "c-wrapper.h"
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include "util.h"

Sqlite3::Sqlite3(const char *filename, const char *initSql) {
  std::ifstream file { initSql, std::ios_base::in };
  if (!file.is_open()) {
    spdlog::error("cannot open {}. Quit now.", initSql);
    std::exit(1);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  auto sql = buffer.str();

  int rc;
  char *err_msg;

  rc = sqlite3_open(filename, &db);
  if (rc != SQLITE_OK) {
    spdlog::critical("Cannot open database: {}", sqlite3_errmsg(db));
    sqlite3_close(db);
    std::exit(1);
  }

  rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    spdlog::critical("sqlite error: {}", err_msg);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    std::exit(1);
  }
}

Sqlite3::~Sqlite3() {
  sqlite3_close(db);
}

bool Sqlite3::checkString(const std::string_view &sv) {
  static const std::regex exp(R"(['\";#* /\\?<>|:]+|(--)|(/\*)|(\*/)|(--\+))");
  return !std::regex_search(sv.begin(), sv.end(), exp);
}

// callback for handling SELECT expression
static int callback(void *arr, int argc, char **argv, char **cols) {
  std::map<std::string, std::string> obj;
  for (int i = 0; i < argc; i++) {
    obj[cols[i]] = argv[i] ? argv[i] : "#null";
  }
  ((Sqlite3::QueryResult *)arr)->push_back(obj);
  return 0;
}

Sqlite3::QueryResult Sqlite3::select(const std::string &sql) {
  QueryResult arr;
  char *err = NULL;
  std::lock_guard<std::mutex> locker { select_lock };
  sqlite3_exec(db, sql.c_str(), callback, (void *)&arr, &err);
  if (err) {
    spdlog::error("error occured in select: {}", err);
    sqlite3_free(err);
  }
  return arr;
}

void Sqlite3::exec(const std::string &sql) {
  auto bytes = sql.c_str();
  sqlite3_exec(db, bytes, nullptr, nullptr, nullptr);
}

std::uint64_t Sqlite3::getMemUsage() {
  return sqlite3_memory_used();
}

// -----------------

static std::unique_ptr<Cbor> __cbor = nullptr;
bool Cbor::_instance() {
  if (!__cbor) __cbor = std::unique_ptr<Cbor>(new Cbor);
  return true;
}

template <typename T>
cbor_item_t* build_cbor_int(T value) {
  static_assert(std::is_integral_v<T>, "Integer type required");

  if constexpr (std::is_signed_v<T>) {
    // 处理有符号整数
    if (value >= 0) {
      // 正数作为无符号处理
      uint64_t uvalue = static_cast<uint64_t>(value);
      if (uvalue <= 0xFF) return cbor_build_uint8(uvalue);
      if (uvalue <= 0xFFFF) return cbor_build_uint16(uvalue);
      if (uvalue <= 0xFFFFFFFF) return cbor_build_uint32(uvalue);
      return cbor_build_uint64(uvalue);
    } else {
      // 负数
      int64_t ivalue = static_cast<int64_t>(value);
      if (ivalue >= -128) return cbor_build_negint8(-(ivalue+1));
      if (ivalue >= -32768) return cbor_build_negint16(-(ivalue+1));
      if (ivalue >= -2147483648LL) return cbor_build_negint32(-(ivalue+1));
      return cbor_build_negint64(-(ivalue+1));
    }
  } else {
    // 处理无符号整数
    if (value <= 0xFF) return cbor_build_uint8(value);
    if (value <= 0xFFFF) return cbor_build_uint16(value);
    if (value <= 0xFFFFFFFF) return cbor_build_uint32(value);
    return cbor_build_uint64(value);
  }
}

cbor_item_t* build_cbor_item(int8_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(int16_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(int32_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(int64_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(uint8_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(uint16_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(uint32_t value) { return build_cbor_int(value); }
cbor_item_t* build_cbor_item(uint64_t value) { return build_cbor_int(value); }

cbor_item_t* build_cbor_item(const std::string_view& value) {
  return cbor_build_bytestring((cbor_data)value.data(), value.size());
}

cbor_item_t* build_cbor_item(const char* value) {
  return cbor_build_string(value);
}

cbor_item_t* build_cbor_item(bool value) {
  return cbor_build_bool(value);
}

std::string Cbor::encodeArray(std::initializer_list<std::variant<
                              int, unsigned int, int64_t, uint64_t,
                              std::string_view, const char*, bool>> items) {

  std::string ret;
  ret.reserve(items.size() * 128);
  u_char buf[10]; size_t buflen;

  // 经典array(n)起手
  buflen = cbor_encode_uint(items.size(), buf, 10);
  buf[0] += 0x80;
  ret += std::string_view { (char*)buf, buflen };

  size_t i = 0;
  for (const auto& item : items) {
    std::visit([&](auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, int> || std::is_same_v<T, int64_t> ||
        std::is_same_v<T, unsigned int> || std::is_same_v<T, uint64_t>
      ) {
        if (arg >= 0) {
          buflen = cbor_encode_uint(arg, buf, 10);
        } else {
          buflen = cbor_encode_negint(-1-arg, buf, 10);
        }
        ret += std::string_view { (char*)buf, buflen };
      } else if constexpr (std::is_same_v<T, std::string_view>) {
        buflen = cbor_encode_uint(arg.size(), buf, 10);
        buf[0] += 0x40;
        ret += std::string_view { (char*)buf, buflen };
        ret += arg;
      } else if constexpr (std::is_same_v<T, const char*>) {
        buflen = cbor_encode_uint(strlen(arg), buf, 10);
        buf[0] += 0x60;
        ret += std::string_view { (char*)buf, buflen };
        ret += arg;
      } else if constexpr (std::is_same_v<T, bool>) {
        ret += arg ? "\xF5" : "\xF4";
      }

    }, item);
  }

  return ret;
}

cbor_callbacks Cbor::intCallbacks = cbor_empty_callbacks;
cbor_callbacks Cbor::bytesCallbacks = cbor_empty_callbacks;
cbor_callbacks Cbor::stringCallbacks = cbor_empty_callbacks;
cbor_callbacks Cbor::arrayCallbacks = cbor_empty_callbacks;
cbor_callbacks Cbor::mapCallbacks = cbor_empty_callbacks;

Cbor::Cbor() {
  // 所有类型必须是int 直接操作void *的要求比较严格
  intCallbacks.uint8 = [](void* self, uint8_t value) {
    auto p = static_cast<int *>(self);
    if (p) *p = value;
  };
  intCallbacks.uint16 = [](void* self, uint16_t value) {
    auto p = static_cast<int *>(self);
    if (p) *p = value;
  };
  intCallbacks.uint32 = [](void* self, uint32_t value) {
    if (value > 0x7FFFFFFF) return;
    auto p = static_cast<int *>(self);
    if (p) *p = value;
  };
  intCallbacks.negint8 = [](void* self, uint8_t value) {
    auto p = static_cast<int *>(self);
    if (p) *p = -1 - value;
  };
  intCallbacks.negint16 = [](void* self, uint16_t value) {
    auto p = static_cast<int *>(self);
    if (p) *p = -1 - value;
  };
  intCallbacks.negint32 = [](void* self, uint32_t value) {
    auto p = static_cast<int *>(self);
    if (p) *p = -1 - value;
  };

  stringCallbacks.string = [](void* self, const cbor_data data, size_t len) {
    auto sv = static_cast<std::string_view *>(self);
    if (sv) *sv = { (char *)data, len };
  };
  bytesCallbacks.byte_string = [](void* self, const cbor_data data, size_t len) {
    auto sv = static_cast<std::string_view *>(self);
    if (sv) *sv = { (char *)data, len };
  };

  arrayCallbacks.array_start = [](void* self, size_t size) {
    auto p = static_cast<size_t *>(self);
    if (p) *p = size;
  };
  mapCallbacks.map_start = [](void* self, size_t size) {
    auto p = static_cast<size_t *>(self);
    if (p) *p = size;
  };
}

bool __cbor_unused = Cbor::_instance();
