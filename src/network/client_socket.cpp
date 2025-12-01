// SPDX-License-Identifier: GPL-3.0-or-later

#include "network/client_socket.h"

#include <openssl/aes.h>

namespace asio = boost::asio;
using asio::awaitable;
using asio::detached;
using asio::use_awaitable;
using asio::redirect_error;

ClientSocket::ClientSocket(tcp::socket &&socket) : m_socket(std::move(socket)) {
  m_peer_address = m_socket.remote_endpoint().address().to_string();
  disconnected_callback = [this] {
    spdlog::info("client {} disconnected", peerAddress());
  };
  message_got_callback = [](Packet &p){ p.describe(); };
}

void ClientSocket::start() {
  asio::co_spawn(m_socket.get_executor(), reader(), detached);
}

awaitable<void> ClientSocket::reader() {
  // 下面那个死循环的持续周期可能比this的生命周期长，所以需要挂个检测
  auto weak { weak_from_this() };

  for (;;) {
    boost::system::error_code ec;
    auto length = co_await m_socket.async_read_some(
      asio::buffer(m_data, max_length), redirect_error(use_awaitable, ec));

    if (ec) break;

    auto self = weak.lock();
    if (!self) break;

    auto stat = self->handleBuffer(length);
    if (stat == CBOR_DECODER_ERROR) {
      spdlog::warn("Malformed data from client {}", self->peerAddress());
      break;
    }
  }

  if (auto self = weak.lock(); self) {
    self->disconnected_callback();

    self->set_message_got_callback([](Packet &){});
    self->set_disconnected_callback([]{});
  }
}

asio::ip::tcp::socket &ClientSocket::socket() {
  return m_socket;
}

std::string_view ClientSocket::peerAddress() const {
  return m_peer_address;
}

void ClientSocket::disconnectFromHost() {
  try {
    m_socket.shutdown(tcp::socket::shutdown_both);
    m_socket.close();
  } catch (std::exception &) {
    // ignore
  }
  disconnected_callback();

  // 连接建立阶段绑的callback中可能拷贝了自身的shared
  set_message_got_callback([](Packet &){});
  set_disconnected_callback([]{});
}

void ClientSocket::send(const std::shared_ptr<std::string> msg) {
  asio::async_write(
    m_socket,
    asio::const_buffer { msg->data(), msg->size() },
    detached
  );
}

void ClientSocket::set_disconnected_callback(std::function<void()> f) {
  disconnected_callback = f;
}

void ClientSocket::set_message_got_callback(std::function<void(Packet &)> f) {
  message_got_callback = f;
}

// private methods

void Packet::describe() {
  spdlog::info("Item data: len={} reqId={} type={} command={} data={} bytes", _len, requestId, type, command, cborData.size());
  cbor_load_result sz;
  auto dat = cbor_load((cbor_data)cborData.data(), cborData.size(), &sz);
  cbor_describe(dat, stdout);
  cbor_decref(&dat);
}

struct PacketBuilder {
  explicit PacketBuilder(Packet &p, auto &callback) : pkt { p }, message_got_callback { callback } {
    reset();
  }

  void handleInteger(int64_t value) {
    if (!valid_packet) return;

    switch (current_field) {
      case 0: pkt.requestId = static_cast<int>(value); break;
      case 1: pkt.type = static_cast<int>(value); break;
      case 4: pkt.timeout = static_cast<int>(value); break;
      case 5: pkt.timestamp = value; break;
      default:
        valid_packet = false;
        return;
    }

    nextField();
  }

  void handleBytes(const cbor_data data, size_t len) {
    if (!valid_packet) return;
    std::string_view sv { (char *)data, len };

    switch (current_field) {
      case 2:
        pkt.command = sv;
        break;
      case 3:
        pkt.cborData = sv;
        break;
      default:
        valid_packet = false;
        return;
    }

    nextField();
  }

  void startArray(size_t size) {
    pkt._len = size;
    valid_packet = true;
    if (size != 4 && size != 6) {
      valid_packet = false;
    }
  }

  void reset() {
    pkt.type = 0;
    pkt._len = 0;
    pkt.command = "";
    pkt.cborData = "";
    current_field = 0;
    valid_packet = false;
  }

  void nextField() {
    current_field++;
    if (current_field == pkt._len) {
      message_got_callback(pkt);
      handled++;
      reset();
    }
  }

  Packet &pkt;
  std::function<void(Packet &)> &message_got_callback;
  int current_field = 0;
  bool valid_packet = false;
  int handled = 0;
};

static struct cbor_callbacks callbacks = cbor_empty_callbacks;
static std::once_flag callbacks_flag;

