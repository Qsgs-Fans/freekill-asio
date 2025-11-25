// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/admin/shell.h"
#include "core/packman.h"
// #include "server/rpc-lua/rpc-lua.h"
#include "server/server.h"
#include "server/user/player.h"
#include "server/user/user_manager.h"
#include "server/room/room_manager.h"
#include "server/room/room.h"
#include "server/room/lobby.h"
#include "server/rpc-lua/rpc-lua.h"
#include "server/gamelogic/roomthread.h"
#include "core/util.h"
#include "core/c-wrapper.h"

#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>

namespace asio = boost::asio;

static constexpr const char *prompt = "fk-asio> ";

void Shell::helpCommand(StringList &) {
  spdlog::info("Frequently used commands:");
#define HELP_MSG(a, b)                                                         \
  spdlog::info((a), Color((b), fkShell::Cyan));

  spdlog::info("===== General commands =====");
  HELP_MSG("{}: Display this help message.", "help");
  HELP_MSG("{}: Shut down the server.", "quit");
  HELP_MSG("{}: Crash the server. Useful when encounter dead loop.", "crash");
  HELP_MSG("{}: View status of server.", "stat/gc");
  HELP_MSG("{}: Reload server config file.", "reloadconf/r");

  spdlog::info("");
  spdlog::info("===== Inspect commands =====");
  HELP_MSG("{}: List all online players.", "lsplayer");
  HELP_MSG("{}: List all running rooms, or show player of room by an <id>.", "lsroom");
  HELP_MSG("{}: Broadcast message.", "msg/m");
  HELP_MSG("{}: Broadcast message to a room.", "msgroom/mr");
  HELP_MSG("{}: Kick a player by his <id>.", "kick");
  HELP_MSG("{}: Kick all players in a room, then abandon it.", "killroom");
  HELP_MSG("{}: Delete dead players in the lobby.", "checklobby");

  spdlog::info("");
  spdlog::info("===== Account commands =====");
  HELP_MSG("{}: Ban 1 or more accounts, IP, UUID by their <name>.", "ban");
  HELP_MSG("{}: Unban 1 or more accounts by their <name>.", "unban");
  HELP_MSG(
      "{}: Ban 1 or more IP address. "
      "At least 1 <name> required.",
      "banip");
  HELP_MSG(
      "{}: Unban 1 or more IP address. "
      "At least 1 <name> required.",
      "unbanip");
  HELP_MSG(
      "{}: Ban 1 or more UUID. "
      "At least 1 <name> required.",
      "banuuid");
  HELP_MSG(
      "{}: Unban 1 or more UUID. "
      "At least 1 <name> required.",
      "unbanuuid");
  HELP_MSG("{}: Ban an accounts by his <name> and <duration> (??m/??h/??d/??mo).", "tempban");
  HELP_MSG("{}: Ban a player's chat by his <name> and <duration> (??m/??h/??d/??mo).", "tempmute");
  HELP_MSG("{}: Unban 1 or more players' chat by their <name>.", "unmute");
  HELP_MSG("{}: Add or remove names from whitelist.", "whitelist");
  HELP_MSG("{}: reset <name>'s password to 1234.", "resetpassword/rp");

  spdlog::info("");
  spdlog::info("===== Package commands =====");
  HELP_MSG("{}: Install a new package from <url>.", "install");
  HELP_MSG("{}: Remove a package.", "remove");
  HELP_MSG("{}: List all packages.", "pkgs");
  HELP_MSG("{}: Get packages hash from file system and write to database.", "syncpkgs");
  HELP_MSG("{}: Enable a package.", "enable");
  HELP_MSG("{}: Disable a package.", "disable");
  HELP_MSG("{}: Upgrade a package. Leave empty to upgrade all.", "upgrade/u");
  spdlog::info("For more commands, check the documentation.");

#undef HELP_MSG
}

Shell::~Shell() {
  rl_clear_history();
  m_thread.join();
}

