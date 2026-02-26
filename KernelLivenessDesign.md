# Interconnect Kernel Liveness Design

## Problem

Interconnect currently uses user-space periodic ping packets to detect peer liveness quickly.
This is reliable but creates periodic actor activity, extra context switches, and additional small TCP packets on idle connections.

The goal is to add an optional mode that uses kernel TCP mechanisms:

- `SO_KEEPALIVE`
- `TCP_KEEPIDLE`
- `TCP_KEEPINTVL`
- `TCP_KEEPCNT`
- `TCP_USER_TIMEOUT` (when supported)

This mode must work correctly in mixed clusters where some nodes have feature enabled and some do not.


## Goals

- Reduce user-space periodic ping traffic and scheduling overhead on idle sessions.
- Keep fast peer disconnect detection.
- Preserve backward compatibility with old binaries.
- Support mixed clusters without relying on identical config on all nodes.
- Keep feature optional and safe-by-default.
- Preserve delivery semantics on interruption: pending tracked messages must still produce `TEvUndelivered`.
- Preserve current interconnect flow-control ACK behavior (application-level confirms) when user-level ping is disabled.


## Non-goals

- Replace transport-level flow control logic.
- Remove all ping-related protocol fields immediately.
- Change existing handshake protocol version.


## Current behavior (relevant points)

- User-space ping generation:
  - `ydb/library/actors/interconnect/interconnect_tcp_session.cpp`
  - `TInterconnectSessionTCP::IssuePingRequest()`
- Ping-driven forced flush logic:
  - `TInterconnectSessionTCP::ResetFlushLogic()`
- Input dead-peer watchdog:
  - `ydb/library/actors/interconnect/interconnect_tcp_input_session.cpp`
  - `TInputSessionTCP::Bootstrap()` and `HandleCheckDeadPeer()`
- Socket setup currently includes nonblock + `TCP_NODELAY`:
  - `ydb/library/actors/interconnect/interconnect_handshake.cpp`
  - `THandshakeActor::TConnection::SetupSocket()`


## High-level design

The feature is enabled only when all three are true:

1. Local config enables kernel liveness.
2. Local side can apply required socket options on the actual socket.
3. Handshake negotiation with peer succeeds (`UseKernelLiveness=true`).

Only then session uses kernel liveness mode and disables user-space periodic ping/dead-peer checks.

Important invariant: kernel liveness mode must not change nondelivery behavior.
Current logic already sends nondelivery notifications during interruption/session termination via:

- `TInterconnectSessionTCP::Terminate()` (`ForwardOnNondelivery` and `channel.ProcessUndelivered`)
  - `ydb/library/actors/interconnect/interconnect_tcp_session.cpp`
- `TEventOutputChannel::ProcessUndelivered()`
  - `ydb/library/actors/interconnect/interconnect_channel.cpp`

Important invariant: kernel liveness mode must not change flow-control ACK routine.
Application-level confirms are independent control packets and must remain active.
Current ACK routine (must stay intact):

- Input session reports receive progress to session:
  - `TEvUpdateFromInputSession(ConfirmedByInput, NumDataBytes, Ping)`
  - `ydb/library/actors/interconnect/interconnect_tcp_input_session.cpp`
- Session updates confirm accounting:
  - `Handle(TEvUpdateFromInputSession)` updates `UnconfirmedBytes`
  - schedules confirm packet by size threshold or `ForceConfirmPeriod`
  - `ydb/library/actors/interconnect/interconnect_tcp_session.cpp`
- Confirm packets are generated as ordinary control packets (`MakePacket(false)`), not ping packets.


## Linux semantics to account for (`TCP_USER_TIMEOUT`)

Reference:

- https://blog.cloudflare.com/when-tcp-sockets-refuse-to-die/

Key implications for this feature:

1. Idle `ESTABLISHED` sockets do not die by themselves.
   - Keepalives are required to detect dead peer in idle case.

2. `TCP_USER_TIMEOUT` does not replace keepalive probes for idle sockets by itself.
   - With keepalive enabled, user-timeout is checked only after at least one probe is sent.
   - Practical lower bound is still `TCP_KEEPIDLE`.

3. Keepalive timers are not active in all states.
   - Busy unacked-data and zero-window (`persist`) paths follow retransmission logic (`tcp_retries2`).
   - `TCP_USER_TIMEOUT` may affect these paths and can close sockets earlier than default retransmission timeout.

4. Timeout selection must be conservative.
   - Baseline recommendation: `UserTimeout ~= KeepAliveIdle + KeepAliveInterval * KeepAliveProbes`.
   - Avoid too small values to prevent false-positive disconnects under transient congestion.
   - If exact keepalive-probe semantics are desired, user-timeout should not significantly exceed that envelope.

These constraints are Linux-specific behavior considerations and must be reflected in defaults and rollout.


## Configuration design

