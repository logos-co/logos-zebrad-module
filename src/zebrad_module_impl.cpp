#include "zebrad_module_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <logos_caller.h>

#include "zebrad_c.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* const kDefaultCaller = "zcash_wallet_core_module";

// regtest.json (the wallet core's keys) -> Zebra's [network.testnet_parameters.activation_heights].
const std::pair<const char*, const char*> kUpgradeKeys[] = {
    {"overwinter", "Overwinter"}, {"sapling", "Sapling"}, {"blossom", "Blossom"},
    {"heartwood", "Heartwood"}, {"canopy", "Canopy"}, {"nu5", "NU5"}, {"nu6", "NU6"},
    {"nu6_1", "\"NU6.1\""}, {"nu6_2", "\"NU6.2\""}, {"nu6_3", "\"NU6.3\""}, {"nu7", "NU7"},
};

std::string takeString(const char* s) {
    std::string out = s ? s : "";
    ZEBRAD_free(s);
    return out;
}

StdLogosResult fail(const std::string& why) { return {false, {}, why}; }

std::string refusal(const std::string& why) { return json{{"ok", false}, {"error", why}}.dump(); }

void appendLine(const std::string& path, const std::string& line) {
    std::ofstream f(path, std::ios::app);
    if (f) f << line << '\n';
}

const char* stateName(int state) {
    switch (state) {
        case ZEBRAD_STOPPED:  return "stopped";
        case ZEBRAD_STARTING: return "starting";
        case ZEBRAD_RUNNING:  return "running";
        case ZEBRAD_STOPPING: return "stopping";
        default:              return "failed";
    }
}

// A TOML basic string, so a setting can never end its value and start a table.
std::string tomlString(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (i + 1 < len ? uint32_t(data[i + 1]) << 8 : 0)
                         | (i + 2 < len ? uint32_t(data[i + 2]) : 0);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < len ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < len ? kB64[n & 63] : '=';
    }
    return out;
}

// Standard alphabet; padding optional, anything else refused.
bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    size_t end = in.size();
    while (end > 0 && in[end - 1] == '=' && in.size() - end < 2) --end;
    if (end % 4 == 1) return false;
    out.clear();
    out.reserve(end / 4 * 3 + 2);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < end; ++i) {
        const char* p = std::strchr(kB64, in[i]);
        if (!p || !*p) return false;
        acc = (acc << 6) | uint32_t(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xff));
        }
    }
    return true;
}

}  // namespace

// Started in the body: the thread uses members declared after it.
ZebradModuleImpl::ZebradModuleImpl() { m_pollThread = std::thread([this] { pollLoop(); }); }

ZebradModuleImpl::~ZebradModuleImpl() {
    { std::lock_guard<std::mutex> lock(m_pollMutex); m_pollStop = true; }
    m_pollWake.notify_all();
    if (m_pollThread.joinable()) m_pollThread.join();
    if (m_stopThread.joinable()) m_stopThread.join();
    ZEBRAD_stop();
}

void ZebradModuleImpl::onContextReady() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_persistDir = instancePersistencePath();
    if (m_persistDir.empty()) return;
    std::error_code ec;
    fs::create_directories(m_persistDir, ec);
    // One file for every run: the library installs its log writer once per process.
    m_logFile = (fs::path(m_persistDir) / "zebrad.log").string();
    std::ifstream f(fs::path(m_persistDir) / "zebrad.json");
    if (f) {
        try { m_config = json::parse(f); } catch (...) { m_config = json::object(); }
        if (!m_config.is_object()) m_config = json::object();
    }
    loadCallers();
    loadRegtest();
}

// Absent file: the wallet core alone. Unreadable file: nobody, so a typo never opens the door.
void ZebradModuleImpl::loadCallers() {
    m_callerModules = {kDefaultCaller};
    m_allowHost = false;
    std::ifstream f(fs::path(m_persistDir) / "callers.json");
    if (!f) return;
    m_callerModules.clear();
    const json c = json::parse(f, nullptr, false);
    if (!c.is_object()) return;
    std::vector<std::string> modules;
    bool allowHost = false;
    for (auto& [k, v] : c.items()) {
        if (k == "modules" && v.is_array()) {
            for (const json& m : v) {
                if (!m.is_string()) return;
                modules.push_back(m.get<std::string>());
            }
        } else if (k == "allowHost" && v.is_boolean()) {
            allowHost = v.get<bool>();
        } else {
            return;
        }
    }
    m_callerModules = std::move(modules);
    m_allowHost = allowHost;
}

