// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class RoomBase;
class Lobby;
class Room;
class ServerPlayer;

class RoomManager {
private:
  // 用有序map吧，有个按id自动排序的小功能
  std::map<int, std::shared_ptr<Room>> rooms;

public:
  explicit RoomManager();
  RoomManager(RoomManager &) = delete;
  RoomManager(RoomManager &&) = delete;

  std::shared_ptr<Room> createRoom(ServerPlayer &, const std::string &name, int capacity,
                                   int timeout = 15, const std::string &settings = "\xA0");

  void removeRoom(int id);

  std::weak_ptr<Room> findRoom(int id) const;
  std::weak_ptr<Lobby> lobby() const;
  auto getRooms() const -> const decltype(rooms) &;

private:
  // what can i say? ServerPlayer::getRoom需要
  std::shared_ptr<Lobby> m_lobby;
};