### Proto changes

File: `ydb/core/protos/config.proto`, message `TInterconnectConfig`.

Add new optional fields:

- `EnableKernelLiveness`
- `KernelKeepAliveIdleDuration`
- `KernelKeepAliveIntervalDuration`
- `KernelKeepAliveProbeCount`
- `KernelUserTimeoutDuration`
- `DisableUserSpacePingWhenKernelLivenessEnabled` (default `true`)

Notes:

- Use duration-based fields (`NKikimrConfigUnits.TDuration`) for time settings.
- Keep defaults conservative and disabled by default.

### Runtime settings mapping

Files:

- `ydb/library/actors/interconnect/interconnect_common.h`
- `ydb/core/driver_lib/run/kikimr_services_initializers.cpp`

Add struct to `TInterconnectSettings`:

- `Enabled`
- `KeepAliveIdle`
- `KeepAliveInterval`
- `KeepAliveProbes`
- `UserTimeout`
- `DisableUserSpacePing`

Validation/clamping:

- Timeouts must be positive.
- Probe count >= 1.
- Clamp to OS `int` range for `setsockopt`.


## Handshake protocol negotiation

Mixed cluster support requires explicit per-connection negotiation.

### Proto additions

File: `ydb/library/actors/protos/interconnect.proto`.

- `THandshakeRequest`: add `optional bool RequestKernelLiveness = <new-id>;`
- `THandshakeSuccess`: add `optional bool UseKernelLiveness = <new-id>;`

IDs must be unique and appended after existing fields to preserve compatibility.

### Negotiation algorithm

#### Outgoing side

Before sending handshake request:

1. Compute `localKernelCandidate`:
   - config enabled
   - socket options applied successfully on outgoing socket
2. Set `request.SetRequestKernelLiveness(localKernelCandidate)`.

After receiving handshake success:

1. Read `peerDecision = success.GetUseKernelLiveness()`.
2. Final session flag:
   - `Params.UseKernelLiveness = peerDecision && localKernelCandidate`.

#### Incoming side

Before sending handshake success:

1. Compute `localKernelCandidate` for accepted socket.
2. Read `peerRequested = request.GetRequestKernelLiveness()`.
3. Decide:
   - `negotiated = localKernelCandidate && peerRequested`.
4. Set:
   - `success.SetUseKernelLiveness(negotiated)`.
   - `Params.UseKernelLiveness = negotiated`.

### Compatibility behavior

- New node <-> old node:
  - old node does not set request/success fields, defaults to `false`.
  - negotiated result is `false`.
  - legacy ping/dead-peer logic remains active.
- New node <-> new node with different local config:
  - negotiation resolves per connection using request/success booleans.


## Socket option application strategy

### Where

Apply in handshake connection setup path:

- `ydb/library/actors/interconnect/interconnect_handshake.cpp`
- extend `TConnection::SetupSocket()`.

Apply to:

- main socket
- external data channel socket (when used)

### Behavior

If kernel liveness config is disabled:

- do nothing (current behavior).

If enabled:

- attempt to set:
  - `SO_KEEPALIVE=1`
  - `TCP_KEEPIDLE`
  - `TCP_KEEPINTVL`
  - `TCP_KEEPCNT`
  - `TCP_USER_TIMEOUT` (best effort, platform/feature guarded)
- if any mandatory keepalive option fails:
  - mark `localKernelCandidate=false`
  - log warning
  - continue handshake with legacy mode
- `TCP_USER_TIMEOUT`:
  - lazy support detection and caching (grpc-like approach)
  - if unsupported, do not fail handshake
  - do not auto-pick aggressive values; rely on conservative config defaults

Important: never fail handshake solely because kernel liveness cannot be enabled.


## Session runtime behavior changes

### Session params

File: `ydb/library/actors/interconnect/types.h`.

Add:

- `bool UseKernelLiveness = false;`

### Disable user-space periodic ping only when negotiated

File: `ydb/library/actors/interconnect/interconnect_tcp_session.cpp`.

Gate by `Params.UseKernelLiveness && Common->Settings.KernelLiveness.DisableUserSpacePing`:

- skip `IssuePingRequest()` calls.
- skip ping-driven forced flush scheduling in `ResetFlushLogic()` (`PingPeriod` timer path).

Do not gate or modify:

- `TEvUpdateFromInputSession` processing
- `UnconfirmedBytes` accounting
- `ForceConfirmPeriod`-based confirm scheduling
- size-triggered confirm path (`needConfirm` -> `MakePacket(false)`)

Keep response to incoming ping requests for backward compatibility and transition period.

### Disable input dead-peer watchdog when negotiated

File: `ydb/library/actors/interconnect/interconnect_tcp_input_session.cpp`.

- In `Bootstrap()` do not arm `TEvCheckDeadPeer` when kernel mode is active.
- `HandleCheckDeadPeer()` path remains unchanged for legacy mode.