void Shell::start() {
  m_thread = std::thread(&Shell::run, this);
}

void Shell::lspCommand(StringList &) {
  auto &user_manager = Server::instance().user_manager();
  auto &players = user_manager.getPlayers();
  if (players.size() == 0) {
    spdlog::info("No online player.");
    return;
  }
  spdlog::info("Current {} online player(s) are:", players.size());
  for (auto &[_, player] : players) {
    spdlog::info("{} {{id:{}, connId:{}, state:{}}}",
                 player->getScreenName(), player->getId(),
                 player->getConnId(), player->getStateString());
  }
}

void Shell::lsrCommand(StringList &list) {
  auto &user_manager = Server::instance().user_manager();
  auto &room_manager = Server::instance().room_manager();
  if (!list.empty() && !list[0].empty()) {
    auto pid = list[0];
    int id = std::atoi(pid.c_str());

    auto room = room_manager.findRoom(id).lock();
    if (!room) {
      if (id != 0) {
        spdlog::info("No such room.");
      } else {
        spdlog::info("You are viewing lobby, players in lobby are:");

        auto lobby = room_manager.lobby().lock();
        for (auto &[pid, _] : lobby->getPlayers()) {
          auto p = user_manager.findPlayerByConnId(pid).lock();
          if (!p) continue;
          spdlog::info("{} {{id:{}, connId:{}, state:{}}}",
                       p->getScreenName(), p->getId(),
                       p->getConnId(), p->getStateString());
        }
      }
    } else {
      auto pw = room->getPassword();
      spdlog::info("{}, {} {{mode:{}, running={}, pw:{}}}", room->getId(), room->getName(),
        room->getGameMode(), room->isStarted(), pw == "" ? "<nil>" : pw);
      spdlog::info("Players in this room:");

      for (auto pid : room->getPlayers()) {
        auto p = user_manager.findPlayerByConnId(pid).lock();
        if (!p) continue;
        spdlog::info("{} {{id:{}, connId:{}, state:{}}}",
                     p->getScreenName(), p->getId(),
                     p->getConnId(), p->getStateString());
      }
    }

    return;
  }

  const auto &rooms = room_manager.getRooms();
  if (rooms.size() == 0) {
    spdlog::info("No running room.");
    return;
  }
  spdlog::info("Current {} running rooms are:", rooms.size());
  for (auto &[_, room] : rooms) {
    auto pw = room->getPassword();
    spdlog::info("{}, {} {{mode:{}, running={}, pw:{}}}", room->getId(), room->getName(),
                 room->getGameMode(), room->isStarted(), pw == "" ? "<nil>" : pw);
  }
}

void Shell::installCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'install' command need a URL to install.");
    return;
  }

  auto url = list[0];
  PackMan::instance().downloadNewPack(url.c_str());
  Server::instance().refreshMd5();
}

void Shell::removeCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'remove' command need a package name to remove.");
    return;
  }

  auto pack = list[0];
  PackMan::instance().removePack(pack.c_str());
  Server::instance().refreshMd5();
}

void Shell::upgradeCommand(StringList &list) {
  if (list.empty()) {
    auto arr = PackMan::instance().listPackages();
    for (auto &a : arr) {
      PackMan::instance().upgradePack(a["name"].c_str());
    }
    Server::instance().refreshMd5();
    return;
  }

  auto pack = list[0];
  PackMan::instance().upgradePack(pack.c_str());
  Server::instance().refreshMd5();
}

void Shell::enableCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'enable' command need a package name to enable.");
    return;
  }

  auto pack = list[0];
  PackMan::instance().enablePack(pack.c_str());
  Server::instance().refreshMd5();
}

void Shell::disableCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'disable' command need a package name to disable.");
    return;
  }

  auto pack = list[0];
  PackMan::instance().disablePack(pack.c_str());
  Server::instance().refreshMd5();
}

