// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _ROOMBASE_H
#define _ROOMBASE_H

class Server;
class Player;

struct Packet;

class RoomBase {
public:
  RoomBase() = default;
  RoomBase(RoomBase &) = delete;
  RoomBase(RoomBase &&) = delete;

  bool isLobby() const;

  int getId() const;

  void doBroadcastNotify(const std::vector<int> targets,
                         const std::string_view &command, const std::string_view &cborData);

  void chat(Player &sender, const Packet &);
  void readGlobalSaveState(Player &sender, const Packet &);

  virtual void addPlayer(Player &player) = 0;
  virtual void removePlayer(Player &player) = 0;
  virtual void handlePacket(Player &sender, const Packet &packet) = 0;

protected:
  int id;
};

#endif // _ROOMBASE_H
