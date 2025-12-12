// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/player.h"

struct Packet;
class ClientSocket;
class Router;
class Server;
class Room;
class RoomBase;

class ServerPlayer final:
  public Player,
  public std::enable_shared_from_this<ServerPlayer>
{
public:
  explicit ServerPlayer();
  ServerPlayer(ServerPlayer &) = delete;
  ServerPlayer(ServerPlayer &&) = delete;
  ~ServerPlayer();

  std::string getScreenName() const override;
  void setScreenName(const std::string &name) override;

  std::string getAvatar() const override;
  void setAvatar(const std::string &avatar) override;

  bool isOnline() const;
  bool insideGame();
  void setState(State state) override;
  void setReady(bool ready) override;

  bool isRunned() const;
  void setRunned(bool run);

  int getConnId() const;

  // std::string_view getPeerAddress() const;
  std::string_view getUuid() const;
  void setUuid(const std::string &uuid);

  std::weak_ptr<RoomBase> getRoom() const;
  void setRoom(RoomBase &room);

  Router &router() const;

  void doRequest(const std::string_view &command,
                 const std::string_view &jsonData, int timeout = -1, int64_t timestamp = -1);
  std::string waitForReply(int timeout);
  void doNotify(const std::string_view &command, const std::string_view &data);

  // 心跳用，若连续TTL个心跳都不回应就踢
  enum { max_ttl = 6 };
  int ttl = max_ttl;

  bool thinking();
  void setThinking(bool t);

  void onNotificationGot(const Packet &);
  void onReplyReady();
  void onStateChanged();
  void onReadyChanged();
  void onDisconnected();

  Router &getRouter();
  void emitKicked();
  void reconnect(std::shared_ptr<ClientSocket> socket);

  void startGameTimer();
  void pauseGameTimer();
  void resumeGameTimer();
  int getGameTime();

  // 模式存档
  void saveState(std::string_view jsonData, std::function<void()> &&);
  void getSaveState(std::function<void(std::string)> &&);
  // 全局存档
  void saveGlobalState(std::string_view key, std::string_view jsonData, std::function<void()> &&);
  void getGlobalSaveState(std::string_view key, std::function<void(std::string)> &&);

private:
  std::string screenName;   // screenName should not be same.
  std::string avatar;
  bool runned = false;

  int connId;
  std::string uuid_str;

  int roomId;       // Room that player is in, maybe lobby

  std::unique_ptr<Router> m_router;

  bool m_thinking; // 是否在烧条？
  std::mutex m_thinking_mutex;

  void kick();

  int64_t gameTime = 0; // 在这个房间的有效游戏时长(秒)
  int64_t gameTimerStartTimestamp;
};
