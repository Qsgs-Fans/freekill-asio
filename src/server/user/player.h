// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

struct Packet;
class ClientSocket;
class Router;
class Server;
class Room;
class RoomBase;

class Player : public std::enable_shared_from_this<Player> {
public:
  enum State {
    Invalid,
    Online,
    Trust,
    Run,
    Leave,
    Robot,  // only for real robot
    Offline
  };

  explicit Player();
  Player(Player &) = delete;
  Player(Player &&) = delete;
  ~Player();

  // property getter/setters
  int getId() const;
  void setId(int id);

  std::string_view getScreenName() const;
  void setScreenName(const std::string &name);

  std::string_view getAvatar() const;
  void setAvatar(const std::string &avatar);

  int getTotalGameTime() const;
  void addTotalGameTime(int toAdd);

  State getState() const;
  std::string_view getStateString() const;
  bool isOnline() const;
  bool insideGame();
  void setState(State state);

  bool isReady() const;
  void setReady(bool ready);

  std::vector<int> getGameData();
  void setGameData(int total, int win, int run);
  std::string_view getLastGameMode() const;
  void setLastGameMode(const std::string &mode);

  bool isDied() const;
  void setDied(bool died);
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
  int id = 0;
  std::string screenName;   // screenName should not be same.
  std::string avatar;
  int totalGameTime = 0;
  State state = Invalid;
  bool ready = false;
  bool died = false;
  bool runned = false;

  std::string lastGameMode;
  int totalGames = 0;
  int winCount = 0;
  int runCount = 0;

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
