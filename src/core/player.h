// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Common part of ServerPlayer and ClientPlayer
// dont initialize it directly
class Player {
public:
  enum State{
    Invalid,
    Online,
    Trust,
    Run,
    Leave,
    Robot,  // only for real robot
    Offline
  };

  explicit Player();
  virtual ~Player();

  int getId() const;
  void setId(int id);

  // 这俩 client和server实现有区别
  virtual std::string getScreenName() const = 0;
  virtual void setScreenName(const std::string &name) = 0;

  virtual std::string getAvatar() const = 0;
  virtual void setAvatar(const std::string &avatar) = 0;

  int getTotalGameTime() const;
  void addTotalGameTime(int toAdd);

  State getState() const;
  std::string_view getStateString() const;
  virtual void setState(State state);

  bool isReady() const;
  virtual void setReady(bool ready);

  std::vector<int> getGameData();
  void setGameData(int total, int win, int run);
  std::string_view getLastGameMode() const;
  void setLastGameMode(const std::string &mode);

  bool isDied() const;
  void setDied(bool died);

private:
  int id = 0;
  int totalGameTime = 0;
  State state = Invalid;
  bool ready = false;
  bool died = false;

  std::string lastGameMode;
  int totalGames = 0;
  int winCount = 0;
  int runCount = 0;
};