static void init_callbacks() {
  callbacks.uint8 = [](void* self, uint8_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.uint16 = [](void* self, uint16_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.uint32 = [](void* self, uint32_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.uint64 = [](void* self, uint64_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(value);
  };
  callbacks.negint8 = [](void* self, uint8_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(-1 - value);
  };
  callbacks.negint16 = [](void* self, uint16_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(-1 - value);
  };
  callbacks.negint32 = [](void* self, uint32_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(-1 - value);
  };
  callbacks.negint64 = [](void* self, uint64_t value) {
    static_cast<PacketBuilder*>(self)->handleInteger(-1 - static_cast<int64_t>(value));
  };
  callbacks.byte_string = [](void* self, const cbor_data data, uint64_t len) {
    static_cast<PacketBuilder*>(self)->handleBytes(data, len);
  };
  callbacks.array_start = [](void* self, uint64_t size) {
    static_cast<PacketBuilder*>(self)->startArray(size);
  };
}

cbor_decoder_status ClientSocket::handleBuffer(size_t length) {
  cborBuffer.insert(cborBuffer.end(), m_data, m_data + length);

  auto cbuf = (unsigned char *)cborBuffer.data();
  auto len = cborBuffer.size();
  size_t total_consumed = 0;

  size_t real_consumed = 0;
  // spdlog::debug("client socket buffer: {}", std::string_view{ (char*)cborBuffer.data(), cborBuffer.size() });

  std::call_once(callbacks_flag, init_callbacks);

  struct cbor_decoder_result decode_result;
  Packet pkt;
  PacketBuilder builder { pkt, message_got_callback };
  int handled = 0;

  cbor_decoder_status lastStat;

  while (true) {
    // 基于callbacks，边读缓冲区边构造packet并进一步调用回调处理packet
    // 下面这个函数一次只读一个item
    decode_result = cbor_stream_decode(cbuf, len, &callbacks, &builder);
    lastStat = decode_result.status;
    if (decode_result.status == CBOR_DECODER_ERROR) {
      return lastStat;
    } else if (decode_result.status == CBOR_DECODER_NEDATA) {
      break;
    }

    if (decode_result.read != 0) {
      cbuf += decode_result.read;
      len -= decode_result.read;
      total_consumed += decode_result.read;
    } else {
      break;
    }

    if (builder.handled != handled) {
      real_consumed = total_consumed;
    }
  }

  if (builder.handled == 0 && !builder.valid_packet) {
    return lastStat;
  }

  // 对剩余的不全数据深拷贝 重新造bytes
  if (real_consumed < len) {
    std::vector<unsigned char> remaining_buffer;
    remaining_buffer.assign(cborBuffer.begin() + real_consumed, cborBuffer.end());
    cborBuffer = remaining_buffer;
  } else {
    cborBuffer.clear();
  }

  return lastStat;
}


/*
ClientSocket::ClientSocket(QTcpSocket *socket) {
  aes_ready = false;
  socket->setParent(this);
  this->socket = socket;
  timerSignup.setSingleShot(true);
  connect(&timerSignup, &QTimer::timeout, this,
          &ClientSocket::disconnectFromHost);
  connect(&timerSignup, &QTimer::timeout, this, &QObject::deleteLater);
  init();
}

void ClientSocket::installAESKey(const QByteArray &key) {
  if (key.length() != 32) {
    return;
  }
  auto key_ = QByteArray::fromHex(key);
  if (key_.length() != 16) {
    return;
  }

  AES_set_encrypt_key((const unsigned char *)key_.data(), 16 * 8, &aes_key);
  aes_ready = true;
}

void ClientSocket::removeAESKey() {
  aes_ready = false;
}

QByteArray ClientSocket::aesEnc(const QByteArray &in) {
  if (!aes_ready) {
    return in;
  }
  int num = 0;
  QByteArray out;
  out.resize(in.length());

  static auto rand_generator = QRandomGenerator::securelySeeded();
  static QByteArray iv_raw(16, Qt::Uninitialized);

  rand_generator.fillRange(reinterpret_cast<quint32*>(iv_raw.data()), 4);

  static unsigned char tempIv[16];
  strncpy((char *)tempIv, iv_raw.constData(), 16);
  AES_cfb128_encrypt((const unsigned char *)in.constData(),
                     (unsigned char *)out.data(), in.length(), &aes_key, tempIv,
                     &num, AES_ENCRYPT);

  return iv_raw.toHex() + out.toBase64();
}
QByteArray ClientSocket::aesDec(const QByteArray &in) {
  if (!aes_ready) {
    return in;
  }

  int num = 0;
  auto iv = in.first(32);
  auto aes_iv = QByteArray::fromHex(iv);

  auto real_in = in;
  real_in.remove(0, 32);
  auto inenc = QByteArray::fromBase64(real_in);
  QByteArray out;
  out.resize(inenc.length());
  unsigned char tempIv[16];
  strncpy((char *)tempIv, aes_iv.constData(), 16);
  AES_cfb128_encrypt((const unsigned char *)inenc.constData(),
                     (unsigned char *)out.data(), inenc.length(), &aes_key,
                     tempIv, &num, AES_DECRYPT);

  return out;
}
*/