### Whiteboard status handling

File: `ydb/library/actors/interconnect/interconnect_tcp_session.cpp`.

In kernel mode, do not degrade status solely by `LastInputActivityTimestamp` thresholds.
Keep utilization-based degradation logic.

### Nondelivery semantics (must stay unchanged)

Kernel liveness integration must not alter:

- tracked delivery behavior (`IEventHandle::FlagTrackDelivery`)
- generation of `TEvUndelivered::Disconnected` on interruption
- existing unsure-undelivered behavior (`FlagGenerateUnsureUndelivered`)

No changes are planned in `Terminate()`/`ProcessUndelivered()` paths besides liveness detection source.

### Flow-control ACK semantics (must stay unchanged)

No changes are planned in confirm/ACK pipeline:

- `ProcessHeader()` confirm ingestion (`ConfirmedByInput`)
- delivery of `TEvUpdateFromInputSession` to session actor
- sender-side `DropConfirmed()` behavior and in-flight window release
- confirm packet emission cadence controlled by `ForceConfirmPeriod` and byte threshold

Disabling user-level ping only removes ping/clock packets and ping-period timer; it must not suppress confirm-only control packets.


## Observability

Add counters and debug fields:

- kernel liveness requested
- negotiated enabled
- local apply success/failure
- fallback reason (unsupported platform, setsockopt failure, peer did not request/accept)

Expose in session HTTP debug pages for both outgoing and input session panels.


## Testing plan

### Unit-level

1. Config parsing and normalization tests:
   - proto -> `TInterconnectSettings` mapping
   - invalid values clamped/rejected as expected

2. Handshake negotiation tests:
   - request=true/success=true => `Params.UseKernelLiveness=true`
   - mixed combos => false
   - old-peer missing fields => false

3. Socket option helper tests (where practical):
   - unsupported `TCP_USER_TIMEOUT` path does not fail.

### Integration (2-node interconnect UT)

Use `ydb/library/actors/interconnect/ut/interconnect_ut.cpp` and/or dedicated UT file.

1. Negotiation matrix:
   - both enabled => negotiated true
   - one enabled, one disabled => negotiated false

2. Idle traffic profile:
   - with negotiated=true, idle generated packets lower than legacy mode.

3. Disconnect detection:
   - interrupt traffic (existing interrupter infra) and assert disconnect appears within configured budget.

4. Flow-control ACK invariants:
   - asymmetric traffic: node A continuously sends data, node B mostly receives;
   - with negotiated kernel liveness enabled, verify progress continues (no stall on in-flight limit);
   - verify confirms are still emitted (e.g. by session debug counters/in-flight drop dynamics).

5. Nondelivery invariants:
   - send messages with `FlagTrackDelivery` while forcing interruption;
   - assert `TEvUndelivered` is produced in both modes:
     - legacy user-space ping mode
     - negotiated kernel liveness mode

6. Backward-compat mode:
   - when negotiation false, old ping/dead-peer path still works.

6. Busy-socket guardrail:
   - create scenario with in-flight/unacked data or receiver-side backpressure;
   - verify configured `TCP_USER_TIMEOUT` does not cause unacceptable false-positive disconnects.


## Rollout strategy

Phase 1:

- introduce proto/config/runtime fields
- implement socket option application + negotiation
- no behavior switch yet (keep user-space ping active)
- add metrics/debug visibility

Phase 2:

- enable runtime switch (disable user-space ping/dead-peer checks only on negotiated sessions)
- run mixed-cluster and stress tests

Phase 3:

- optional default enablement for selected environments after production validation


## Risks and mitigations

1. `TCP_USER_TIMEOUT` not supported everywhere.
   - Mitigation: best-effort, cached detection, fallback to legacy mode.

2. Negotiated=true but runtime mismatch.
   - Mitigation: request flag is set only if local options were successfully applied.

3. Observability regressions (PingTime/ClockSkew less meaningful in kernel mode).
   - Mitigation: keep fields, mark mode in debug/counters, avoid false alarms in whiteboard status.

4. Behavior drift in mixed clusters.
   - Mitigation: explicit handshake negotiation and exhaustive matrix tests.


## Implementation checklist (files)

- `ydb/core/protos/config.proto`
- `ydb/library/actors/protos/interconnect.proto`
- `ydb/library/actors/interconnect/interconnect_common.h`
- `ydb/core/driver_lib/run/kikimr_services_initializers.cpp`
- `ydb/library/actors/interconnect/types.h`
- `ydb/library/actors/interconnect/interconnect_handshake.cpp`
- `ydb/library/actors/interconnect/interconnect_tcp_session.cpp`
- `ydb/library/actors/interconnect/interconnect_tcp_input_session.cpp`
- interconnect UT files under `ydb/library/actors/interconnect/ut/`