void Shell::lspkgCommand(StringList &) {
  auto arr = PackMan::instance().listPackages();
  spdlog::info("Name\tVersion\t\tEnabled");
  spdlog::info("------------------------------");
  for (auto &a : arr) {
    auto hash = a["hash"];
    spdlog::info("{}\t{}\t{}", a["name"], hash.substr(0, 8), a["enabled"]);
  }
}

void Shell::syncpkgCommand(StringList &) {
  PackMan::instance().syncCommitHashToDatabase();
  Server::instance().refreshMd5();
  spdlog::info("Done.");
}

void Shell::kickCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'kick' command needs a player id.");
    return;
  }

  auto pid = list[0];
  int id = std::atoi(pid.c_str());

  auto p = Server::instance().user_manager().findPlayer(id).lock();
  if (p) {
    p->emitKicked();
  }
}

void Shell::msgCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'msg' command needs message body.");
    return;
  }

  std::string msg;
  for (auto &s : list) {
    msg += s;
    msg += ' ';
  }
  Server::instance().broadcast("ServerMessage", msg);
}

void Shell::msgRoomCommand(StringList &list) {
  if (list.size() < 2) {
    spdlog::warn("The 'msgroom' command needs <roomId> and message body.");
    return;
  }

  auto roomId = atoi(list[0].c_str());
  auto room = Server::instance().room_manager().findRoom(roomId).lock();
  if (!room) {
    spdlog::info("No such room.");
    return;
  }
  std::string msg;
  for (size_t i = 1; i < list.size(); i++) {
    msg += list[i];
    msg += ' ';
  }
  room->doBroadcastNotify(room->getPlayers(), "ServerMessage", msg);
}

static void banAccount(Sqlite3 &db, const std::string_view &name, bool banned) {
  if (!Sqlite3::checkString(name))
    return;
  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return;
  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  db.exec(fmt::format("UPDATE userinfo SET banned={} WHERE id={};",
                  banned ? 1 : 0, id));

  if (banned) {
    auto p = Server::instance().user_manager().findPlayer(id).lock();
    if (p) {
      p->emitKicked();
    }
    spdlog::info("Banned {}.", name);
  } else {
    spdlog::info("Unbanned {}.", name);
  }
}

void Shell::banCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'ban' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();

  for (auto &name : list) {
    banAccount(db, name, true);
  }
}

void Shell::unbanCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unban' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();

  for (auto &name : list) {
    banAccount(db, name, false);
  }

  // unbanipCommand(list);
  unbanUuidCommand(list);
}

static void banIPByName(Sqlite3 &db, const std::string_view &name, bool banned) {
  if (!Sqlite3::checkString(name))
    return;

  static constexpr const char *sql_find =
    "SELECT id, lastLoginIp FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return;
  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  auto addr = obj["lastLoginIp"];

  if (banned) {
    db.exec(fmt::format("INSERT INTO banip VALUES('{}');", addr));

    auto p = Server::instance().user_manager().findPlayer(id).lock();
    if (p) {
      p->emitKicked();
    }
    spdlog::info("Banned IP {}.", addr);
  } else {
    db.exec(fmt::format("DELETE FROM banip WHERE ip='{}';", addr));
    spdlog::info("Unbanned IP {}.", addr);
  }
}

void Shell::banipCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'banip' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();

  for (auto &name : list) {
    banIPByName(db, name, true);
  }
}

void Shell::unbanipCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unbanip' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();

  for (auto &name : list) {
    banIPByName(db, name, false);
  }
}

static void banUuidByName(Sqlite3 &db, const std::string_view &name, bool banned) {
  if (!Sqlite3::checkString(name))
    return;
  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return;
  auto obj = result[0];
  int id = atoi(obj["id"].c_str());

  auto result2 = db.select(fmt::format("SELECT * FROM uuidinfo WHERE id={};", id));
  if (result2.empty())
    return;

  auto uuid = result2[0]["uuid"];

  if (banned) {
    db.exec(fmt::format("INSERT INTO banuuid VALUES('{}');", uuid));

    auto p = Server::instance().user_manager().findPlayer(id).lock();
    if (p) {
      p->emitKicked();
    }
    spdlog::info("Banned UUID {}.", uuid);
  } else {
    db.exec(fmt::format("DELETE FROM banuuid WHERE uuid='{}';", uuid));
    spdlog::info("Unbanned UUID {}.", uuid);
  }
}

