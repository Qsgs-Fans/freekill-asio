// SPDX-License-Identifier: GPL-3.0-or-later

#include "network/router.h"

/*
Router::Router(QObject *parent, ClientSocket *socket, RouterType type)
    : QObject(parent) {
  this->type = type;
  this->socket = nullptr;
  setSocket(socket);
  expectedReplyId = -1;
  replyTimeout = 0;
  extraReplyReadySemaphore = nullptr;
}

Router::~Router() { abortRequest(); }

ClientSocket *Router::getSocket() const { return socket; }

void Router::setSocket(ClientSocket *socket) {
  if (this->socket != nullptr) {
    this->socket->disconnect(this);
    disconnect(this->socket);
    this->socket->deleteLater();
  }

  this->socket = nullptr;
  if (socket != nullptr) {
    connect(this, &Router::messageReady, socket, &ClientSocket::send);
    connect(socket, &ClientSocket::message_got, this, &Router::handlePacket);
    connect(socket, &ClientSocket::disconnected, this, &Router::abortRequest);
    socket->setParent(this);
    this->socket = socket;
  }
}

void Router::removeSocket() {
  socket->disconnect(this);
  socket = nullptr;
}

bool Router::isConsoleStart() const {
  return socket->peerAddress() == "127.0.0.1";
}

void Router::setReplyReadySemaphore(QSemaphore *semaphore) {
  extraReplyReadySemaphore = semaphore;
}

void Router::request(int type, const QByteArray &command, const QByteArray &cborData,
                     int timeout, qint64 timestamp) {
  // In case a request is called without a following waitForReply call
  if (replyReadySemaphore.available() > 0)
    replyReadySemaphore.acquire(replyReadySemaphore.available());

  static int requestId = 0;
  requestId++;
  if (requestId > 10000000) requestId = 1;

  replyMutex.lock();
  expectedReplyId = requestId;
  replyTimeout = timeout;
  requestStartTime = QDateTime::currentDateTime();
  m_reply = QStringLiteral("__notready");
  replyMutex.unlock();

  QCborArray body {
    requestId,
    type,
    command,
    cborData,
    timeout,
    (timestamp <= 0 ? requestStartTime.toMSecsSinceEpoch() : timestamp)
  };

  sendMessage(body.toCborValue().toCbor());
}

void Router::reply(int type, const QByteArray &command, const QByteArray &cborData) {
  QCborArray body {
    this->requestId,
    type,
    command,
    cborData,
  };

  sendMessage(body.toCborValue().toCbor());
}

void Router::notify(int type, const QByteArray &command, const QByteArray &cborData) {
  QCborArray body {
    -2,
    type,
    command,
    cborData,
  };

  sendMessage(body.toCborValue().toCbor());
}

int Router::getTimeout() const { return requestTimeout; }

// cancel last request from the sender
void Router::cancelRequest() {
  replyMutex.lock();
  expectedReplyId = -1;
  replyTimeout = 0;
  extraReplyReadySemaphore = nullptr;
  replyMutex.unlock();

  if (replyReadySemaphore.available() > 0)
    replyReadySemaphore.acquire(replyReadySemaphore.available());
}

QString Router::waitForReply(int timeout) {
  QString ret;
  replyReadySemaphore.tryAcquire(1, timeout * 1000);
  replyMutex.lock();
  ret = m_reply;
  replyMutex.unlock();
  return ret;
}

void Router::abortRequest() {
  replyMutex.lock();
  if (expectedReplyId != -1) {
    replyReadySemaphore.release();
    if (extraReplyReadySemaphore)
      extraReplyReadySemaphore->release();
    expectedReplyId = -1;
    extraReplyReadySemaphore = nullptr;
  }
  replyMutex.unlock();
}

void Router::handlePacket(const QCborArray &packet) {
  int requestId = packet[0].toInteger();
  int type = packet[1].toInteger();
  auto command = packet[2].toByteArray();
  auto cborData = packet[3].toByteArray();

  if (type & TYPE_NOTIFICATION) {
    emit notification_got(command, cborData);
  } else if (type & TYPE_REQUEST) {
    this->requestId = requestId;
    this->requestTimeout = packet[4].toInteger();
    this->requestTimestamp = packet[5].toInteger();

    emit request_got(command, cborData);
  } else if (type & TYPE_REPLY) {
    QMutexLocker locker(&replyMutex);

    if (requestId != this->expectedReplyId)
      return;

    this->expectedReplyId = -1;

    if (replyTimeout >= 0 &&
      replyTimeout < requestStartTime.secsTo(QDateTime::currentDateTime()))
      return;

    m_reply = cborData;
    // TODO: callback?
    replyReadySemaphore.release();
    if (extraReplyReadySemaphore) {
      extraReplyReadySemaphore->release();
      extraReplyReadySemaphore = nullptr;
    }

    locker.unlock();
    emit replyReady();
  }
}

void Router::sendMessage(const QByteArray &msg) {
  auto connType = qApp->thread() == QThread::currentThread()
    ? Qt::DirectConnection : Qt::BlockingQueuedConnection;
  QMetaObject::invokeMethod(qApp, [&]() { emit messageReady(msg); }, connType);
}
*/
