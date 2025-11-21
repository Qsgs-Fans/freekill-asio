#pragma once

// 为sqlite3读写单独开线程执行 并采用异步方式沟通
// 目前仅用于gamedb

#include "core/c-wrapper.h"

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
void DbThread::async_select(const std::string &sql, T &&token) {
  using handler_type = typename asio::async_result<T>::completion_handler_type;
  handler_type handler(std::forward<T>(token));
  asio::async_result<T> result(handler);

  asio::post(worker_io, [this, sql, handler] {
    auto ret = db->select(sql);

    asio::post(main_io, [handler, ret] {
      handler(ret);
    });
  });
}

template<typename T>
void DbThread::async_exec(const std::string &sql, T &&token) {
  using handler_type = typename asio::async_result<T>::completion_handler_type;
  handler_type handler(std::forward<T>(token));
  asio::async_result<T> result(handler);

  asio::post(worker_io, [this, sql, handler] {
    db->exec(sql);

    asio::post(main_io, [handler] {
      handler();
    });
  });
}
