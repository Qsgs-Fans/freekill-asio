// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

struct Packet;
class Player;
class ClientSocket;

class Router {
public:
  enum PacketType {
    TYPE_REQUEST = 0x100,      ///< 类型为Request的包
    TYPE_REPLY = 0x200,        ///< 类型为Reply的包
    TYPE_NOTIFICATION = 0x400, ///< 类型为Notify的包
    SRC_CLIENT = 0x010,        ///< 从客户端发出的包
    SRC_SERVER = 0x020,        ///< 从服务端发出的包
    SRC_LOBBY = 0x040,
    DEST_CLIENT = 0x001,
    DEST_SERVER = 0x002,
    DEST_LOBBY = 0x004
  };

  enum RouterType {
    TYPE_SERVER,
    TYPE_CLIENT
  };

  Router() = delete;
  Router(Player *player, std::shared_ptr<ClientSocket> socket, RouterType type);
  ~Router();

  std::shared_ptr<ClientSocket> getSocket() const;
  void setSocket(std::shared_ptr<ClientSocket> socket);

  // signal connectors
  void set_reply_ready_callback(std::function<void()> callback);
  void set_notification_got_callback(std::function<void(const Packet &)> callback);

  void request(int type, const std::string_view &command,
              const std::string_view &cborData, int timeout, int64_t timestamp = -1);
  void notify(int type, const std::string_view &command, const std::string_view &cborData);
  std::string waitForReply(int timeout);

  void abortRequest();

protected:
  void handlePacket(const Packet &packet);

private:
  std::shared_ptr<ClientSocket> socket;
  Player *player = nullptr;

  RouterType type;

  std::mutex replyMutex;

  int64_t requestStartTime;
  std::string m_reply;    // should be json string
  std::vector<int> expectedReplyIds;
  int replyTimeout;

  void sendMessage(const std::string &msg);

  // signals
  std::function<void()> reply_ready_callback;
  std::function<void(const Packet &)> notification_got_callback;
};