void Shell::banUuidCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'banuuid' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();

  for (auto &name : list) {
    banUuidByName(db, name, true);
  }
}

void Shell::unbanUuidCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unbanuuid' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();

  for (auto &name : list) {
    banUuidByName(db, name, false);
  }
}

void Shell::tempbanCommand(StringList &list) {
  if (list.size() != 2) {
    spdlog::warn("usage: tempban <name> <duration>");
    return;
  }

  auto &db = Server::instance().database();
  auto name = list[0];
  auto duration_str = list[1];
  static const char *invalid_dur = "Invalid duration value. "
    "Possible choices: ??m (minute), ??h (hour), ??d (day) and ??mo (month, 30 days).";
  size_t pos;
  long value;
  try {
    value = std::stol(duration_str, &pos);
  } catch (const std::exception& e) {
    spdlog::warn(invalid_dur);
    return;
  }

  if (value < 0) {
    spdlog::warn(invalid_dur);
    return;
  }

  using namespace std::chrono;
  std::string unit = duration_str.substr(pos);

  seconds duration;

  if (unit == "m") {
    duration = value * 60s;
  } else if (unit == "h") {
    duration = value * 3600s;
  } else if (unit == "d") {
    duration = value * 86400s;
  } else if (unit == "mo") {
    duration = value * 2592000s;
  } else {
    spdlog::warn(invalid_dur);
    return;
  }

  auto end_tp = system_clock::now() + duration;
  auto expireTimestamp = duration_cast<seconds>(end_tp.time_since_epoch()).count();

  if (!Sqlite3::checkString(name))
    return;

  static constexpr const char *sql_find =
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return;

  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  db.exec(fmt::format("UPDATE userinfo SET banned=1 WHERE id={};", id));
  db.exec(fmt::format(
    "REPLACE INTO tempban (uid, expireAt) VALUES ({}, {});", id, expireTimestamp));

  auto p = Server::instance().user_manager().findPlayer(id).lock();
  if (p) {
    p->emitKicked();
  }

  std::time_t now_time_t = system_clock::to_time_t(end_tp);
  std::tm local_tm = *std::localtime(&now_time_t);
  spdlog::info("Banned {} until {:04}-{:02}-{:02} {:02}:{:02}:{:02}.", name.c_str(),
               local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
               local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
}

void Shell::tempmuteCommand(StringList &list) {
  if (list.size() != 3) {
    spdlog::warn("usage: tempmute <type> <name> <duration>");
    spdlog::warn("type: 1 for full mute, 2 for blocking $-commands");
    return;
  }

  auto &db = Server::instance().database();
  auto type_num = list[0];
  auto name = list[1];
  auto duration_str = list[2];
  int mute_type = std::stoi(type_num);
  if (mute_type != 1 && mute_type != 2) {
    spdlog::warn("Invalid mute type. Use 1 for full mute, 2 for blocking $-commands");
    return;
  }

  static const char *invalid_dur = "Invalid duration value. "
    "Possible choices: ??m (minute), ??h (hour), ??d (day) and ??mo (month, 30 days).";
  size_t pos;
  long value;
  try {
    value = std::stol(duration_str, &pos);
  } catch (const std::exception& e) {
    spdlog::warn(invalid_dur);
    return;
  }

  if (value < 0) {
    spdlog::warn(invalid_dur);
    return;
  }

  using namespace std::chrono;
  std::string unit = duration_str.substr(pos);

  seconds duration;

  if (unit == "m") {
    duration = value * 60s;
  } else if (unit == "h") {
    duration = value * 3600s;
  } else if (unit == "d") {
    duration = value * 86400s;
  } else if (unit == "mo") {
    duration = value * 2592000s;
  } else {
    spdlog::warn(invalid_dur);
    return;
  }

  auto end_tp = system_clock::now() + duration;
  auto expireTimestamp = duration_cast<seconds>(end_tp.time_since_epoch()).count();

  if (!Sqlite3::checkString(name))
    return;

  static constexpr const char *sql_find = 
    "SELECT id FROM userinfo WHERE name='{}';";
  auto result = db.select(fmt::format(sql_find, name));
  if (result.empty())
    return;

  auto obj = result[0];
  int id = atoi(obj["id"].c_str());
  db.exec(fmt::format(
    "REPLACE INTO tempmute (uid, expireAt, type) VALUES ({}, {}, {});", id, expireTimestamp, mute_type));

  std::time_t now_time_t = system_clock::to_time_t(end_tp);
  std::tm local_tm = *std::localtime(&now_time_t);
  if (mute_type == 1) {
    spdlog::info("Muted {} until {:04}-{:02}-{:02} {:02}:{:02}:{:02}.", name.c_str(),
                local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    } else if (mute_type == 2) {
      spdlog::info("Muted {} from using $-commands until {:04}-{:02}-{:02} {:02}:{:02}:{:02}.", name.c_str(),
                local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
  }
}

void Shell::unmuteCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'unmute' command needs at least 1 <name>.");
    return;
  }
  auto &db = Server::instance().database();

  for (auto &name : list) {
    if (!Sqlite3::checkString(name))
      continue;

    static constexpr const char *sql_find = 
      "SELECT id FROM userinfo WHERE name='{}';";
    auto result = db.select(fmt::format(sql_find, name));
    if (result.empty()) {
      spdlog::info("Player {} not found.", name.c_str());
      continue;
    }

    auto obj = result[0];
    int id = atoi(obj["id"].c_str());
    db.exec(fmt::format("DELETE FROM tempmute WHERE uid={};", id));
    spdlog::info("Unmuted player {}.", name.c_str());
  }
}

void Shell::whitelistCommand(StringList &list) {
  if (list.size() < 2) {
    spdlog::warn("usage: whitelist add/rm <names>...");
    return;
  }

  auto op = list[0];
  auto &server = Server::instance();
  auto &db = server.database();

  if (op == "add") {
    server.beginTransaction();
    for (size_t i = 1; i < list.size(); i++) {
      auto &name = list[i];
      if (!Sqlite3::checkString(name))
        continue;

      db.exec(fmt::format("INSERT INTO whitelist VALUES ('{}');", name));
    }
    server.endTransaction();
  } else if (op == "rm") {
    server.beginTransaction();
    for (size_t i = 1; i < list.size(); i++) {
      auto &name = list[i];
      if (!Sqlite3::checkString(name))
        continue;

      db.exec(fmt::format("DELETE FROM whitelist WHERE name='{}';", name));
    }
    server.endTransaction();
  } else {
    spdlog::warn("usage: whitelist add/rm <names>...");
    return;
  }
}

void Shell::reloadConfCommand(StringList &) {
  Server::instance().reloadConfig();
  spdlog::info("Reloaded server config file.");
}

void Shell::resetPasswordCommand(StringList &list) {
  if (list.empty()) {
    spdlog::warn("The 'resetpassword' command needs at least 1 <name>.");
    return;
  }

  auto &db = Server::instance().database();
  for (auto &name : list) {
    // 重置为1234
    db.exec(fmt::format("UPDATE userinfo SET password="
          "'dbdc2ad3d9625407f55674a00b58904242545bfafedac67485ac398508403ade',"
          "salt='00000000' WHERE name='{}';", name));
  }
}

static std::string formatMsDuration(int64_t time) {
  std::string ret;
  ret.reserve(32);

  auto ms = time % 1000;
  time /= 1000;
  auto sec = time % 60;
  ret = fmt::format("{}.{} seconds", sec, ms) + ret;
  time /= 60;
  if (time == 0) return ret;

  auto min = time % 60;
  ret = fmt::format("{} minutes, ", min) + ret;
  time /= 60;
  if (time == 0) return ret;

  auto hour = time % 24;
  ret = fmt::format("{} hours, ", hour) + ret;
  time /= 24;
  if (time == 0) return ret;

  ret = fmt::format("{} days, ", time) + ret;
  return ret;
}

void Shell::statCommand(StringList &) {
  auto &server = Server::instance();
  auto uptime_ms = server.getUptime();
  spdlog::info("uptime: {}", formatMsDuration(uptime_ms));

  auto players = server.user_manager().getPlayers();
  spdlog::info("Player(s) logged in: {}", players.size());
  // spdlog::info("Rooms: {}", server.room_manager().getRooms().size());

  auto &threads = server.getThreads();
  for (auto &[id, thr] : threads) {
    auto roomsCount = thr->getRefCount();
    auto &L = thr->getLua();

    auto stat_str = L.getConnectionInfo();
    auto outdated = thr->isOutdated();
    if (roomsCount == 0 && outdated) {
      server.removeThread(thr->id());
    } else {
      spdlog::info("RoomThread {} | {} | {} room(s) {}", id, stat_str, roomsCount,
            outdated ? "| Outdated" : "");
    }
  }

  spdlog::info("Database memory usage: {:.2f} MiB",
        ((double)server.database().getMemUsage()) / 1048576);
}

void Shell::killRoomCommand(StringList &list) {
  if (list.empty() || list[0].empty()) {
    spdlog::warn("Need room id to do this.");
    return;
  }

  auto pid = list[0];
  int id = atoi(pid.c_str());

  auto &um = Server::instance().user_manager();
  auto &rm = Server::instance().room_manager();
  auto room = rm.findRoom(id).lock();
  if (!room) {
    spdlog::info("No such room.");
  } else {
    spdlog::info("Killing room {}", id);

    for (auto pConnId : room->getPlayers()) {
      auto player = um.findPlayerByConnId(pConnId).lock();
      if (player && player->getId() > 0)
        player->emitKicked();
    }
    room->checkAbandoned(Room::NoHuman);
  }
}

void Shell::checkLobbyCommand(StringList &) {
  auto &server = Server::instance();
  auto lobby = server.room_manager().lobby().lock();
  asio::post(Server::instance().context(), [&] { lobby->checkAbandoned(); });
}

static void sigintHandler(int) {
  rl_reset_line_state();
  rl_replace_line("", 0);
  rl_crlf();
  rl_forced_update_display();
}
static char **fk_completion(const char *text, int start, int end);
static char *null_completion(const char *, int) { return NULL; }

Shell::Shell() {
  // setObjectName("Shell");
  // Setup readline here

  // 别管Ctrl+C了
  //rl_catch_signals = 1;
  //rl_catch_sigwinch = 1;
  //rl_persistent_signal_handlers = 1;
  //rl_set_signals();
  signal(SIGINT, sigintHandler);
  rl_attempted_completion_function = fk_completion;
  rl_completion_entry_function = null_completion;

  static const std::unordered_map<std::string_view, void (Shell::*)(StringList &)> handlers = {
    {"help", &Shell::helpCommand},
    {"?", &Shell::helpCommand},
    {"lsplayer", &Shell::lspCommand},
    {"lsroom", &Shell::lsrCommand},
    {"install", &Shell::installCommand},
    {"remove", &Shell::removeCommand},
    {"upgrade", &Shell::upgradeCommand},
    {"u", &Shell::upgradeCommand},
    {"pkgs", &Shell::lspkgCommand},
    {"syncpkgs", &Shell::syncpkgCommand},
    {"enable", &Shell::enableCommand},
    {"disable", &Shell::disableCommand},
    {"kick", &Shell::kickCommand},
    {"msg", &Shell::msgCommand},
    {"m", &Shell::msgCommand},
    {"msgroom", &Shell::msgRoomCommand},
    {"mr", &Shell::msgRoomCommand},
    {"ban", &Shell::banCommand},
    {"unban", &Shell::unbanCommand},
    {"banip", &Shell::banipCommand},
    {"unbanip", &Shell::unbanipCommand},
    {"banuuid", &Shell::banUuidCommand},
    {"unbanuuid", &Shell::unbanUuidCommand},
    {"tempban", &Shell::tempbanCommand},
    {"tempmute", &Shell::tempmuteCommand},
    {"unmute", &Shell::unmuteCommand},
    {"whitelist", &Shell::whitelistCommand},
    {"reloadconf", &Shell::reloadConfCommand},
    {"r", &Shell::reloadConfCommand},
    {"resetpassword", &Shell::resetPasswordCommand},
    {"rp", &Shell::resetPasswordCommand},
    {"stat", &Shell::statCommand},
    {"gc", &Shell::statCommand},
    {"killroom", &Shell::killRoomCommand},
    {"checklobby", &Shell::checkLobbyCommand},
    // special command
    {"quit", &Shell::helpCommand},
    {"crash", &Shell::helpCommand},
  };
  handler_map = handlers;
}

void Shell::handleLine(char *bytes) {
  if (!bytes || !strncmp(bytes, "quit", 4)) {
    spdlog::info("Server is shutting down.");
    Server::instance().stop();
    done = true;
    return;
  }

  spdlog::info("Running command: '{}'", bytes);

  if (!strncmp(bytes, "crash", 5)) {
    spdlog::error("Crashing."); // should dump core
    std::exit(1);
    return;
  }

  add_history(bytes);

  auto command = std::string { bytes };
  std::istringstream iss(command);
  std::vector<std::string> command_list;

  for (std::string token; iss >> token;) {
    command_list.push_back(token);
  }
  if (command_list.size() == 0) return;

  auto it = handler_map.find(command_list[0]);
  if (it == handler_map.end()) {
    auto bytes = command_list[0];
    spdlog::warn("Unknown command '{}'. Type 'help' for hints.", bytes);
  } else {
    command_list.erase(command_list.begin());
    (this->*it->second)(command_list);
  }

  free(bytes);
}

void Shell::redisplay() {
  // QString tmp = syntaxHighlight(rl_line_buffer);
  rl_clear_visible_line();
  rl_forced_update_display();

  //moveCursorToStart();
  //printf("\r{}{}", prompt, tmp.toUtf8().constData());
}

void Shell::moveCursorToStart() {
  winsize sz;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &sz);
  int lines = (rl_end + strlen(prompt) - 1) / sz.ws_col;
  printf("\e[%d;%dH", sz.ws_row - lines, 0);
}

