#include "protocol/Protocol.h"

#include <cmath>

namespace dbdeck {

Request parseRequest(std::string_view line) {
    if (line.size() > kMaxMessageBytes) throw ProtocolError("bad_request", "message too large");
    json j;
    try {
        j = json::parse(line);
    } catch (const json::exception&) {
        throw ProtocolError("bad_request", "malformed JSON");
    }
    if (!j.is_object()) throw ProtocolError("bad_request", "request must be an object");
    Request r;
    auto id = j.find("id");
    if (id == j.end() || !id->is_number_integer()) throw ProtocolError("bad_request", "missing integer 'id'");
    r.id = id->get<int64_t>();
    auto cmd = j.find("cmd");
    if (cmd == j.end() || !cmd->is_string()) throw ProtocolError("bad_request", "missing string 'cmd'");
    r.cmd = cmd->get<std::string>();
    if (r.cmd.empty() || r.cmd.size() > 64) throw ProtocolError("bad_request", "invalid 'cmd'");
    auto args = j.find("args");
    if (args == j.end() || args->is_null()) r.args = json::object();
    else if (!args->is_object()) throw ProtocolError("bad_request", "'args' must be an object");
    else r.args = *args;
    return r;
}

json okResponse(int64_t id, json result) { return {{"id", id}, {"ok", true}, {"result", std::move(result)}}; }

json errorResponse(int64_t id, const std::string& code, const std::string& message) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

json eventMessage(const std::string& name, json data) { return {{"event", name}, {"data", std::move(data)}}; }

static const json* find(const json& args, const char* key) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return nullptr;
    return &*it;
}

std::optional<std::string> optString(const json& args, const char* key) {
    const json* v = find(args, key);
    if (!v) return std::nullopt;
    if (!v->is_string()) throw ProtocolError("bad_args", std::string("'") + key + "' must be a string");
    std::string s = v->get<std::string>();
    if (s.size() > 4096) throw ProtocolError("bad_args", std::string("'") + key + "' is too long");
    return s;
}

std::string argString(const json& args, const char* key) {
    auto v = optString(args, key);
    if (!v) throw ProtocolError("bad_args", std::string("missing '") + key + "'");
    return *v;
}

std::optional<float> optFloat(const json& args, const char* key, float lo, float hi) {
    const json* v = find(args, key);
    if (!v) return std::nullopt;
    if (!v->is_number()) throw ProtocolError("bad_args", std::string("'") + key + "' must be a number");
    const double d = v->get<double>();
    if (!std::isfinite(d) || d < lo || d > hi)
        throw ProtocolError("bad_args", std::string("'") + key + "' out of range [" + std::to_string(lo) + ", " +
                                            std::to_string(hi) + "]");
    return static_cast<float>(d);
}

float argFloat(const json& args, const char* key, float lo, float hi) {
    auto v = optFloat(args, key, lo, hi);
    if (!v) throw ProtocolError("bad_args", std::string("missing '") + key + "'");
    return *v;
}

std::optional<bool> optBool(const json& args, const char* key) {
    const json* v = find(args, key);
    if (!v) return std::nullopt;
    if (!v->is_boolean()) throw ProtocolError("bad_args", std::string("'") + key + "' must be a boolean");
    return v->get<bool>();
}

bool argBool(const json& args, const char* key) {
    auto v = optBool(args, key);
    if (!v) throw ProtocolError("bad_args", std::string("missing '") + key + "'");
    return *v;
}

const json& argObject(const json& args, const char* key) {
    const json* v = find(args, key);
    if (!v || !v->is_object()) throw ProtocolError("bad_args", std::string("'") + key + "' must be an object");
    return *v;
}

}  // namespace dbdeck
