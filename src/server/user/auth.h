// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _AUTH_H
#define _AUTH_H

class Server;
class Sqlite3;
class ClientSocket;
class AuthManagerPrivate;

struct Packet;

class AuthManager {
public:
  AuthManager();
  AuthManager(AuthManager &) = delete;
  AuthManager(AuthManager &&) = delete;

  ~AuthManager() noexcept;
  std::string_view getPublicKeyCbor() const;

  void processNewConnection(std::shared_ptr<ClientSocket> conn, Packet &packet);

private:
  std::string public_key_cbor;
  std::unique_ptr<AuthManagerPrivate> p_ptr;

  bool loadSetupData(Packet &packet);
  bool checkVersion();

  Sqlite3 *db;

  bool checkIfUuidNotBanned();
  bool checkMd5();
  std::map<std::string, std::string> checkPassword();
  std::map<std::string, std::string> queryUserInfo(const std::string &decrypted_pw);

  void updateUserLoginData(int id);

};

#endif // _AUTH_H
