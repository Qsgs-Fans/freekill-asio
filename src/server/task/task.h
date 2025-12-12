#pragma once

class RoomThread;

class Task {
public:
  Task(const std::string &type, const std::string &data);
  Task(const Task&) = delete;
  Task(Task&&) = delete;
  ~Task();

  int getId() const;
  int getUserConnId() const;
  void setUserConnId(int connId);
  int getExpectedReplyId() const;
  void setExpectedReplyId(int id);

  std::string getTaskType() const;
  std::string getData() const;

  RoomThread *thread() const;

  // 启动并执行
  void start();
  // 继续执行 对应coro.resume
  void resume(const std::string &reason);
  // 中途关闭，对应coro.close
  void abort();

  void delay(int ms);

  int getRefCount();
  void increaseRefCount();
  void decreaseRefCount();

private:
  int id; // 负数
  int userConnId = 0; // 关联的用户，0表示无
  int expectedReplyId = 0; // 正在等待的requestId

  std::string taskType;
  std::string data = "\xF6"; // 必须是CBOR，默认null

  // 很遗憾，负责执行Lua代码的那个类命名成这样了 导致在这里有点违和
  RoomThread *m_thread = nullptr;

  int lua_ref_count = 0;
  std::mutex lua_ref_mutex;
};