void ZebradModuleImpl::loadRegtest() {
    m_regtestHeights.clear();
    m_regtestError.clear();
    const fs::path path = fs::path(m_persistDir) / "regtest.json";
    std::error_code ec;
    m_regtest = fs::exists(path, ec);
    if (!m_regtest) return;
    std::ifstream f(path);
    const json h = json::parse(f, nullptr, false);
    if (!h.is_object()) { m_regtestError = "regtest.json is not a JSON object"; return; }
    for (auto it = h.begin(); it != h.end(); ++it) {
        const char* zebraKey = nullptr;
        for (const auto& [ours, theirs] : kUpgradeKeys)
            if (it.key() == ours) zebraKey = theirs;
        if (!zebraKey) { m_regtestError = "regtest.json: unknown upgrade " + it.key(); break; }
        if (it->is_null()) continue;
        if (!it->is_number_unsigned() || it->get<uint64_t>() > UINT32_MAX) {
            m_regtestError = "regtest.json: " + it.key() + " must be a height or null";
            break;
        }
        m_regtestHeights.emplace_back(zebraKey, it->get<uint32_t>());
    }
}

bool ZebradModuleImpl::knownNetwork(const std::string& network) const {
    return network == "mainnet" || network == "testnet" || (network == "regtest" && m_regtest);
}

LogosMap ZebradModuleImpl::defaultConfig(const std::string& network) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return defaultsFor(network);
}

json ZebradModuleImpl::defaultsFor(const std::string& network) const {
    if (!knownNetwork(network)) return json::object();
    // Zebra listens on every interface by default; a regtest chain has no one to listen to.
    const char* listen = network == "mainnet" ? "[::]:8233"
                       : network == "testnet" ? "[::]:18233" : "127.0.0.1:18344";
    json d{
        {"cacheDir", m_persistDir.empty() ? "" : (fs::path(m_persistDir) / network).string()},
        {"peersetInitialTargetSize", 25},
        {"listenAddr", listen},
        {"exposeLightwalletd", ""},
        {"exposeJsonRpc", ""},
        {"logFilter", "info"},
    };
    // Where `generate` pays its coinbase; mainnet and testnet are mined by others.
    if (network == "regtest") d["minerAddress"] = "";
    return d;
}

// A stored value of the wrong type (a hand edit) falls back to the default.
json ZebradModuleImpl::settingsFor(const std::string& network) {
    json s = defaultsFor(network);
    if (m_config.contains(network) && m_config[network].is_object())
        for (auto& [k, v] : m_config[network].items())
            if (s.contains(k) && (s[k].is_number() ? v.is_number_integer() : v.is_string())) s[k] = v;
    return s;
}

LogosMap ZebradModuleImpl::getConfig(const std::string& network) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return settingsFor(network);
}

StdLogosResult ZebradModuleImpl::configure(const std::string& network, const LogosMap& config) {
    if (!config.is_object()) return fail("config must be an object");
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!knownNetwork(network)) return fail("unknown network: " + network);
    const json defaults = defaultsFor(network);
    for (auto& [k, v] : config.items()) {
        if (k == "minerAddress" && network != "regtest") return fail("minerAddress is for regtest only");
        if (!defaults.contains(k)) return fail("unknown setting: " + k);
        // nlohmann types a parsed 25 as unsigned and a negative as signed.
        const bool ok = defaults[k].is_number() ? v.is_number_integer() : v.is_string();
        if (!ok) return fail("wrong type for " + k);
        if (k == "peersetInitialTargetSize" && v.get<int64_t>() < 1)
            return fail("peersetInitialTargetSize must be at least 1");
        if ((k == "cacheDir" || k == "listenAddr" || k == "logFilter") && v.get<std::string>().empty())
            return fail(k + " must not be empty");
    }
    json& stored = m_config[network];
    if (!stored.is_object()) stored = json::object();
    for (auto& [k, v] : config.items()) stored[k] = v;
    persist();
    return {true, settingsFor(network)};
}

