#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

/// A Zcash node (Zebra) running in-process through libzebrad_c. One node at a time; its
/// lifetime is this module's lifetime. Callers reach it through grpc() and rpc(), not a port.
class ZebradModuleImpl : public LogosModuleContext {
public:
    ZebradModuleImpl();
    ~ZebradModuleImpl();

    /// Merge `config` into the stored settings for `network` (mainnet|testnet, and regtest
    /// once `activationHeights` or regtest.json in the instance directory names its upgrades).
    StdLogosResult configure(const std::string& network, const LogosMap& config);

    /// Stored settings for `network`, with defaults filled in.
    LogosMap getConfig(const std::string& network);

    /// Default settings for `network`; empty for a network this instance does not run.
    LogosMap defaultConfig(const std::string& network);

    /// Start the node for `network`. Returns once its thread is running.
    StdLogosResult start(const std::string& network);

    /// Stop the node and wait for it to shut down.
    StdLogosResult stop();

    /// @code{.json}
    /// { "state": "stopped|starting|running|stopping|failed", "network": string,
    ///   "uptimeSecs": number, "cacheDir": string, "height": number,
    ///   "finalizedHeight": number, "estimatedHeight": number, "peers": number,
    ///   "rpcExposed": {"lightwalletd": string, "jsonRpc": string, "indexer": string,
    ///   "health": string}, "lastStopMs": number, "version": string, "lastError": string,
    ///   "syncPercent": number, "chainAgeSecs": number }
    /// @endcode
    /// Never waits on the node: the fields are the library's last status sample, taken
    /// once a second, `chainAgeSecs` old. Numbers are -1 until known; `syncPercent` is
    /// height / estimatedHeight, capped at 100 (100 on regtest), or -1.
    LogosMap status();

    /// One call into Zebra's lightwalletd CompactTxStreamer service, in memory.
    /// `path` is the full gRPC path; the body is gRPC-framed, base64. Returns
    /// {"ok":true,"status":n,"message":s,"body":base64} or {"ok":false,"error":s}.
    std::string grpc(const std::string& path, const std::string& bodyBase64);

    /// One call into Zebra's JSON-RPC, in memory. `paramsJson` is a JSON array or object, or
    /// "" for none. Returns {"result":...,"error":null} or
    /// {"result":null,"error":{"code":n,"message":s}}.
    std::string rpc(const std::string& method, const std::string& paramsJson);

    /// The last `lines` lines of the node's log.
    std::string logTail(int64_t lines);

logos_events:
    /// Emitted on every lifecycle transition, with the shape of status().
    void zebradStateChanged(const std::string& payloadJson);

protected:
    void onContextReady() override;
    LogosShutdown aboutToUnload() override;

private:
    bool knownNetwork(const std::string& network) const;
    nlohmann::json defaultsFor(const std::string& network) const;
    nlohmann::json networkDefaults(const std::string& network) const;
    nlohmann::json settingsFor(const std::string& network);
    std::string zebradToml(const std::string& network, const nlohmann::json& s) const;
    void persist();
    void loadRegtest();
    void pollLoop();
    void refresh(const char* forceState = nullptr);
    nlohmann::json decorate(nlohmann::json st, std::chrono::steady_clock::time_point at, bool sampled);
    void unloadLog(const std::string& line);

    std::mutex m_mutex;
    std::string m_persistDir;               // guarded by m_mutex
    std::string m_logFile;                  // guarded by m_mutex
    nlohmann::json m_config = nlohmann::json::object();  // guarded by m_mutex
    // Regtest's upgrades mapped to Zebra's activation-height keys; regtest exists only with them.
    bool m_regtest = false;                 // guarded by m_mutex
    std::string m_regtestError;             // guarded by m_mutex
    std::vector<std::pair<std::string, uint32_t>> m_regtestHeights;  // guarded by m_mutex

    // ZEBRAD_status_json can wait on the node while it starts, so a thread samples it and
    // status() serves the last sample.
    std::thread m_pollThread;
    std::atomic<bool> m_pollStop{false};
    std::mutex m_pollMutex;
    std::condition_variable m_pollWake;
    std::mutex m_sampleMutex;
    nlohmann::json m_sample;                            // guarded by m_sampleMutex
    std::chrono::steady_clock::time_point m_sampleAt;   // guarded by m_sampleMutex
    std::mutex m_emitMutex;                 // one sample-and-emit at a time, so events keep order
    std::string m_emittedState = "stopped"; // guarded by m_emitMutex

    // Emitting after the host's grace is a use-after-free in the host, so unload silences us.
    std::atomic<bool> m_unloading{false};
    std::thread m_stopThread;
};
