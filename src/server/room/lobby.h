// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "server/room/roombase.h"

struct Packet;
class ServerPlayer;

class Lobby final : public RoomBase {
private:
  // connId -> true
  std::unordered_map<int, bool> players;

public:
  Lobby();
  Lobby(Lobby &) = delete;
  Lobby(Lobby &&) = delete;

  auto getPlayers() const -> const decltype(players) &;

  void addPlayer(ServerPlayer &player) final;
  void removePlayer(ServerPlayer &player) final;
  void handlePacket(ServerPlayer &sender, const Packet &packet) final;

  void updateOnlineInfo();

  void checkAbandoned();

private:
  // for handle packet
  void updateAvatar(ServerPlayer &, const Packet &);
  void updatePassword(ServerPlayer &, const Packet &);
  void createRoom(ServerPlayer &, const Packet &);
  void enterRoom(ServerPlayer &, const Packet &);
  void observeRoom(ServerPlayer &, const Packet &);
  void refreshRoomList(ServerPlayer &, const Packet &);

  void joinRoom(ServerPlayer &, const Packet &, bool ob = false);
};
