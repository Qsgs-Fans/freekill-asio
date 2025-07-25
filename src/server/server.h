// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class ServerSocket;
class ClientSocket;

class UserManager;
class RoomManager;

using asio::ip::tcp;

class Server {
public:
  static Server &instance();
  Server(Server &) = delete;
  Server(Server &&) = delete;
  ~Server();

  void listen(asio::io_context &io_ctx, tcp::endpoint end);

  UserManager &user_manager();
  RoomManager &room_manager();

  void sendEarlyPacket(ClientSocket &client, const std::string_view &type, const std::string_view &msg);

  /*
  void updateRoomList(Player *teller);
  void updateOnlineInfo();

  Sqlite3 *getDatabase();

  void broadcast(const QByteArray &command, const QByteArray &jsonData);
  bool isListening;

  QJsonValue getConfig(const QString &command);
  bool checkBanWord(const QString &str);
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

  /*
  QList<QString> temp_banlist;

  Sqlite3 *db;
  QMutex transaction_mutex;
  QString md5;

  QElapsedTimer uptime_counter;

  void readConfig();
  QJsonObject config;

  bool hasWhitelist = false;
  QVariantList whitelist;
  */
};
