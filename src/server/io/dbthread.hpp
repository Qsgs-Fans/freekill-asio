#pragma once

// 为sqlite3读写单独开线程执行 并采用异步方式沟通
// 目前仅用于gamedb

#include "core/c-wrapper.h"
#include <pthread.h>

class DbThread {
public:
  using io_context = boost::asio::io_context;

  DbThread(io_context &main_io, std::unique_ptr<Sqlite3> &&db) :
    main_io { main_io }, db { std::move(db) } {}

  DbThread(DbThread &) = delete;
  DbThread(DbThread &&) = delete;

  ~DbThread() {
    worker_io.stop();
    m_thread.join();
  }

  void start() {
    m_thread = std::thread([&] {
      pthread_setname_np(pthread_self(), "DbThread");
      auto guard = boost::asio::make_work_guard(worker_io);
      worker_io.run();
    });
  }

  template<typename T>
  void async_select(const std::string &sql, T &&token);

  template<typename T>
  void async_exec(const std::string &sql, T &&token);

private:
  io_context worker_io;
  io_context &main_io;
  std::thread m_thread;

  std::unique_ptr<Sqlite3> db;
};

// --------------------------------

namespace asio = boost::asio;

template<typename T>
void DbThread::async_select(const std::string &sql, T &&handler) {
  asio::post(worker_io, [this, sql, handler = std::forward<T>(handler)] {
    auto ret = db->select(sql);

    asio::post(main_io, [handler = std::move(handler), ret] {
      handler(ret);
    });
  });
}

template<typename T>
void DbThread::async_exec(const std::string &sql, T &&handler) {
  asio::post(worker_io, [this, sql, handler = std::forward<T>(handler)] {
    db->exec(sql);

    asio::post(main_io, [handler = std::move(handler)] {
      handler();
    });
  });
}
