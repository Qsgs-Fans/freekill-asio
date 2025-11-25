// SPDX-License-Identifier: GPL-3.0-or-later

#include "server/rpc-lua/rpc-lua.h"
#include "core/packman.h"
#include "server/rpc-lua/jsonrpc.h"

#include "server/gamelogic/rpc-dispatchers.h"

#include "core/util.h"

#include <unistd.h>
#include <sys/wait.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using namespace JsonRpc;
namespace asio = boost::asio;

// 传过去的算上call和返回值只有int bytes和null... 毁灭吧
static void sendParam(asio::posix::stream_descriptor &file, JsonRpcParam &param) {
  u_char buf[10]; size_t buflen;
  std::visit([&](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, int>) {
      if (arg >= 0) {
        buflen = cbor_encode_uint(arg, buf, 10);
      } else {
        buflen = cbor_encode_negint(-1-arg, buf, 10);
      }
      file.write_some(asio::const_buffer(buf, buflen));
    } else if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>) {
      buflen = cbor_encode_uint(arg.size(), buf, 10);
      buf[0] += 0x40;
      file.write_some(asio::const_buffer(buf, buflen));
      file.write_some(asio::const_buffer(arg.data(), arg.size()));
    } else if constexpr (std::is_same_v<T, bool>) {
      file.write_some(asio::const_buffer(arg ? "\xF5" : "\xF4", 1));
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
      file.write_some(asio::const_buffer("\xF6", 1));
    }
  }, param);
}

// request: { jsonRpc, method, params, id }
static void sendRequest(asio::posix::stream_descriptor &file, JsonRpcPacket &pkt) {
  u_char buf[10]; size_t buflen;
  // { jsonRpc: '2.0', method: '
  file.write_some(asio::const_buffer("\xa4\x18\x64\x43\x32\x2e\x30\x18\x65", 9));
  buflen = cbor_encode_uint(pkt.method.size(), buf, 10);
  buf[0] += 0x40;
  // <method>',
  file.write_some(asio::const_buffer(buf, buflen));
  file.write_some(asio::const_buffer(pkt.method.data(), pkt.method.size()));
  // id:
  buflen = cbor_encode_uint(pkt.id, buf, 10);
  file.write_some(asio::const_buffer("\x18\x68", 2));
  file.write_some(asio::const_buffer(buf, buflen));
  // params + arr head
  size_t i = pkt.param_count;
  buflen = cbor_encode_uint(i, buf, 10);
  buf[0] += 0x80;
  file.write_some(asio::const_buffer("\x18\x66", 2));
  file.write_some(asio::const_buffer(buf, buflen));

  if (i == 0) return;
  sendParam(file, pkt.param1);
  i--;

  if (i == 0) return;
  sendParam(file, pkt.param2);
  i--;

  if (i == 0) return;
  sendParam(file, pkt.param3);
}

// response: { jsonRpc, result, id }
static void sendResponse(asio::posix::stream_descriptor &file, JsonRpcPacket &pkt) {
  u_char buf[10]; size_t buflen;
  // { jsonRpc: '2.0', id:
  file.write_some(asio::const_buffer("\xa3\x18\x64\x43\x32\x2e\x30\x18\x68", 9));

  // id
  buflen = cbor_encode_uint(pkt.id, buf, 10);
  file.write_some(asio::const_buffer(buf, buflen));

  // result
  file.write_some(asio::const_buffer("\x18\x69", 2));
  sendParam(file, pkt.result);
}

// response: { jsonRpc, error, [id] }
static void sendError(asio::posix::stream_descriptor &file, JsonRpcPacket &pkt) {
  u_char buf[10]; size_t buflen;

  if (pkt.id < 0) {
    file.write_some(asio::const_buffer("\xa2", 1));
  } else {
    file.write_some(asio::const_buffer("\xa3", 1));
  }

  // { jsonRpc: '2.0',
  file.write_some(asio::const_buffer("\x18\x64\x43\x32\x2e\x30", 6));

  // [id]
  if (pkt.id >= 0) {
    buflen = cbor_encode_uint(pkt.id, buf, 10);
    file.write_some(asio::const_buffer("\x18\x68", 2));
    file.write_some(asio::const_buffer(buf, buflen));
  }

  // error: { code:
  file.write_some(asio::const_buffer("\x18\x67\xA3\x18\xC8", 5));
  buflen = cbor_encode_negint(pkt.error.code, buf, 10);
  file.write_some(asio::const_buffer(buf, buflen));

  // msg:
  file.write_some(asio::const_buffer("\x18\xC9", 2));
  buflen = cbor_encode_uint(pkt.error.message.size(), buf, 10);
  buf[0] += 0x40;
  file.write_some(asio::const_buffer(buf, buflen));
  file.write_some(asio::const_buffer(pkt.error.message.data(), pkt.error.message.size()));

  // data:
  file.write_some(asio::const_buffer("\x18\xCA", 2));
  sendParam(file, pkt.error.data);
}

