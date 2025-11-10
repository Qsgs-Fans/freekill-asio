// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "server/room/roombase.h"

class Server;
class Player;
class RoomThread;

class ClientSocket;

class Room final : public RoomBase, public std::enable_shared_from_this<Room> {
public:
  explicit Room();
  Room(Room &) = delete;
  Room(Room &&) = delete;
  ~Room();

  void addPlayer(Player &player);
  void removePlayer(Player &player);
  void handlePacket(Player &sender, const Packet &packet);

  // Property reader & setter
  // ==================================={
  std::string &getName();
  void setName(const std::string_view &name);
  size_t getCapacity() const;
  void setCapacity(size_t capacity);
  bool isFull() const;

  const std::vector<int> &getPlayers() const;

  // TODO 改成用得到的password和gameMode
  // const QJsonObject getSettingsObject() const;
  const std::string_view getSettings() const;
  const std::string_view getGameMode() const;
  const std::string_view getPassword() const;
  void setSettings(const std::string_view &settings);
  bool isAbandoned() const;

  std::weak_ptr<Player> getOwner() const;
  void setOwner(Player &owner);

  void addRobot(Player &player);

  void addObserver(Player &player);
  void removeObserver(Player &player);
  bool hasObserver(Player &player) const;
  const std::vector<int> &getObservers() const;

  int getTimeout() const;
  void setTimeout(int timeout);
  void delay(int ms);

  bool isOutdated();
  void setOutdated();

  bool isStarted();

  std::weak_ptr<RoomThread> thread() const;
  void setThread(RoomThread &);

  enum CheckAbandonReason {
    NoRefCount,
    NoHuman,
  };

  void checkAbandoned(CheckAbandonReason reason);

  // ====================================}

  void updatePlayerWinRate(int id, const std::string_view &mode, const std::string_view &role, int result);
  void updateGeneralWinRate(const std::string_view &general, const std::string_view &mode, const std::string_view &role, int result);

  void gameOver();
  void manuallyStart();
  void pushRequest(const std::string &req);

  void addRejectId(int id);
  void removeRejectId(int id);
  bool isRejected(Player &) const;

  // router用
  void setRequestTimer(int ms);
  void destroyRequestTimer();

  // Lua专用
  int getRefCount();
  void increaseRefCount();
  void decreaseRefCount();

  int getSessionId() const;
  std::string_view getSessionData() const;
  void setSessionData(std::string_view json);

private:
  int m_thread_id = 0;

  // connId[]
  std::vector<int> players;
  std::vector<int> observers;

  std::string name;         // “阴间大乱斗”
  size_t capacity = 0;         // by default is 5, max is 8
  int m_owner_conn_id = 0;

  std::string settings;
  std::string gameMode;
  std::string password;

  // id[]
  std::vector<int> rejected_players;

  int timeout = 0;
  std::string md5;

  // 表示此房被多少个Lua room引用，为0时才能回收
  // 显然这个数字一般来说最大为1
  int lua_ref_count = 0;
  std::mutex lua_ref_mutex;

  // 表示此房正在运行第几局游戏
  int session_id = 0;
  // 以及某个供Lua往里面放点数据的东西
  std::string session_data = "{}";

  std::unique_ptr<boost::asio::steady_timer> request_timer = nullptr;

  void createRunnedPlayer(Player &player, std::shared_ptr<ClientSocket> socket);
  void detectSameIpAndDevice();
  void updatePlayerGameTime();

  void _gameOver();
  void _checkAbandoned(CheckAbandonReason reason);

  void addRunRate(int id, const std::string_view &mode);
  void updatePlayerGameData(int id, const std::string_view &mode);

  void setPlayerReady(Player &, bool ready);

  // handle packet
  void quitRoom(Player &, const Packet &);
  void addRobotRequest(Player &, const Packet &);
  void kickPlayer(Player &, const Packet &);
  void ready(Player &, const Packet &);
  void startGame(Player &, const Packet &);
  void trust(Player &, const Packet &);
  void changeRoom(Player &, const Packet &);

};
