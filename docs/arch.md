## Architecture

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
