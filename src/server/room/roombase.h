// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _ROOMBASE_H
#define _ROOMBASE_H

class Server;
class ServerPlayer;

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

  void chat(ServerPlayer &sender, const Packet &);
  // TODO: 这玩意后面得扔了
  void readGlobalSaveState(ServerPlayer &sender, const Packet &);

  void saveGlobalState(std::string_view key, std::string_view jsonData, std::function<void()> &&);
  void getGlobalSaveState(std::string_view key, std::function<void(std::string)> &&);

  virtual void addPlayer(ServerPlayer &player) = 0;
  virtual void removePlayer(ServerPlayer &player) = 0;
  virtual void handlePacket(ServerPlayer &sender, const Packet &packet) = 0;

protected:
  int id;
};

#endif // _ROOMBASE_H
