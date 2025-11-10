// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class ServerSocket;
class ClientSocket;

class UserManager;
class RoomManager;
class RoomThread;

class Shell;
class Sqlite3;

struct ServerConfig {
  std::vector<std::string> banWords;
  std::string description = "FreeKill Server (non-Qt)";
  std::string iconUrl = "default";
  int capacity = 100;
  int tempBanTime = 0;
  std::string motd = "Welcome!";
  std::vector<std::string> hiddenPacks;
  std::vector<std::string> disabledFeatures;
  bool enableWhitelist = false;
  int roomCountPerThread = 2000;
  int maxPlayersPerDevice = 1000;

  void loadConf(const char *json);

  ServerConfig() = default;
  ServerConfig(ServerConfig &) = delete;
  ServerConfig(ServerConfig &&) = delete;
};

class Server {
public:
  using io_context = boost::asio::io_context;
  using tcp = boost::asio::ip::tcp;
  using udp = boost::asio::ip::udp;

  static Server &instance();

  Server(Server &) = delete;
  Server(Server &&) = delete;
  ~Server();

  void listen(io_context &io_ctx, tcp::endpoint end, udp::endpoint uend);
  void stop();
  static void destroy();

  io_context &context();

  UserManager &user_manager();
  RoomManager &room_manager();
  Sqlite3 &database();
  Sqlite3 &gameDatabase();  // gamedb的getter
  Shell &shell();

  void sendEarlyPacket(ClientSocket &client, const std::string_view &type, const std::string_view &msg);

  RoomThread &createThread();
  void removeThread(int threadId);
  std::weak_ptr<RoomThread> getThread(int threadId);
  RoomThread &getAvailableThread();
  const std::unordered_map<int, std::shared_ptr<RoomThread>> &getThreads() const;

  void broadcast(const std::string_view &command, const std::string_view &jsonData);

  const ServerConfig &config() const;
  void reloadConfig();
  bool checkBanWord(const std::string_view &str);

  void temporarilyBan(int playerId);
  bool isTempBanned(const std::string_view &addr) const;
  int isMuted(int playerId) const;

  void beginTransaction();
  void endTransaction();

  const std::string &getMd5() const;
  void refreshMd5();

  int64_t getUptime() const;

  bool nameIsInWhiteList(const std::string_view &name) const;

private:
  explicit Server();
  std::unique_ptr<ServerConfig> m_config;
  std::unique_ptr<ServerSocket> m_socket;

  std::unique_ptr<Sqlite3> db;
  std::unique_ptr<Sqlite3> gamedb;  // 存档变量
  std::mutex transaction_mutex;

  std::unordered_map<int, std::shared_ptr<RoomThread>> m_threads;

  std::unique_ptr<UserManager> m_user_manager;
  std::unique_ptr<RoomManager> m_room_manager;

  std::unique_ptr<Shell> m_shell;

  io_context *main_io_ctx = nullptr;

  std::vector<std::string> temp_banlist;

  std::string md5;

  int64_t start_timestamp;
  std::unique_ptr<boost::asio::steady_timer> heartbeat_timer;

  boost::asio::awaitable<void> heartbeat();

  void _refreshMd5();
};
