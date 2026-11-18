# logos-zebrad-module

`zebrad_module` runs a Zcash full node, [Zebra](https://github.com/ZcashFoundation/zebra),
**in-process**, through `libzebrad_c` from `logos-zebra-nix`. No executable is bundled and no
subprocess is spawned: the node lives and dies with this module. Other modules reach it over
Logos IPC, through `grpc()` and `rpc()`, rather than through a port.

Linux (x86_64 and arm64), macOS and Windows. Windows builds are cross builds from x86_64-linux; there the
plugin ships beside `zebrad_c.dll`, which imports GCC's runtime (libstdc++, libgcc_s,
mcfgthread) from the host, as every Logos module built with MinGW does.

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
| `rpc(method, paramsJson)` | One call into Zebra's JSON-RPC, as a JSON string |
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
or `{"ok":false,"error":"unloading"}` and the like when the call never reached the node.
The library ends a call at 120 s (DEADLINE_EXCEEDED) or past 256 MiB of response
(RESOURCE_EXHAUSTED), and answers UNIMPLEMENTED for `GetMempoolStream`, which stays open until
the next block; poll `GetMempoolTx` instead.

`rpc(method, paramsJson)` takes a JSON-RPC method and its params as a JSON array or object, or
`""` for none. It answers `{"result":…,"error":null}` or
`{"result":null,"error":{"code":…,"message":…}}`, with no port and no cookie: `-9` while the node
is not running, `-28` while it starts, `-32601` for a method outside the lists below, `-32600`
for params of another shape, and the node's own errors otherwise.

- Every network: `getinfo`, `getdeprecationinfo`, `getblockchaininfo`, `getaddressbalance`,
  `sendrawtransaction`, `getblock`, `getblockheader`, `getbestblockhash`,
  `getbestblockheightandhash`, `getmempoolinfo`, `getrawmempool`, `z_gettreestate`,
  `z_getsubtreesbyindex`, `getrawtransaction`, `getaddresstxids`, `getaddressutxos`,
  `getblockcount`, `getblockhash`, `getmininginfo`, `getnetworksolps`, `getnetworkhashps`,
  `getnetworkinfo`, `getpeerinfo`, `ping`, `validateaddress`, `z_validateaddress`, `z_converttex`,
  `getstandardfee`, `getblocksubsidy`, `getdifficulty`, `z_listunifiedreceivers`, `gettxout`.
- Regtest only, judged by the chain the node runs: `generate`, `generatetoaddress`, `submitblock`,
  `getblocktemplate`, `invalidateblock`, `reconsiderblock`, `addnode`.
- Never: `stop`, which would signal the whole host process (use this module's `stop()`), and
  `rpc.discover`.

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
| `activationHeights` | `{}` | Regtest only: the height of each network upgrade |

The library installs its log writer once per process, so the log file and `logFilter` of the
first start in a host process hold until the module is reloaded.

Networks are `mainnet` and `testnet`, plus `regtest` once its upgrades are named, by
`configure("regtest", {"activationHeights":{"nu7":1}})` or by a `regtest.json` in the instance
directory. Both use the wallet core's format:
`{"overwinter":1,"sapling":1,"blossom":1,"heartwood":1,"canopy":1,"nu5":1,"nu6":1,"nu6_1":1,"nu6_2":1,"nu6_3":120,"nu7":null}`.
The heights become Zebra's `[network.testnet_parameters.activation_heights]` (`NU5`,
`"NU6.1"`, `"NU6.3"` and so on; `null` leaves an upgrade inactive). A non-empty
`activationHeights` replaces the file. The file is read on each regtest call, so it can be
written after the module loads.

A regtest node saves each block as it accepts it, so `stop()` keeps every accepted block. On
mainnet and testnet Zebra saves recent blocks at most every 5 seconds and fetches any it lost
from its peers after a restart.

## Why IPC, not a port

Zebra can serve the lightwalletd protocol on a TCP port, but its own documentation warns
that this port is plaintext HTTP/2 with no authentication: cookie authentication does not
apply to it, and a loopback port is reachable by any local process and by web pages loaded
in a browser on the same machine. So the wallet does not use one. `libzebrad_c` exposes
Zebra's `CompactTxStreamer` service and JSON-RPC as in-memory calls, `ZEBRAD_grpc` and
`ZEBRAD_rpc`, and this module puts them behind Logos IPC.

Any module may call `grpc()` and `rpc()`, and so may the host (`logosctl call`). Both serve
public chain data and relay signed transactions. Which modules may reach this one is the
Logos runtime's decision.

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