struct RpcPacketBuilder {
  enum State {
    NOT_START,
    WAIT_KEY,
    WAIT_VALUE,
    READING_PARAMS,
    READING_ERROR_K,
    READING_ERROR_V,
    FIN,
    ERROR,
  };

  explicit RpcPacketBuilder(JsonRpcPacket &p) : pkt { p } {
    reset();
  }

  void handleInteger(int64_t value) {
    if (state == WAIT_KEY) {
      current_key = (int)value;
      state = WAIT_VALUE;
    } else if (state == READING_ERROR_K) {
      current_err_key = (int)value;
      state = READING_ERROR_V;
    } else if (state == WAIT_VALUE) {
      switch ((JsonKeys)current_key) {
        case Id:
          pkt.id = (int)value;
          nextKey();
          break;
        case Result:
          nextKey();
          break;
        default:
          checkState(ERROR);
          break;
      }
    } else if (state == READING_ERROR_V) {
      if (current_err_key == ErrorCode) {
        pkt.error.code = (int)value;
      } else if (current_err_key == ErrorData) {
        ; // no-op
      } else {
        checkState(ERROR);
        return;
      }

      error_value_readed++;
      if (error_value_readed == error_key_count) {
        nextKey();
      } else {
        state = READING_ERROR_K;
      }
    } else if (state == READING_PARAMS) {
      if (value < 0 || (uint64_t)value < 0xFFFFFFFF) {
        readParam((int)value);
      } else {
        readParam(value);
      }
    } else {
      checkState(ERROR);
    }
  }

  void handleBool(bool value) {
    if (state == WAIT_VALUE) {
      if (current_key == Result) {
        nextKey();
      } else {
        checkState(ERROR);
      }
    } else if (state == READING_PARAMS) {
      readParam(value);
    } else {
      checkState(ERROR);
    }
  }

  void handleBytes(const cbor_data data, size_t len) {
    if (state == WAIT_VALUE) {
      std::string_view sv { (char *)data, len };
      switch ((JsonKeys)current_key) {
        case JsonRpc::JsonRpc:
          if (sv != "2.0") {
            checkState(ERROR);
            return;
          }
          nextKey();
          break;
        case Method:
          pkt.method = sv;
          nextKey();
          break;
        case Result:
          nextKey();
          break;
        default:
          checkState(ERROR);
          break;
      }
    } else if (state == READING_PARAMS) {
      readParam(std::string_view { (char *)data, len });
    } else if (state == READING_ERROR_V) {
      if (current_err_key == ErrorMessage) {
        pkt.error.message = std::string_view { (char *)data, len };
      } else if (current_err_key == ErrorData) {
        ; // no-op
      } else {
        checkState(ERROR);
        return;
      }

      error_value_readed++;
      if (error_value_readed == error_key_count) {
        nextKey();
      } else {
        state = READING_ERROR_K;
      }
    } else {
      checkState(ERROR);
    }
  }

  void startArray(size_t size) {
    if (!checkState(WAIT_VALUE)) return;
    if (current_key != Params) {
      checkState(ERROR);
      return;
    }

    param_count = size;
    pkt.param_count = size;
    if (size > 0) {
      state = READING_PARAMS;
    } else {
      nextKey();
    }
  }

  void startMap(size_t size) {
    if (state == NOT_START) {
      key_count = size;
      valid = true;
      state = WAIT_KEY;
    } else if (state == WAIT_VALUE && current_key == Error) {
      error_key_count = size;
      state = READING_ERROR_K;
    } else {
      checkState(ERROR);
    }
  }

  void reset() {
    pkt.reset();
    state = NOT_START;
    key_count = 0;
    param_count = 0;
    current_param_idx = 0;
    value_readed = 0;
    error_value_readed = 0;
  }

  void nextKey() {
    value_readed++;
    if (value_readed == key_count) {
      state = FIN;
    } else {
      state = WAIT_KEY;
    }
  }

  void readParam(JsonRpcParam v) {
    switch (current_param_idx) {
      case 0:
        pkt.param1 = v;
        break;
      case 1:
        pkt.param2 = v;
        break;
      case 2:
        pkt.param3 = v;
        break;
      case 3:
        pkt.param4 = v;
        break;
      case 4:
        pkt.param5 = v;
        break;
      default:
        checkState(ERROR);
        return;
    }
    current_param_idx++;
    if (current_param_idx == param_count) {
      nextKey();
    }
  }

  bool checkState(State st) {
    if (state != st) {
      state = ERROR;
      valid = false;
      return false;
    }
    return true;
  }

