// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class ClientSocket;
class ServerPlayer;
class AuthManager;

class UserManager {
public:
  explicit UserManager();
  UserManager(UserManager &) = delete;
  UserManager(UserManager &&) = delete;

  std::weak_ptr<ServerPlayer> findPlayer(int id) const;
  std::weak_ptr<ServerPlayer> findPlayerByConnId(int connId) const;
  void addPlayer(std::shared_ptr<ServerPlayer> player);
  void deletePlayer(ServerPlayer &p);
  void removePlayer(ServerPlayer &p, int id);
  void removePlayerByConnId(int connid);

  const std::unordered_map<int, std::shared_ptr<ServerPlayer>> &getPlayers() const;

  void processNewConnection(std::shared_ptr<ClientSocket> client);

  void createNewPlayer(std::shared_ptr<ClientSocket> client, std::string_view name, std::string_view avatar, int id, std::string_view uuid_str);
  ServerPlayer &createRobot();

  void setupPlayer(ServerPlayer &player, bool all_info = true);

private:
  std::unique_ptr<AuthManager> m_auth;

  // connId -> ServerPlayer
  std::unordered_map<int, std::shared_ptr<ServerPlayer>> players_map;
  // Id -> ServerPlayer
  std::unordered_map<int, std::shared_ptr<ServerPlayer>> robots_map;
  std::unordered_map<int, std::shared_ptr<ServerPlayer>> online_players_map;

  std::weak_ptr<ServerPlayer> findRobot(int id) const;
};
