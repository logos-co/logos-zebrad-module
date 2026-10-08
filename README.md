# logos-zebrad-module

`zebrad_module` runs a Zcash full node, [Zebra](https://github.com/ZcashFoundation/zebra),
**in-process**, through `libzebrad_c` from `logos-zebra-nix`. No executable is bundled and no
subprocess is spawned: the node lives and dies with this module. The Zcash wallet reaches it
over Logos IPC, through `grpc()`, rather than through a port.

Linux and macOS. For Windows the flake publishes only the module's contract
(`packages.x86_64-windows.lidl`), so modules that depend on it build there; the node itself
waits on RocksDB under MinGW.

```bash
logosctl call zebrad_module configure testnet 'json:{"peersetInitialTargetSize":25}'
logosctl call zebrad_module start testnet
logosctl call zebrad_module status        # state, height, estimatedHeight, syncPercent, peers, ...
logosctl call zebrad_module logTail 50
logosctl call zebrad_module stop
```

## API

| Method | Returns |
|---|---|
| `configure(network, config)` | Merges `config` into the stored settings; refuses unknown keys and wrong types |
| `getConfig(network)`, `defaultConfig(network)` | The stored settings with defaults filled in; the defaults alone |
| `start(network)` | Starts the node and returns once its thread runs |
| `stop()` | Stops the node and waits for it |
| `status()` | The library's status plus `syncPercent` and `chainAgeSecs`; never waits on the node |
| `grpc(path, bodyBase64)` | One call into Zebra's lightwalletd service, as a JSON string |
| `logTail(lines)` | The last lines of the node's log |

Event: `zebradStateChanged(payloadJson)` on every lifecycle transition
(`stopped`, `starting`, `running`, `stopping`, `failed`), with `status()`'s shape.

`status()` carries `state`, `network`, `uptimeSecs`, `cacheDir`, `height`, `finalizedHeight`,
`estimatedHeight`, `peers`, `rpcExposed` (the address of each open TCP server, `""` when
off), `lastStopMs`, `version` and `lastError` from the library, -1 for a number not yet
known. `running` means the node answers `grpc()`. A thread samples the library once a
second; `chainAgeSecs` is the sample's age. `syncPercent` is `height / estimatedHeight`,
capped at 100, or -1 while either is unknown. On regtest it is 100 once there is a tip:
Zebra's estimate extrapolates from the blocks' 2011 timestamps there, and a local chain has
nothing to catch up with.

`grpc(path, bodyBase64)` takes the full gRPC path, such as
`/cash.z.wallet.sdk.rpc.CompactTxStreamer/GetLightdInfo`, and a gRPC-framed request body
(a 5-byte prefix per message) in standard base64. It answers
`{"ok":true,"status":0,"message":"","body":"<base64 framed response>"}`, where `status` is
the gRPC status (14, UNAVAILABLE, while the node is not running) and `message` its text,
or `{"ok":false,"error":"not authorized"}` and the like when the call never reached the node.
The library ends a call at 120 s (DEADLINE_EXCEEDED) or past 256 MiB of response
(RESOURCE_EXHAUSTED), and answers UNIMPLEMENTED for `GetMempoolStream`, which stays open until
the next block; poll `GetMempoolTx` instead.

## Settings

Per network, persisted in the instance directory (`zebrad.json`).

| Key | Default | |
|---|---|---|
| `cacheDir` | `<instance dir>/<network>` | Zebra's state, peer cache and RPC cookie |
| `peersetInitialTargetSize` | `25` | Peers Zebra aims for at start |
| `listenAddr` | `[::]:8233`, `[::]:18233`; regtest `127.0.0.1:18344` | P2P listener; Zebra's defaults outside regtest |
| `exposeLightwalletd` | `""` (off) | `host:port` for Zebra's lightwalletd gRPC over TCP |
| `exposeJsonRpc` | `""` (off) | `host:port` for Zebra's JSON-RPC |
| `logFilter` | `"info"` | A `tracing` filter for the node's log, `zebrad.log` in the instance directory |
| `minerAddress` | `""` | Regtest only: where `generate` pays the coinbase |

The library installs its log writer once per process, so the log file and `logFilter` of the
first start in a host process hold until the module is reloaded.

Networks are `mainnet` and `testnet`, plus `regtest` when the instance directory holds
`regtest.json`, in the wallet core's format:
`{"overwinter":1,"sapling":1,"blossom":1,"heartwood":1,"canopy":1,"nu5":1,"nu6":1,"nu6_1":1,"nu6_2":1,"nu6_3":120,"nu7":null}`.
Its heights become Zebra's `[network.testnet_parameters.activation_heights]` (`NU5`,
`"NU6.1"`, `"NU6.3"` and so on; `null` leaves an upgrade inactive). The file is read when
the module loads.

## Why IPC, not a port

Zebra can serve the lightwalletd protocol on a TCP port, but its own documentation warns
that this port is plaintext HTTP/2 with no authentication: cookie authentication does not
apply to it, and a loopback port is reachable by any local process and by web pages loaded
in a browser on the same machine. So the wallet does not use one. `libzebrad_c` exposes
Zebra's `CompactTxStreamer` service as an in-memory call, `ZEBRAD_grpc`, and this module
puts it behind Logos IPC with a caller check.

Only `zcash_wallet_core_module` may call `grpc()`. The check reads the caller identity the
host attaches to each call (`logos::currentCaller()`), not anything the caller sends. For
tests, a `callers.json` in the instance directory replaces that list:
`{"modules":["zcash_wallet_core_module"],"allowHost":true}`. `allowHost` admits calls from
the host itself, such as `logosctl call`. A file that does not parse, or has a key other
than these two, admits nobody. The file is read when the module loads.

## Exposing the node to other programs

`exposeLightwalletd` and `exposeJsonRpc` are opt-ins for other consumers on the machine,
such as a second wallet or a block explorer, and are off by default. When either is set the
module writes it into Zebra's `[rpc]` section and tells the library so (`exposeRpc`); without
that the library refuses a config that opens either port.

**Security.** The lightwalletd port has no TLS and no authentication: anything that can reach
it can read the chain through it and send transactions, and a browser page can reach a
loopback port. Bind it to `127.0.0.1` and only on a machine whose local programs you trust.
The JSON-RPC port keeps Zebra's cookie authentication: the cookie is written to
`<cacheDir>/.cookie`, readable only by the user, and is required on every request.

## Unloading

On unload the node is stopped on a thread and the outcome is written to `unload.log` in the
instance directory. The host waits up to its grace period; a node killed after that resumes
from its last committed block, since Zebra's RocksDB state is written in transactions.

## Licence

Zebra is © the Zcash Foundation, dual-licensed under MIT or Apache-2.0, at your option.
`libzebrad_c` links Zebra into one shared library, and Zebra's licence texts ship beside the
plugin as `LICENSE-MIT.zebra` and `LICENSE-APACHE.zebra`.

## Development

```bash
nix build .#install    # result/modules/zebrad_module: the plugin and libzebrad_c
```

