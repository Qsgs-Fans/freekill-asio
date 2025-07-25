// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class ClientSocket;
class Router;
class Server;
class Room;
class RoomBase;

class Player {
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
  void setState(State state);

  bool isReady() const;
  void setReady(bool ready);

  std::vector<int> getGameData();
  void setGameData(int total, int win, int run);
  std::string_view getLastGameMode() const;
  void setLastGameMode(const std::string &mode);

  bool isDied() const;
  void setDied(bool died);

  std::string_view getConnId() const;

  // std::string_view getPeerAddress() const;
  std::string_view getUuid() const;
  void setUuid(const std::string &uuid);

  RoomBase &getRoom() const;
  void setRoom(RoomBase &room);

  /*
  // server stuff
  void setSocket(ClientSocket *socket);
  void removeSocket();  // For the running players
  ClientSocket *getSocket() const;


  void speak(const  std::string &&message);

  void doRequest(const QByteArray &command,
                 const QByteArray &jsonData, int timeout = -1, qint64 timestamp = -1);
  void abortRequest();
  std::string &waitForReply(int timeout);
  void doNotify(const QByteArray &command, const QByteArray &jsonData);

  void prepareForRequest(const  std::string &&command,
                         const  std::string &&data);

  volatile bool alive; // For heartbeat
  void kick();
  void reconnect(ClientSocket *socket);

  bool thinking();
  void setThinking(bool t);

  void startGameTimer();
  void pauseGameTimer();
  void resumeGameTimer();
  int getGameTime();
  */

  /*
signals:
  void stateChanged();
  void readyChanged();
  void kicked();

public slots:
  void onNotificationGot(const QByteArray &c, const QByteArray &j);
  void onReplyReady();
  void onStateChanged();
  void onReadyChanged();
  void onDisconnected();
  */

private:
  int id = 0;
  std::string screenName;   // screenName should not be same.
  std::string avatar;
  int totalGameTime = 0;
  State state = Invalid;
  bool ready = false;
  bool died = false;

  std::string lastGameMode;
  int totalGames = 0;
  int winCount = 0;
  int runCount = 0;

  std::string connId;
  std::string uuid_str;

  int roomId;       // Room that player is in, maybe lobby

  /*
  ClientSocket *socket;   // socket for communicating with client
  Router *router;
  bool m_thinking; // 是否在烧条？
  std::mutex m_thinking_mutex;

  int gameTime = 0; // 在这个房间的有效游戏时长(秒)
  QElapsedTimer gameTimer;
  */
};
