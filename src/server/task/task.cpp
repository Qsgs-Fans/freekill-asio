#include "server/task/task.h"
#include "server/task/task_manager.h"
#include "server/server.h"
#include "server/gamelogic/roomthread.h"

namespace asio = boost::asio;

Task::Task(const std::string &type, const std::string &data)
    : taskType { type }, data { data }
{
  static int nextId = -1000;
  id = nextId--;
  if (nextId < -10000000)
    nextId = -1000;

  auto &thread = Server::instance().getAvailableThread();
  m_thread = &thread;
  m_thread->increaseRefCount();
}

Task::~Task() {
  abort();
  m_thread->decreaseRefCount();
}

int Task::getId() const {
  return id;
}

int Task::getUserConnId() const {
  return userConnId;
}

void Task::setUserConnId(int uid) {
  userConnId = uid;
}

int Task::getExpectedReplyId() const {
  return expectedReplyId;
}

void Task::setExpectedReplyId(int rid) {
  expectedReplyId = rid;
}

std::string Task::getTaskType() const {
  return taskType;
}

std::string Task::getData() const {
  return data;
}

void Task::start() {
  m_thread->pushRequest(fmt::format("-1,{},newtask", id));
  increaseRefCount();
}

void Task::resume(const std::string &reason) {
  m_thread->wakeUp(id, reason.c_str());
}

void Task::abort() {
  m_thread->wakeUp(id, "abort");
}

void Task::delay(int ms) {
  m_thread->delay(id, ms);
}

int Task::getRefCount() {
  std::lock_guard<std::mutex> locker(lua_ref_mutex);
  return lua_ref_count;
}

void Task::increaseRefCount() {
  std::lock_guard<std::mutex> locker(lua_ref_mutex);
  lua_ref_count++;
}

void Task::decreaseRefCount() {
  {
    std::lock_guard<std::mutex> locker(lua_ref_mutex);
    lua_ref_count--;
  }

  if (lua_ref_count == 0) {
    // 主线程执行
    asio::dispatch(Server::instance().context(), [id = this->id] {
      auto &tm = Server::instance().task_manager();
      tm.removeTask(id);
    });
  }
}