void ZebradModuleImpl::persist() {
    if (m_persistDir.empty()) return;
    const fs::path path = fs::path(m_persistDir) / "zebrad.json";
    const fs::path tmp = path.string() + ".tmp";
    { std::ofstream f(tmp); f << m_config.dump(2); }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

std::string ZebradModuleImpl::zebradToml(const std::string& network, const json& s) const {
    const std::string cacheDir = s["cacheDir"].get<std::string>();
    const std::string lwd = s["exposeLightwalletd"].get<std::string>();
    const std::string rpc = s["exposeJsonRpc"].get<std::string>();
    std::string t = "[network]\n";
    t += "network = " + tomlString(network == "mainnet" ? "Mainnet" : network == "testnet" ? "Testnet" : "Regtest") + "\n";
    t += "listen_addr = " + tomlString(s["listenAddr"].get<std::string>()) + "\n";
    t += "cache_dir = " + tomlString(cacheDir) + "\n";
    t += "peerset_initial_target_size = " + std::to_string(s["peersetInitialTargetSize"].get<int64_t>()) + "\n";
    if (network == "regtest") {
        t += "\n[network.testnet_parameters.activation_heights]\n";
        for (const auto& [key, height] : m_regtestHeights) t += key + " = " + std::to_string(height) + "\n";
    }
    t += "\n[state]\ncache_dir = " + tomlString(cacheDir) + "\n";
    if (network == "regtest" && !s["minerAddress"].get<std::string>().empty())
        t += "\n[mining]\nminer_address = " + tomlString(s["minerAddress"].get<std::string>()) + "\n";
    if (!lwd.empty() || !rpc.empty()) {
        // The cookie stays beside the chain rather than in the user's cache directory.
        t += "\n[rpc]\n";
        if (!rpc.empty()) t += "listen_addr = " + tomlString(rpc) + "\n";
        if (!lwd.empty()) t += "lightwalletd_listen_addr = " + tomlString(lwd) + "\n";
        t += "enable_cookie_auth = true\ncookie_dir = " + tomlString(cacheDir) + "\n";
    }
    return t;
}

StdLogosResult ZebradModuleImpl::start(const std::string& network) {
    if (m_unloading) return fail("unloading");
    json options;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!knownNetwork(network)) return fail("unknown network: " + network);
        if (m_persistDir.empty()) return fail("no persistence path yet");
        if (network == "regtest" && !m_regtestError.empty()) return fail(m_regtestError);
        const json s = settingsFor(network);
        std::error_code ec;
        fs::create_directories(s["cacheDir"].get<std::string>(), ec);
        if (ec) return fail("cacheDir: " + ec.message());
        const bool expose = !s["exposeLightwalletd"].get<std::string>().empty()
                         || !s["exposeJsonRpc"].get<std::string>().empty();
        options = {{"config", zebradToml(network, s)}, {"logFile", m_logFile},
                   {"logFilter", s["logFilter"]}, {"exposeRpc", expose}};
    }
    if (ZEBRAD_start(options.dump().c_str()) != 0) {
        const std::string why = takeString(ZEBRAD_last_error());
        refresh();
        return fail(why);
    }
    refresh();
    return {true, status()};
}

StdLogosResult ZebradModuleImpl::stop() {
    const int st = ZEBRAD_state();
    if (st == ZEBRAD_RUNNING || st == ZEBRAD_STARTING) refresh("stopping");
    ZEBRAD_stop();
    refresh();
    return {true, status()};
}

// Async: a stop can take Zebra's full 20 s shutdown budget, past the host's 3 s grace.
// The thread is a member and joined in the destructor, so it cannot outlive us.
LogosShutdown ZebradModuleImpl::aboutToUnload() {
    m_unloading = true;
    if (m_stopThread.joinable()) return LogosShutdown::Asynchronous;
    const int st = ZEBRAD_state();
    if (st == ZEBRAD_STOPPED || st == ZEBRAD_FAILED) {
        unloadLog("aboutToUnload: node not running");
        return LogosShutdown::Synchronous;
    }
    unloadLog("aboutToUnload: stopping node");
    m_stopThread = std::thread([this] {
        const auto t0 = std::chrono::steady_clock::now();
        ZEBRAD_stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        unloadLog("aboutToUnload: node stopped in " + std::to_string(ms) + " ms; "
                  + takeString(ZEBRAD_status_json()));
        unloadFinished();
    });
    return LogosShutdown::Asynchronous;
}

// A file, not stderr: the host closes our stdio before it unloads us.
void ZebradModuleImpl::unloadLog(const std::string& line) {
    std::string dir;
    { std::lock_guard<std::mutex> lock(m_mutex); dir = m_persistDir; }
    if (!dir.empty()) appendLine((fs::path(dir) / "unload.log").string(), line);
}

void ZebradModuleImpl::pollLoop() {
    std::unique_lock<std::mutex> lock(m_pollMutex);
    while (!m_pollStop) {
        lock.unlock();
        refresh();
        lock.lock();
        m_pollWake.wait_for(lock, std::chrono::seconds(1), [this] { return m_pollStop.load(); });
    }
}

