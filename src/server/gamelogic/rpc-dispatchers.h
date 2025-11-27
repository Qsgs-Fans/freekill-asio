// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "server/rpc-lua/jsonrpc.h"
#include <nlohmann/json.hpp>

class Player;

namespace RpcDispatchers {

extern nlohmann::json getPlayerObject(Player &p);

extern const JsonRpc::RpcMethodMap ServerRpcMethods;

}
