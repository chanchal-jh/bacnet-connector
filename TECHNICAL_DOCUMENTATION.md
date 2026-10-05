# BACnet REST Gateway — Technical Documentation

> **Version:** 0.0.1  
> **Last Updated:** September 2026

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Architecture](#2-architecture)
3. [Project Structure](#3-project-structure)
4. [Component Deep-Dive](#4-component-deep-dive)
   - 4.1 [Go API — `cmd/api/main.go`](#41-go-api--cmdapimain-go)
   - 4.2 [Go Router — `internal/api/router.go`](#42-go-router--internalapiroutergo)
   - 4.3 [Go Handlers — `internal/api/properties.go`](#43-go-handlers--internalapipropertiesgo)
   - 4.4 [Daemon Client — `internal/daemon/client.go`](#44-daemon-client--internaldaemonclientgo)
   - 4.5 [C Daemon — `bacnetd/main.c`](#45-c-daemon--bacnetdmainc)
   - 4.6 [IPC Server — `bacnetd/ipc.c`](#46-ipc-server--bacnetdipcc)
   - 4.7 [Discovery — `bacnetd/discovery.c`](#47-discovery--bacnetddiscoveryc)
   - 4.8 [ReadProperty — `bacnetd/readprop.c`](#48-readproperty--bacnetdreadpropc)
   - 4.9 [WriteProperty — `bacnetd/writeprop.c`](#49-writeproperty--bacnetdwritepropc)
   - 4.10 [ReadPropertyMultiple — `bacnetd/readpropmultiple.c`](#410-readpropertymultiple--bacnetdreadpropmultiplec)
5. [IPC Protocol](#5-ipc-protocol)
6. [API Process Flows](#6-api-process-flows)
   - 6.1 [GET /api/devices](#61-get-apidevices)
   - 6.2 [GET /api/devices/{id}/info](#62-get-apidevicesidinfo)
   - 6.3 [GET /api/devices/{id}/objects](#63-get-apidevicesidobjects)
   - 6.4 [POST /api/read-property](#64-post-apiread-property)
   - 6.5 [POST /api/read-property-multiple](#65-post-apiread-property-multiple)
   - 6.6 [PUT /api/write-property](#66-put-apiwrite-property)
7. [Auto-Discovery Architecture](#7-auto-discovery-architecture)
8. [BACnet Address Cache](#8-bacnet-address-cache)
9. [Docker & Deployment](#9-docker--deployment)
10. [Environment Variables](#10-environment-variables)
11. [Key Design Decisions](#11-key-design-decisions)

---

## 1. Project Overview

The **BACnet REST Gateway** is a production-ready protocol adapter that translates BACnet/IP (a building automation protocol used for HVAC, lighting, and sensor systems) into a simple REST API that any frontend or cloud application can consume.

### The Problem it Solves
BACnet is a binary, UDP-based, broadcast-driven protocol — it cannot be consumed from a browser, mobile app, or cloud service without a translation layer. The Gateway sits on the edge of the building network, does all the heavy BACnet protocol lifting in C, and exposes clean REST endpoints via Go.

### Core Principle
> **Go is a passthrough proxy. It never transforms or validates BACnet data.**

The C daemon owns all BACnet protocol logic. Go owns HTTP routing, request lifecycle, timeouts, and process management. The C daemon's JSON is passed untouched to the HTTP client.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Docker Container                            │
│                                                                     │
│  Frontend / Cloud                                                   │
│       │ TCP:8080 (HTTP)                                             │
│       ▼                                                             │
│  ┌─────────────┐     Unix Socket         ┌─────────────────────┐   │
│  │  Go API     │ ──── /tmp/bacnetd.sock ──►  C Daemon (bacnetd) │   │
│  │  bacnet-api │ ◄────────────────────── │  (BACnet Stack)      │   │
│  └─────────────┘   newline-delimited JSON└─────────────────────┘   │
│                                                  │ UDP:47808        │
└──────────────────────────────────────────────────┼──────────────────┘
                                                   │
                                              Building Network
                                                   │
                                    ┌──────────────┼──────────────┐
                                    │              │              │
                               BACnet          BACnet         BACnet
                               Device          Device         Device
                              (HVAC)          (Sensor)       (Lighting)
```

### Two-Process Model
The application runs as **two cooperating processes inside a single Docker container**:

| Process | Language | Responsibility |
|---|---|---|
| `bacnet-api` (Go) | Go | HTTP server, process manager, request routing |
| `bacnetd` (C) | C | BACnet/IP protocol, UDP comms, address cache |

The Go process **spawns** the C process at startup using `os/exec` and acts as its parent. All C logs are piped to Go's stdout so they appear together in `docker logs`.

---

## 3. Project Structure

```
bacnet/
├── Dockerfile                    # Multi-stage build (C → Go → tiny Debian image)
├── .env                          # Local config (git-ignored)
├── .env.example                  # Config template (committed to Git)
├── .gitignore                    # Excludes binaries, build dirs, .env
├── .dockerignore                 # Excludes .git, IDE files from Docker context
├── VERSION                       # Single source of truth for version number
├── go.mod / go.sum               # Go module definition and checksums
│
├── cmd/
│   └── api/
│       └── main.go               # Go entry point — spawns C daemon, starts HTTP server
│
├── internal/
│   ├── api/
│   │   ├── router.go             # Registers all HTTP routes with middleware
│   │   ├── devices.go            # GET /api/devices handler
│   │   └── properties.go         # All other endpoint handlers + helpers
│   └── daemon/
│       └── client.go             # Unix socket IPC client (dials per-request)
│
├── bacnetd/                      # C source files for the BACnet daemon
│   ├── CMakeLists.txt            # CMake build definition
│   ├── main.c                    # C entry point: BACnet init, IPC loop, dispatching
│   ├── ipc.c / ipc.h             # Unix domain socket server
│   ├── discovery.c / discovery.h # Who-Is / I-Am broadcast and address cache
│   ├── readprop.c / readprop.h   # ReadProperty BACnet service
│   ├── writeprop.c / writeprop.h # WriteProperty BACnet service
│   └── readpropmultiple.c / .h   # ReadPropertyMultiple BACnet service
│
└── bacnet-stack/                 # Official BACnet C library (git submodule)
    └── src/bacnet/...            # ~200k lines of ASHRAE 135 protocol implementation
```

---

## 4. Component Deep-Dive

### 4.1 Go API — `cmd/api/main.go`

**Role:** Application entry point and process manager.

**What it does on startup (in order):**
1. Reads all configuration from environment variables.
2. Deletes any leftover Unix socket file from a previous crash.
3. Launches `bacnetd` as a child process using `os/exec`, piping its stdout/stderr to its own streams so all logs appear together.
4. Sleeps 500ms to allow the C daemon to initialize and bind to the socket.
5. Creates the `daemon.Client` (the IPC connector to C).
6. Launches a **startup warm-up goroutine** — immediately fires a global `Who-Is` broadcast to populate the BACnet address cache before the first client request arrives.
7. Launches a **periodic re-discovery goroutine** — re-broadcasts `Who-Is` every `DISCOVERY_INTERVAL` seconds (default: 5 min) to catch new or rebooted devices.
8. Starts the HTTP server on `HTTP_PORT`.
9. Blocks on `SIGINT`/`SIGTERM` for graceful shutdown.
10. On shutdown: gracefully stops the HTTP server, then kills the C child process.

**Key variable: `Version`**
```go
var Version = "dev"  // overridden at build time with -ldflags "-X main.Version=0.0.1"
```

---

### 4.2 Go Router — `internal/api/router.go`

**Role:** Registers all HTTP routes using the `chi` router.

**Middleware applied to all requests:**
- `middleware.Logger` — logs each request's method, path, status, and duration
- `middleware.Recoverer` — catches panics and returns a 500 instead of crashing
- `middleware.RequestID` — adds a unique `X-Request-Id` header to every response

**Route table:**

| Method | Path | Handler |
|---|---|---|
| `GET` | `/health` | Inline → `{"status":"ok"}` |
| `GET` | `/api/devices` | `devicesHandler` |
| `GET` | `/api/devices/{deviceID}/info` | `deviceInfoHandler` |
| `GET` | `/api/devices/{deviceID}/objects` | `objectListHandler` |
| `POST` | `/api/read-property` | `genericReadHandler` |
| `POST` | `/api/read-property-multiple` | `rpmHandler` |
| `PUT` | `/api/write-property` | `writeHandler` |

---

### 4.3 Go Handlers — `internal/api/properties.go`

Each handler follows the same 3-step pattern:
1. Decode the HTTP request (URL params or JSON body).
2. Call `dc.Raw(...)` or `dc.RawJSON(...)` to send a command to the C daemon over the Unix socket.
3. Write the C daemon's raw JSON response byte-for-byte to the HTTP client.

**Go never modifies the values returned by C.** It only strips the internal `id` field used for IPC correlation.

**Key functions:**

| Function | Purpose |
|---|---|
| `devicesHandler` | Triggers Who-Is, returns device list |
| `deviceInfoHandler` | Reads 15 Device Object properties |
| `objectListHandler` | Reads Property 76 (Object List) |
| `genericReadHandler` | Reads one property from one object |
| `rpmHandler` | Bulk reads via ReadPropertyMultiple |
| `writeHandler` | Writes one property to one object |
| `jsonError` | Writes `{"error":"..."}` with HTTP status |
| `parseUint32` | Parses URL path parameters safely |

**Why `rpmHandler` uses `RawJSON` instead of `Raw`:**
The standard `daemon.Request` struct is flat and cannot represent the nested array of objects that RPM requires. `RawJSON` accepts a pre-built JSON string and injects the IPC `id` field dynamically.

---

### 4.4 Daemon Client — `internal/daemon/client.go`

**Role:** Manages the IPC communication channel between Go and C.

**Design: Per-Request Connections**
The client dials a **new Unix socket connection for every single request** and closes it when done. This is intentional. A persistent connection caused the C daemon to block on `read()` waiting for Go data instead of processing background BACnet traffic.

**Key methods:**

| Method | Purpose |
|---|---|
| `Raw(req, timeout)` | Marshals a `Request` struct to JSON, sends it, reads one response line, strips `id`, returns raw bytes |
| `RawJSON(jsonStr, timeout)` | Same as `Raw` but takes a pre-built JSON string. Injects `id` at position 1 |
| `Discover()` | Convenience wrapper: `Raw({cmd:"discover"}, 10s)` |

**Request ID:** Every request gets a unique monotonic ID (`r1`, `r2`, `r3`...) using `atomic.Uint64`.

---

### 4.5 C Daemon — `bacnetd/main.c`

**Role:** Entry point for the C process. Initializes BACnet, runs the IPC event loop, and dispatches commands.

**Startup sequence:**
```c
dlenv_init();    // Read BACNET_IFACE and BACNET_IP_PORT, bind UDP socket
address_init();  // Initialize the in-memory BACnet device address cache
apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, my_handler_i_am_add);
// Register custom I-Am handler so ALL incoming I-Am packets update the cache
ipc_server_start(socket_path);  // Bind the Unix domain socket for Go comms
```

**Main event loop:**
```c
while (g_running) {
    // 1. Drain any pending BACnet UDP packets (non-blocking)
    datalink_receive(&src, rx_buf, 0) → npdu_handler(...)

    // 2. Wait up to 1 second for a Go client connection
    select(server_fd, timeout=1s)

    // 3. If connected, read JSON command and dispatch
    ipc_accept() → ipc_readline() → dispatch()
}
```

**IPC command dispatch table:**

| `cmd` value | Handler |
|---|---|
| `"discover"` | `handle_discover` |
| `"read"` | `handle_read` |
| `"device-info"` | `handle_device_info` |
| `"object-list"` | `handle_object_list` |
| `"write"` | `handle_write` |
| `"rpm"` | `handle_rpm` |

**Value serialization helpers:**
- `val_to_str(out, val)` — converts `bacnet_value_t` to bare JSON value (e.g., `23.5`, `"Room 101"`)
- `val_to_json(buf, pos, val)` — emits `"value":<v>,"type":"<t>"` for Read response envelope

---

### 4.6 IPC Server — `bacnetd/ipc.c`

**Role:** Wraps POSIX Unix domain socket API for newline-delimited JSON communication.

**Key functions:**
- `ipc_server_start(path)` — creates and binds a `SOCK_STREAM` Unix socket
- `ipc_accept(server_fd)` — accepts one client (Go) connection
- `ipc_readline(client_fd, buf, max)` — reads until `\n`; one line = one JSON command
- `ipc_writeline(client_fd, msg)` — writes message + `\n`; one call = one JSON response

---

### 4.7 Discovery — `bacnetd/discovery.c`

**Role:** Implements BACnet device discovery via `Who-Is` / `I-Am` service.

**Key functions:**

| Function | Description |
|---|---|
| `discovery_whois(list, timeout_ms)` | Broadcasts global `Who-Is`. Waits for `I-Am` replies. Returns discovered device list. |
| `discovery_whois_target(device_id, timeout_ms)` | Sends targeted `Who-Is` for one specific device. Used as auto-discovery fallback on cache miss. |
| `my_handler_i_am_add(src, len, apdu)` | Custom `I-Am` handler. Calls `address_add()` to cache **any** device that sends `I-Am`. |

**Why a custom I-Am handler?**
The library's default handler only caches devices from explicitly requested `Who-Is` responses. Our handler uses `address_add()` (not `address_add_binding()`) which caches spontaneous `I-Am` broadcasts too.

---

### 4.8 ReadProperty — `bacnetd/readprop.c`

**Role:** Sends a BACnet `ReadProperty` confirmed request for a single property.

**Key function:**
```c
int readprop_read(device_id, obj_type, obj_instance, prop_id, array_index, timeout_ms, *out_val)
```

**Execution flow:**
1. Check address cache → `address_get_by_device()`
2. Cache miss → `discovery_whois_target()` → retry cache
3. Still not found → return error
4. `Send_Read_Property_Request()` → UDP to device
5. Poll `datalink_receive()` → `npdu_handler()` until ACK
6. `bacapp_decode_application_data()` → fill `bacnet_value_t`

**`bacnet_value_t` — universal return type:**
```c
typedef struct {
    int type;       // BACNET_VAL_REAL, STRING, BOOLEAN, UNSIGNED, etc.
    char error_msg[128];
    union { float real; unsigned uval; int ival; bool boolean;
            char str[128]; struct { uint type; uint instance; } obj_id; } v;
} bacnet_value_t;
```

---

### 4.9 WriteProperty — `bacnetd/writeprop.c`

**Role:** Sends a BACnet `WriteProperty` confirmed request to set a property value.

**Key function:**
```c
int writeprop_write(device_id, obj_type, obj_instance, prop_id, array_index, priority, *val, timeout_ms, *result)
```

**`wp_val_t` write value types:** `WP_TYPE_REAL`, `WP_TYPE_UNSIGNED`, `WP_TYPE_SIGNED`, `WP_TYPE_BOOLEAN`, `WP_TYPE_ENUMERATED`, `WP_TYPE_STRING`, `WP_TYPE_NULL`

**Priority:** BACnet has 16 command priority levels (1=highest, 16=lowest). Priority `null` relinquishes a level, allowing the next active priority to take effect.

---

### 4.10 ReadPropertyMultiple — `bacnetd/readpropmultiple.c`

**Role:** Sends one BACnet `ReadPropertyMultiple` request covering multiple objects and properties in a single UDP packet.

**Key function:**
```c
int rpm_read(device_id, rpm_object_req_t *req, req_count, out_buf, out_sz, err_buf, err_sz, timeout_ms)
```

**Execution flow:**
1. Cache check + auto-discovery (same as RP)
2. Build `BACNET_READ_ACCESS_DATA` linked list for all objects/properties
3. `Send_Read_Property_Multiple_Request()` → ONE UDP packet
4. Poll `datalink_receive()` → RPM ACK
5. **Decode nested ACK:**
   - `rpm_ack_decode_object_id()` → object type + instance
   - `rpm_ack_decode_object_property()` → property ID
   - Context Tag `[4]` = success → `bacapp_decode_application_data()` → value
   - Context Tag `[5]` = error → emit `{"value":null,"type":"error"}`
   - `rpm_ack_decode_object_end()` → next object

**Performance impact:**

| Method | Objects | Properties | UDP Packets | Time |
|---|---|---|---|---|
| ReadProperty (sequential) | 10 | 5 | 50 | ~10 seconds |
| ReadPropertyMultiple | 10 | 5 | 1 | ~200ms |

---

## 5. IPC Protocol

All communication between Go and C uses **newline-delimited JSON** over a Unix domain socket.

### Request (Go → C):
```json
{"id":"r42","cmd":"read","device":3489866,"obj_type":0,"obj_instance":1,"prop":85}
```

### Response (C → Go):
```json
{"id":"r42","status":"ok","value":23.5,"type":"real"}
```
or on error:
```json
{"id":"r42","status":"error","error":"device not in address cache"}
```

Go strips `"id"` before writing to the HTTP client.

### Full Command Reference:

| `cmd` | Required Fields | C Handler | BACnet Service |
|---|---|---|---|
| `discover` | — | `handle_discover` | Who-Is broadcast |
| `device-info` | `device` | `handle_device_info` | ReadProperty × 15 |
| `object-list` | `device` | `handle_object_list` | ReadProperty (prop 76) |
| `read` | `device`, `obj_type`, `obj_instance`, `prop` | `handle_read` | ReadProperty |
| `write` | `device`, `obj_type`, `obj_instance`, `prop`, `val_type`, `value`, `priority` | `handle_write` | WriteProperty |
| `rpm` | `device`, `objects:[{obj_type, obj_instance, props:[...]}]` | `handle_rpm` | ReadPropertyMultiple |

---

## 6. API Process Flows

### 6.1 GET /api/devices

```
Frontend: GET /api/devices
  ↓
Go devicesHandler → dc.Discover()
  ↓ Unix socket
C handle_discover → discovery_whois(3000ms)
  → broadcast Who-Is UDP to 192.168.0.255:47808
  → each I-Am reply → my_handler_i_am_add() → address_add()
  → build JSON array of discovered devices
  ↓
Frontend: {"status":"ok","devices":[{"id":3489866,"addr":"192.168.0.86",...}]}
```

---

### 6.2 GET /api/devices/{id}/info

```
Frontend: GET /api/devices/3489866/info
  ↓
Go deviceInfoHandler → dc.Raw({cmd:"device-info", device:3489866}, 60s)
  ↓
C handle_device_info
  → loop over 15 property IDs [77,28,58,121,120,70,44,12,98,139,112,62,11,73,155]
  → for each: readprop_read(device=3489866, obj=Device/3489866, prop=N)
  → val_to_str() + bactext_property_name() → "\"object-name\":\"device-3489866\""
  → skip unsupported properties silently
  ↓
Frontend: {"status":"ok","properties":{"object-name":"...","vendor-name":"...",...}}
```

---

### 6.3 GET /api/devices/{id}/objects

```
Frontend: GET /api/devices/3489866/objects
  ↓
Go objectListHandler → dc.Raw({cmd:"object-list", device:3489866}, 30s)
  ↓
C handle_object_list
  → readprop_read(prop=76, array_index=0) → count=13
  → loop i=1..13: readprop_read(prop=76, array_index=i) → OBJECT_ID value
  → bactext_object_type_name() → "analog-input"
  → build JSON array
  ↓
Frontend: {"status":"ok","objects":[{"type":0,"type_name":"analog-input","instance":1},...]}
```

---

### 6.4 POST /api/read-property

```
Frontend: POST /api/read-property
  Body: {"device":3489866,"obj_type":0,"obj_instance":1,"prop":85}
  ↓
Go genericReadHandler → dc.Raw({cmd:"read",...}, 10s)
  ↓
C handle_read → readprop_read(3489866, AI-1, prop=PresentValue, 5000ms)
  → cache hit → Send_Read_Property_Request() → UDP
  → poll → ACK → decode → val={REAL, 23.5}
  → val_to_json() → "\"value\":23.5,\"type\":\"real\""
  ↓
Frontend: {"status":"ok","value":23.5,"type":"real"}
```

---

### 6.5 POST /api/read-property-multiple

```
Frontend: POST /api/read-property-multiple
  Body: {"device":3489866,"objects":[
    {"obj_type":0,"obj_instance":1,"props":[77,85,117,111]},
    {"obj_type":2,"obj_instance":1,"props":[77,85,111]}
  ]}
  ↓
Go rpmHandler
  → build IPC JSON string with nested arrays
  → dc.RawJSON({"cmd":"rpm",...}, 15s)
  ↓
C handle_rpm
  → parse objects array manually
  → rpm_read(device, req[2], ...)
    → build BACNET_READ_ACCESS_DATA linked list
    → Send_Read_Property_Multiple_Request() → ONE UDP packet
    → poll → RPM ACK → rpm_ack_handler()
      → for each object: decode obj_id
      → for each property: decode prop_id
      → Context Tag [4]: decode value → val_to_json_fragment()
      → Context Tag [5]: emit {"value":null,"type":"error"}
  ↓
Frontend: {"status":"ok","objects":[
  {"obj_type":0,"type_name":"analog-input","obj_instance":1,
   "properties":{"77":{"value":"Zone_Temp","type":"string"},"85":{"value":23.5,"type":"real"},...}},
  {"obj_type":2,"type_name":"analog-value","obj_instance":1,"properties":{...}}
]}
```

---

### 6.6 PUT /api/write-property

```
Frontend: PUT /api/write-property
  Body: {"device":3489866,"obj_type":2,"obj_instance":1,
         "prop":85,"val_type":"real","value":"22.0","priority":10}
  ↓
Go writeHandler → dc.Raw({cmd:"write",...}, 10s)
  ↓
C handle_write
  → parse val_type="real" → val={WP_TYPE_REAL, 22.0}
  → writeprop_write(device, AV-1, PresentValue, priority=10, val, 3000ms)
    → cache hit → build BACNET_APPLICATION_DATA_VALUE
    → Send_Write_Property_Request_Data() → UDP
    → poll → SimpleACK → success
  ↓
Frontend: {"status":"ok"}
```

---

## 7. Auto-Discovery Architecture

The gateway uses a **three-tier discovery system** so clients never need to manually trigger discovery:

| Tier | Where | Trigger | Handles |
|---|---|---|---|
| **Tier 1: Startup Warm-Up** | Go goroutine | On boot | Populates cache before first request |
| **Tier 2: Periodic Re-Discovery** | Go ticker goroutine | Every N seconds | New/rebooted devices |
| **Tier 3: Per-Request Fallback** | C `readprop.c` / `rpm.c` | On cache miss | Specific device not yet cached |

Tier 3 is transparent — it fires a targeted `Who-Is` for the exact device ID, waits for `I-Am`, updates the cache, and retries the original request, all within the same API call.

---

## 8. BACnet Address Cache

The `bacnet-stack` library maintains an in-memory table mapping `device_id → {IP, port, max APDU size}`.

**Key functions:**
- `address_add(device_id, src, max_apdu)` — aggressively adds/updates an entry (used by our `my_handler_i_am_add`)
- `address_get_by_device(device_id, &max_apdu, &dest)` — returns `true` if found

The cache **resets on every restart** — this is why the startup warm-up goroutine is essential.

**Why `address_add` vs `address_add_binding`?**
`address_add_binding()` only caches devices that responded to a specifically-sent `Who-Is`. Our handler uses `address_add()` which caches **any** device that sends `I-Am`, including spontaneous broadcasts.

---

## 9. Docker & Deployment

### Multi-Stage Dockerfile

| Stage | Base Image | What it does |
|---|---|---|
| `c-builder` | `debian:bookworm-slim` + gcc/cmake | Compiles `bacnetd` |
| `go-builder` | `golang:1.22-bookworm` | Compiles `bacnet-api` (static, CGO_ENABLED=0) |
| `final` | `debian:bookworm-slim` | Copies two binaries; sets `ENTRYPOINT=/app/bacnet-api` |

The final image is ~40MB. It only contains two binaries and a minimal Debian userland.

### Why `--network host` is mandatory
BACnet/IP uses UDP broadcast packets. Docker's default bridge network blocks these. `--network host` removes isolation, allowing the C daemon to reach the physical building network.

### Build & Run:
```bash
# Local test
docker buildx build --load -t bacnet-gateway:local .
docker run -d --network host -e BACNET_IFACE=eth0 bacnet-gateway:local

# Production multi-arch push
docker buildx build \
  --platform linux/amd64,linux/arm64 \
  -t registry/bacnet-gateway:0.0.1 \
  -t registry/bacnet-gateway:latest \
  --push .
```

---

## 10. Environment Variables

| Variable | Default | Process | Description |
|---|---|---|---|
| `HTTP_PORT` | `8080` | Go | TCP port the REST API listens on |
| `BACNETD_PATH` | `./bacnetd/build/bacnetd` | Go | Path to the compiled C binary |
| `BACNETD_SOCKET` | `/tmp/bacnetd.sock` | Go + C | Unix socket path for IPC |
| `BACNET_IFACE` | (auto) | C | Network interface (e.g., `eth0`) |
| `BACNET_IP_PORT` | `47808` | C | BACnet UDP port (0xBAC0 = 47808) |
| `DISCOVERY_INTERVAL` | `300` | Go | Seconds between periodic re-discovery (0 = disable) |

---

## 11. Key Design Decisions

### Go as a Passthrough Proxy
Go handlers never inspect or transform BACnet values. The C daemon's JSON is written byte-for-byte to the HTTP client. BACnet nuances are preserved for the frontend to handle, and future C serialization changes don't require Go changes.

### Per-Request Unix Socket Connections
Each API call opens and closes a fresh Unix socket connection. This keeps the C daemon's main loop free to drain background BACnet UDP traffic (`I-Am` broadcasts) between requests, without blocking on Go IPC.

### No External JSON Library in C
The C daemon parses IPC JSON with two hand-written helpers (`json_get_str`, `json_get_int`). The IPC format is fully controlled by us, so a full JSON library is unnecessary and would add complexity and binary size.

### Single-Threaded C Daemon
The C daemon is intentionally single-threaded. The `bacnet-stack` TSM (Transaction State Machine) that tracks pending confirmed requests is not thread-safe. All BACnet I/O and IPC handling happen on the same thread.

### Human-Readable BACnet Enums
`bactext_object_type_name()` and `bactext_property_name()` from `bacnet-stack` map raw integers to ASHRAE standard strings (`"analog-input"`, `"object-name"`). The frontend never needs a mapping table.