// Samples the library; emits when the state moved. `forceState` announces a stop before it blocks.
void ZebradModuleImpl::refresh(const char* forceState) {
    std::lock_guard<std::mutex> emitLock(m_emitMutex);
    const std::string raw = takeString(ZEBRAD_status_json());
    json st = json::parse(raw, nullptr, false);
    if (!st.is_object() || !st.contains("state") || !st["state"].is_string())
        st = json{{"state", stateName(ZEBRAD_state())}, {"lastError", "unreadable status: " + raw}};
    const auto now = std::chrono::steady_clock::now();
    if (forceState) st["state"] = forceState;
    else {
        std::lock_guard<std::mutex> lock(m_sampleMutex);
        m_sample = st;
        m_sampleAt = now;
    }
    const std::string state = st.value("state", "");
    if (state == m_emittedState || m_unloading) return;
    m_emittedState = state;
    zebradStateChanged(decorate(std::move(st), now, true).dump());
}

json ZebradModuleImpl::decorate(json st, std::chrono::steady_clock::time_point at, bool sampled) {
    st["chainAgeSecs"] = sampled ? std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - at).count() : -1;
    const auto num = [&st](const char* k) {
        return st.contains(k) && st[k].is_number() ? st[k].get<double>() : -1.0;
    };
    const double height = num("height"), estimated = num("estimatedHeight");
    // Regtest blocks carry 2011 timestamps, so Zebra's estimate there is millions of blocks
    // ahead; a regtest chain has no network to catch up with.
    const bool regtest = st.contains("network") && st["network"] == "Regtest";
    st["syncPercent"] = height < 0 ? -1.0 : regtest ? 100.0
        : estimated > 0 ? std::min(100.0, std::floor(height * 10000.0 / estimated) / 100.0) : -1.0;
    return st;
}

LogosMap ZebradModuleImpl::status() {
    json st;
    std::chrono::steady_clock::time_point at;
    {
        std::lock_guard<std::mutex> lock(m_sampleMutex);
        st = m_sample;
        at = m_sampleAt;
    }
    const bool sampled = st.is_object();
    if (!sampled) st = json::object();
    // An atomic read: the state is never a second stale, even when the rest is.
    st["state"] = stateName(ZEBRAD_state());
    return decorate(std::move(st), at, sampled);
}

bool ZebradModuleImpl::admitted() {
    const logos::LogosCaller caller = logos::currentCaller();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (caller.isHost()) return m_allowHost;
    if (!caller.isModule() || caller.name.empty()) return false;
    return std::find(m_callerModules.begin(), m_callerModules.end(), caller.name) != m_callerModules.end();
}

std::string ZebradModuleImpl::grpc(const std::string& path, const std::string& bodyBase64) {
    if (!admitted()) return refusal("not authorized");
    if (m_unloading) return refusal("unloading");
    if (path.size() < 2 || path[0] != '/') return refusal("path must be a full gRPC path");
    std::vector<uint8_t> body;
    if (!base64Decode(bodyBase64, body)) return refusal("body is not base64");
    static const uint8_t kEmpty = 0;
    uint8_t* out = nullptr;
    size_t outLen = 0;
    const int status = ZEBRAD_grpc(path.c_str(), body.empty() ? &kEmpty : body.data(), body.size(), &out, &outLen);
    const std::string message = status == 0 ? "" : takeString(ZEBRAD_last_error());
    std::string encoded = out ? base64Encode(out, outLen) : "";
    if (out) ZEBRAD_free_bytes(out, outLen);
    return json{{"ok", true}, {"status", status}, {"message", message}, {"body", std::move(encoded)}}.dump();
}

// Reads backwards from the end, so a long-running node's log costs only the tail.
std::string ZebradModuleImpl::logTail(int64_t lines) {
    std::string path;
    { std::lock_guard<std::mutex> lock(m_mutex); path = m_logFile; }
    if (path.empty() || lines <= 0) return "";
    lines = std::min<int64_t>(lines, 10000);
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    std::string tail;
    std::streamoff pos = size;
    int64_t newlines = 0;
    while (pos > 0) {
        const std::streamoff chunk = std::min<std::streamoff>(pos, 64 * 1024);
        pos -= chunk;
        std::string buf(static_cast<size_t>(chunk), '\0');
        f.seekg(pos);
        f.read(buf.data(), chunk);
        tail.insert(0, buf);
        // The last line's own newline does not start another line.
        newlines = std::count(tail.begin(), tail.end(), '\n') - (!tail.empty() && tail.back() == '\n');
        if (newlines >= lines) break;
    }
    size_t start = 0;
    for (int64_t skip = newlines - lines; skip >= 0 && start < tail.size(); --skip)
        start = tail.find('\n', start) + 1;
    std::string out = tail.substr(start);
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out;
}