void Shell::clearLine() {
  rl_clear_visible_line();
}

bool Shell::lineDone() const {
  return (bool)rl_done;
}

/*
// 最简单的语法高亮，若命令可执行就涂绿，否则涂红
QString Shell::syntaxHighlight(char *bytes) {
  QString ret(bytes);
  auto command = ret.split(' ').first();
  auto func = handler_map[command];
  auto colored_command = command;
  if (!func) {
    colored_command = Color(command, fkShell::Red, fkShell::Bold);
  } else {
    colored_command = Color(command, fkShell::Green);
  }
  ret.replace(0, command.length(), colored_command);
  return ret;
}
*/

char *Shell::generateCommand(const char *text, int state) {
  static size_t list_index, len;
  static std::vector<std::string_view> keys;
  static std::once_flag flag;
  std::call_once(flag, [&] {
    for (const auto &[k, _] : handler_map) {
      keys.push_back(k);
    }
  });
  const char *name;

  if (state == 0) {
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < keys.size()) {
    name = keys[list_index].data();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
}

static char *command_generator(const char *text, int state) {
  return Server::instance().shell().generateCommand(text, state);
}

static char *repo_generator(const char *text, int state) {
  static constexpr const char *recommend_repos[] = {
    "https://gitee.com/Qsgs-Fans/standard_ex",
    "https://gitee.com/Qsgs-Fans/shzl",
    "https://gitee.com/Qsgs-Fans/sp",
    "https://gitee.com/Qsgs-Fans/yj",
    "https://gitee.com/Qsgs-Fans/ol",
    "https://gitee.com/Qsgs-Fans/mougong",
    "https://gitee.com/Qsgs-Fans/mobile",
    "https://gitee.com/Qsgs-Fans/tenyear",
    "https://gitee.com/Qsgs-Fans/overseas",
    "https://gitee.com/Qsgs-Fans/jsrg",
    "https://gitee.com/Qsgs-Fans/qsgs",
    "https://gitee.com/Qsgs-Fans/mini",
    "https://gitee.com/Qsgs-Fans/gamemode",
    "https://gitee.com/Qsgs-Fans/utility",
    "https://gitee.com/Qsgs-Fans/freekill-core",
    "https://gitee.com/Qsgs-Fans/offline",
    "https://gitee.com/Qsgs-Fans/hegemony",
    "https://gitee.com/Qsgs-Fans/lunar",
  };
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < std::size(recommend_repos)) {
    name = recommend_repos[list_index];
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
}

static char *package_generator(const char *text, int state) {
  static Sqlite3::QueryResult arr;
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    arr = PackMan::instance().listPackages();
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index].at("name").c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
}

