# BACnet REST Gateway

A production-ready BACnet-to-REST gateway that decouples raw BACnet/IP protocol handling from frontend applications.

```text
UI ──HTTP:8080──► Go API ──Unix socket──► bacnetd (C) ──UDP:47808──► BACnet Devices
```

### Key Features
- **Process Management:** The Go API acts as a transparent process manager, automatically spawning the C daemon in the background and managing its lifecycle and logs.
- **Zero-Configuration Routing:** The C daemon actively listens for background `I-Am` broadcasts and caches device IP addresses. If a requested device isn't in cache, the daemon automatically pauses, broadcasts a targeted `Who-Is`, updates the cache, and fulfills the request on the fly.
- **ReadPropertyMultiple (RPM):** Supports high-performance bulk reads (fetching multiple properties from multiple objects in a single BACnet UDP packet).
- **Docker Multi-Arch:** Ships as a tiny, multi-stage Docker image compatible with both `amd64` servers and `arm64` edge devices (like Raspberry Pi).

---

## Getting Started (Docker)

### Building the Image (Multi-Architecture)
To compile the C stack and Go API into a production Docker image:
```bash
docker buildx build --platform linux/amd64,linux/arm64 -t bacnet-gateway:latest .
```
*(If building only for your local machine, use `docker buildx build --load -t bacnet-gateway:local .`)*

The recommended way to run the gateway is via Docker. Because BACnet uses UDP broadcasts, you **must** use host networking.

```bash
# Run the pre-built image
docker run -d \
  --name bacnet-gateway \
  --network host \
  -e BACNET_IFACE=eth0 \
  -e BACNET_IP_PORT=47808 \
  -e HTTP_PORT=8080 \
  bacnet-gateway:latest
```

---

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HTTP_PORT` | `8080` | Go API HTTP port |
| `BACNETD_PATH` | `./bacnetd/build/bacnetd` | Path to the compiled C binary |
| `BACNETD_SOCKET` | `/tmp/bacnetd.sock` | Path to Unix socket |
| `BACNET_IFACE` | (auto) | Network interface for BACnet/IP (e.g., `eth0`) |
| `BACNET_IP_PORT` | `47808` | BACnet UDP port |

---
## Local Development (Without Docker)

If you are developing locally, build the C daemon first, then run the Go API.

```bash
# 1. Build the C Daemon
cd bacnetd
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd ..

# 2. Run the Go API (It will automatically launch the C daemon!)
go build -o bacnet-api ./cmd/api
export BACNET_IFACE=eth0 
./bacnet-api
```

---

## REST API Documentation

### 1. Discover Devices
Broadcasts a global `Who-Is` and returns all responding devices on the network.
```bash
curl -s http://localhost:8080/api/devices
```

### 2. Get Device Profile
Fetches the 15 standard properties of the Device Object as a flat dictionary.
```bash
curl -s http://localhost:8080/api/devices/3489866/info
```

### 3. List Device Objects
Reads Property 76 (`Object_List`) and maps numeric types to strings.
```bash
curl -s http://localhost:8080/api/devices/3489866/objects
```

### 4. Read Property (Generic)
Reads any specific property from any object.
```bash
curl -s -X POST http://localhost:8080/api/read-property \
  -H "Content-Type: application/json" \
  -d '{"device": 3489866, "obj_type": 0, "obj_instance": 1, "prop": 85}'
```

### 5. Read Property Multiple (RPM)
High-performance endpoint to read multiple properties across multiple objects in a single network trip.
```bash
curl -s -X POST http://localhost:8080/api/read-property-multiple \
  -H "Content-Type: application/json" \
  -d '{
    "device": 3489866,
    "objects": [
      {"obj_type": 0, "obj_instance": 1, "props": [77, 85, 117, 111]},
      {"obj_type": 2, "obj_instance": 1, "props": [77, 85, 111]}
    ]
  }'
```
```json
{
  "status": "ok",
  "objects": [
    {
      "obj_type": 0, "type_name": "analog-input", "obj_instance": 1,
      "properties": {
        "77": {"value": "Zone_Temp", "type": "string"},
        "85": {"value": 23.5, "type": "real"},
        "117": {"value": 62, "type": "unsigned"},
        "111": {"value": 0, "type": "unsigned"}
      }
    }
  ]
}
```

### 6. Write Property (Generic)
Writes to any commandable property. `val_type` can be `real`, `unsigned`, `signed`, `boolean`, `enumerated`, `string`, or `null` (to relinquish). Priority defaults to 16.
```bash
curl -s -X PUT http://localhost:8080/api/write-property \
  -H "Content-Type: application/json" \
  -d '{
    "device": 3489866,
    "obj_type": 2,
    "obj_instance": 1,
    "prop": 85,
    "val_type": "real",
    "value": "72.5",
    "priority": 10
  }'
```

---