  State state = NOT_START;
  size_t key_count = 0;
  size_t param_count = 0;
  bool valid = false;
  int current_key;
  size_t value_readed = 0;
  size_t current_param_idx = 0;
  size_t error_key_count = 0;
  int current_err_key;
  size_t error_value_readed = 0;
  JsonRpcPacket &pkt;
};

static struct cbor_callbacks callbacks = cbor_empty_callbacks;
static std::once_flag callbacks_flag;

static void init_callbacks() {
  callbacks.uint8 = [](void* self, uint8_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.uint16 = [](void* self, uint16_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.uint32 = [](void* self, uint32_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.uint64 = [](void* self, uint64_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.negint8 = [](void* self, uint8_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(-1 - value);
  };
  callbacks.negint16 = [](void* self, uint16_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(-1 - value);
  };
  callbacks.negint32 = [](void* self, uint32_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(-1 - value);
  };
  callbacks.negint64 = [](void* self, uint64_t value) {
    static_cast<RpcPacketBuilder*>(self)->handleInteger(-1 - static_cast<int64_t>(value));
  };
  callbacks.byte_string = [](void* self, const cbor_data data, uint64_t len) {
    static_cast<RpcPacketBuilder*>(self)->handleBytes(data, len);
  };
  callbacks.array_start = [](void* self, uint64_t size) {
    static_cast<RpcPacketBuilder*>(self)->startArray(size);
  };
  callbacks.map_start = [](void* self, uint64_t size) {
    static_cast<RpcPacketBuilder*>(self)->startMap(size);
  };
  callbacks.boolean = [](void* self, bool value) {
    static_cast<RpcPacketBuilder*>(self)->handleBool(value);
  };
}

static cbor_decoder_status readJsonRpcPacket(cbor_data &cbuf, size_t &len, JsonRpcPacket &packet) {
  std::call_once(callbacks_flag, init_callbacks);
  RpcPacketBuilder builder { packet };
  auto cbufsave = cbuf;
  auto lensave = len;

  while (true) {
    // 基于callbacks，边读缓冲区边构造packet并进一步调用回调处理packet
    // 下面这个函数一次只读一个item
    auto decode_result = cbor_stream_decode(cbuf, len, &callbacks, &builder);

    if (decode_result.read != 0) {
      cbuf += decode_result.read;
      len -= decode_result.read;
    } else {
      // NEDATA or ERROR
      cbuf = cbufsave;
      len = lensave;
      return decode_result.status;
    }

    if (builder.state == RpcPacketBuilder::FIN) return CBOR_DECODER_FINISHED;
  }
}

RpcLua::RpcLua(asio::io_context &ctx) : io_ctx { ctx },
  child_stdin { ctx }, child_stdout { ctx }
{
  int stdin_pipe[2];  // [0]=read, [1]=write
  int stdout_pipe[2]; // [0]=read, [1]=write
  if (::pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1) {
    throw std::runtime_error("Failed to create pipes");
  }

  pid_t pid = fork();
  if (pid == 0) { // child
    // 关闭父进程用的 pipe 端
    ::close(stdin_pipe[1]);  // 关闭父进程的写入端（子进程只读 stdin）
    ::close(stdout_pipe[0]); // 关闭父进程的读取端（子进程只写 stdout）

    // 重定向 stdin/stdout
    ::dup2(stdin_pipe[0], STDIN_FILENO);   // 子进程的 stdin ← 父进程的写入端
    ::dup2(stdout_pipe[1], STDOUT_FILENO); // 子进程的 stdout → 父进程的读取端
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);

    sigset_t newmask, oldmask;
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGINT); // 阻塞 SIGINT
    sigprocmask(SIG_BLOCK, &newmask, &oldmask);

    if (int err = ::chdir("packages/freekill-core"); err != 0) {
      std::cout << "!" << std::endl;
      throw std::runtime_error(fmt::format("Cannot chdir into packages/freekill-core: {}\n\tYou must install freekill-core before starting the server.", strerror(errno)));
      // ::_exit(err);
    }

    auto disabled_packs = PackMan::instance().getDisabledPacks();
    auto json_string = json { disabled_packs }.dump();
    ::setenv("FK_DISABLED_PACKS", json_string.c_str(), 1);

    ::setenv("FK_RPC_MODE", "cbor", 1);
    ::execlp("lua5.4", "lua5.4", "lua/server/rpc/entry.lua", nullptr);

    ::_exit(EXIT_FAILURE);
  } else if (pid > 0) { // 父进程
    child_pid = pid;
    // 转下文
  } else {
    throw std::runtime_error("Failed to fork process");
  }

  // 关闭子进程用的 pipe 端
  close(stdin_pipe[0]);   // 关闭子进程的读取端（父进程只写 stdin）
  close(stdout_pipe[1]);  // 关闭子进程的写入端（父进程只读 stdout）

  child_stdin = { io_ctx, stdin_pipe[1] };
  child_stdout = { io_ctx, stdout_pipe[0] };

  wait(WaitForNotification, "hello", 0);
}

RpcLua::~RpcLua() {
  if (!alive()) return;

  call("bye");

  int wstatus;
  int w = waitpid(child_pid, &wstatus, WUNTRACED);
  if (w == -1) {
    spdlog::error("waitpid() error: {}", strerror(errno));
    return;
  }

  if (WIFEXITED(wstatus)) {
    spdlog::info("child process exited, status={}", WEXITSTATUS(wstatus));
  } else if (WIFSIGNALED(wstatus)) {
    spdlog::info("killed by signal {}", WTERMSIG(wstatus));
  } else if (WIFSTOPPED(wstatus)) {
    spdlog::info("stopped by signal {}", WSTOPSIG(wstatus));
  }
}

void RpcLua::wait(WaitType waitType, const char *method, int id) {
  JsonRpcPacket received_pkt;

  while (child_stdout.is_open() && alive()) {
    received_pkt.reset();
    boost::system::error_code ec;
    auto read_sz = child_stdout.read_some(asio::buffer(buffer, max_length), ec);
    if (ec) {
      spdlog::error("Error occured when reading child stdin: {}", ec.message());
      break;
    }

    cborBuffer.insert(cborBuffer.end(), buffer, buffer + read_sz);
    cbor_data cbuf = (cbor_data)cborBuffer.data(); size_t len = cborBuffer.size();

    auto stat = readJsonRpcPacket(cbuf, len, received_pkt);

    if (stat == CBOR_DECODER_ERROR) {
      cborBuffer.clear();
      break;
    } else {
      cborBuffer.clear();
      cborBuffer.insert(cborBuffer.end(), cbuf, cbuf + len);
      if (stat == CBOR_DECODER_NEDATA) continue;
    }

    if ((waitType == WaitForResponse && received_pkt.id == id && received_pkt.method == "" && received_pkt.error.code == 0) ||
      (waitType == WaitForNotification && received_pkt.id == -1 && received_pkt.method == method)) {
#ifdef RPC_DEBUG
      spdlog::debug("Me <-- returned {}", toHex({ buffer, read_sz }));
#endif
      // 并不关心lua返回了啥；那为什么还要去读取
      return;
    } else if (received_pkt.error.code != 0) {
      spdlog::warn("RPC call failed! id={} method={} ec={} msg={}", id, method, received_pkt.error.code, received_pkt.error.message);
      return;
    } else {
#ifdef RPC_DEBUG
      spdlog::debug("  Me <-- {} {}", received_pkt.method, toHex({ buffer, read_sz }));
#endif
      auto res = JsonRpc::handleRequest(RpcDispatchers::ServerRpcMethods, received_pkt);
      if (res) {
        if (res->error.code < 0) {
          sendError(child_stdin, *res);
#ifdef RPC_DEBUG
          spdlog::debug("  Me --> returned an error");
#endif
        } else if (res->id > 0) {
          sendResponse(child_stdin, *res);
#ifdef RPC_DEBUG
          spdlog::debug("  Me --> returned some value");
#endif
        } else {
          // 爆炸罢
          throw "unknown res type";
        }
      }
    }
  }

#ifdef RPC_DEBUG
  spdlog::debug("Me <-- IO read timeout. Is Lua process died?");
#endif
}

void RpcLua::call(const char *func_name, JsonRpcParam param1, JsonRpcParam param2, JsonRpcParam param3) {
#ifdef RPC_DEBUG
  spdlog::debug("L->call({})", func_name);
#endif

  if (!alive()) {
#ifdef RPC_DEBUG
    spdlog::debug("Me <-- <process died>");
#endif
    return;
  }

  auto req = JsonRpc::request(func_name, param1, param2, param3);
  auto id = req.id;
  sendRequest(child_stdin, req);

  wait(WaitForResponse, func_name, id);
}

std::string RpcLua::getConnectionInfo() const {
  auto ret = fmt::format("PID {}", child_pid);
  if (alive()) {
    std::ifstream f { fmt::format("/proc/{}/statm", child_pid) };
    if (f.is_open()) {
      std::string line;
      std::getline(f, line);

      std::istringstream iss(line);
      // 取splited[1]
      long rss_pages;
      iss >> rss_pages;
      iss >> rss_pages;

      long pageSize = sysconf(_SC_PAGESIZE);
      double mem_mib = (rss_pages * pageSize) / (1024.0 * 1024.0);

      ret += fmt::format(" (RSS = {:.2f} MiB)", mem_mib);
    } else {
      ret += " (unknown)";
    }
  } else {
    ret += " (died)";
  }

  return ret;
}

bool RpcLua::alive() const {
  auto procDir = fmt::format("/proc/{}/exe", child_pid);
  return std::filesystem::exists(procDir);
}