static char *user_generator(const char *text, int state) {
  // TODO: userinfo表需要一个cache机制
  static Sqlite3::QueryResult arr;
  static size_t list_index, len;
  const char *name;

  if (state == 0) {
    arr = Server::instance().database().select("SELECT name FROM userinfo;");
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index]["name"].c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
};

static char *banned_user_generator(const char *text, int state) {
  // TODO: userinfo表需要一个cache机制
  static Sqlite3::QueryResult arr;
  static size_t list_index, len;
  const char *name;
  auto &db = Server::instance().database();

  if (state == 0) {
    arr = db.select("SELECT name FROM userinfo WHERE banned = 1;");
    list_index = 0;
    len = strlen(text);
  }

  while (list_index < arr.size()) {
    name = arr[list_index]["name"].c_str();
    ++list_index;
    if (strncmp(name, text, len) == 0) {
      return strdup(name);
    }
  }

  return NULL;
};

static char **fk_completion(const char* text, int start, int end) {
  char **matches = NULL;
  if (start == 0) {
    matches = rl_completion_matches(text, command_generator);
  } else {
    auto str = std::string { rl_line_buffer };
    std::istringstream iss(str);
    std::vector<std::string> command_list;

    for (std::string token; iss >> token;) {
      command_list.push_back(token);
    }

    if (command_list.size() > 2) return NULL;
    auto command = command_list[0];
    if (command == "install") {
      matches = rl_completion_matches(text, repo_generator);
    } else if (command == "remove" || command == "upgrade" || command == "u"
        || command == "enable" || command == "disable") {
      matches = rl_completion_matches(text, package_generator);
    } else if (command.starts_with("ban") || command == "tempban"
        || command == "resetpassword" || command == "rp") {
      matches = rl_completion_matches(text, user_generator);
    } else if (command.starts_with("unban")) {
      matches = rl_completion_matches(text, banned_user_generator);
    }
  }

  return matches;
}

void Shell::run() {
  pthread_setname_np(pthread_self(), "Shell");

  printf("\rfreekill-asio, Copyright (C) 2025, GNU GPL'd, by Notify et al.\n");
  printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
  printf(
      "This is free software, and you are welcome to redistribute it under\n");
  printf("certain conditions; For more information visit "
         "http://www.gnu.org/licenses.\n\n");

  printf("[freekill-asio v%s] Welcome to CLI. Enter 'help' for usage hints.\n", FK_VERSION);

  while (true) {
    char *bytes = readline(prompt);
    handleLine(bytes);
    if (done) break;
  }
}
