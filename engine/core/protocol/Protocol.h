#pragma once
// dB-Deck engine IPC protocol, version 1.
//
// Transport: a per-user local named pipe carrying UTF-8 JSON, one message per
// line ('\n'). Messages are limited to kMaxMessageBytes.
//
//   request : {"id": <int>, "cmd": "<name>", "args": {...}}
//   response: {"id": <int>, "ok": true,  "result": ...}
//             {"id": <int>, "ok": false, "error": {"code": "...", "message": "..."}}
//   event   : {"event": "<name>", "data": ...}
//
// The first request on a connection must be "hello" with
// {"protocol": 1}. See shared/protocol/PROTOCOL.md for the command list.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace dbdeck {

using json = nlohmann::json;

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxMessageBytes = 1 << 20;

struct ProtocolError : std::runtime_error {
    ProtocolError(std::string c, const std::string& msg) : std::runtime_error(msg), code(std::move(c)) {}
    std::string code;  // "bad_request" | "bad_args" | "not_found" | "rejected" | "unsupported" | "internal"
};

struct Request {
    int64_t id = 0;
    std::string cmd;
    json args;
};

Request parseRequest(std::string_view line);

json okResponse(int64_t id, json result);
json errorResponse(int64_t id, const std::string& code, const std::string& message);
json eventMessage(const std::string& name, json data);

// Typed argument accessors; throw ProtocolError("bad_args") on type errors.
std::string argString(const json& args, const char* key);
std::optional<std::string> optString(const json& args, const char* key);
float argFloat(const json& args, const char* key, float lo, float hi);
std::optional<float> optFloat(const json& args, const char* key, float lo, float hi);
bool argBool(const json& args, const char* key);
std::optional<bool> optBool(const json& args, const char* key);
const json& argObject(const json& args, const char* key);

}  // namespace dbdeck
