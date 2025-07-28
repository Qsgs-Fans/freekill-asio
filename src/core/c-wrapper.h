#pragma once

// 为C库提供一层C++包装 方便操作
// 主要是lua和sqlite

struct sqlite3;

class Sqlite3 {
public:
  Sqlite3(const char *filename = "./server/users.db",
          const char *initSql = "./server/init.sql");
  ~Sqlite3();

  static bool checkString(const char *str);

  typedef std::vector<std::map<std::string, std::string>> QueryResult;
  QueryResult select(const std::string &sql);
  void exec(const std::string &sql);

  std::uint64_t getMemUsage();

private:
  sqlite3 *db;
  std::mutex select_lock;
};

class Cbor {
public:
  static std::string encodeArray(std::initializer_list<std::variant<
    int, unsigned int, int64_t, uint64_t,
    std::string_view, const char*, bool>> items);
};
