// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/c-wrapper.h"

class ServerSocket;
class ClientSocket;

class UserManager;
class RoomManager;
class RoomThread;

class Shell;

using asio::ip::tcp;

struct ServerConfig {
  std::vector<std::string> banWords;
  std::string description = "FreeKill Server (non-Qt)";
  std::string iconUrl = "default";
  int capacity = 100;
  int tempBanTime = 0;
  std::string motd = "Welcome!";
  std::vector<std::string> hiddenPacks;
  bool enableBots = true;
  int roomCountPerThread = 2000;
  int maxPlayersPerDevice = 1000;

  void loadConf(const char *json);

  ServerConfig() = default;
  ServerConfig(ServerConfig &) = delete;
  ServerConfig(ServerConfig &&) = delete;
};

class Server {
public:
  static Server &instance();

  Server(Server &) = delete;
  Server(Server &&) = delete;
  ~Server();

  void listen(asio::io_context &io_ctx, tcp::endpoint end, asio::ip::udp::endpoint uend);
  void stop();
  static void destroy();

  asio::io_context &context();

  UserManager &user_manager();
  RoomManager &room_manager();
  Sqlite3 &getDatabase();
  Shell &shell();

  void sendEarlyPacket(ClientSocket &client, const std::string_view &type, const std::string_view &msg);

  RoomThread &createThread();
  void removeThread(int threadId);
  RoomThread *getThread(int threadId);
  RoomThread &getAvailableThread();
  std::thread::id mainThreadId() const;

  /*

  void broadcast(const QByteArray &command, const QByteArray &jsonData);
  bool isListening;
  */

  const ServerConfig &config() const;
  void reloadConfig();
  bool checkBanWord(const std::string_view &str);
  /*
  void temporarilyBan(int playerId);

  void beginTransaction();
  void endTransaction();

  const QString &getMd5() const;
  void refreshMd5();

  qint64 getUptime() const;

  bool nameIsInWhiteList(const QString &name) const;
  */

private:
  explicit Server();
  std::unique_ptr<ServerSocket> m_socket;

  std::unique_ptr<UserManager> m_user_manager;
  std::unique_ptr<RoomManager> m_room_manager;
  std::unique_ptr<Sqlite3> db;
  std::unordered_map<int, std::unique_ptr<RoomThread>> m_threads;

  std::unique_ptr<Shell> m_shell;

  asio::io_context *main_io_ctx = nullptr;
  std::thread::id main_thread_id;

  std::unique_ptr<ServerConfig> m_config;

  /*
  QList<QString> temp_banlist;

  Sqlite3 *db;
  QMutex transaction_mutex;
  QString md5;

  QElapsedTimer uptime_counter;

  bool hasWhitelist = false;
  QVariantList whitelist;
  */
};
