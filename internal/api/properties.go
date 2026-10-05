package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"github.com/go-chi/chi/v5"

	"github.com/chanchal/bacnet/internal/daemon"
)

// readRequest mirrors the IPC fields the C daemon expects for a "read" command.
// The frontend sends this JSON body directly to POST /api/read-property.
type readRequest struct {
	Device      uint32 `json:"device"`
	ObjType     uint16 `json:"obj_type"`
	ObjInstance uint32 `json:"obj_instance"`
	Prop        uint32 `json:"prop"`
}

// devicesHandler — GET /api/devices
// Triggers Who-Is and passes the C daemon response straight through.
func devicesHandler(dc *daemon.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		raw, err := dc.Discover()
		if err != nil {
			jsonError(w, err.Error(), http.StatusBadGateway)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(raw)
	}
}

// deviceInfoHandler — GET /api/devices/{deviceID}/info
// Sends device-info command to C; the daemon reads all standard Device Object
// properties and returns a property map. Go writes it untouched.
func deviceInfoHandler(dc *daemon.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		deviceID, err := parseUint32(chi.URLParam(r, "deviceID"))
		if err != nil {
			jsonError(w, "invalid deviceID", http.StatusBadRequest)
			return
		}

		raw, err := dc.Raw(daemon.Request{
			Cmd:    "device-info",
			Device: deviceID,
		}, 60*time.Second) // longer: 15 sequential reads
		if err != nil {
			jsonError(w, err.Error(), http.StatusBadGateway)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(raw)
	}
}

// objectListHandler — GET /api/devices/{deviceID}/objects
// Fetches the array of Object IDs supported by the device.
func objectListHandler(dc *daemon.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		deviceID, err := parseUint32(chi.URLParam(r, "deviceID"))
		if err != nil {
			jsonError(w, "invalid deviceID", http.StatusBadRequest)
			return
		}

		raw, err := dc.Raw(daemon.Request{
			Cmd:    "object-list",
			Device: deviceID,
		}, 30*time.Second) // Could take a while if the device has many objects
		if err != nil {
			jsonError(w, err.Error(), http.StatusBadGateway)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(raw)
	}
}

// genericReadHandler — POST /api/read-property
// Accepts a JSON body {device, obj_type, obj_instance, prop},
// passes it to the C daemon, and writes the response untouched.
func genericReadHandler(dc *daemon.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req readRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonError(w, "invalid json payload", http.StatusBadRequest)
			return
		}

		raw, err := dc.Raw(daemon.Request{
			Cmd:         "read",
			Device:      req.Device,
			ObjType:     req.ObjType,
			ObjInstance: req.ObjInstance,
			Prop:        req.Prop,
		}, 10*time.Second)
		if err != nil {
			jsonError(w, err.Error(), http.StatusBadGateway)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(raw)
	}
}

// ---- helpers ----

func jsonError(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func parseUint32(s string) (uint32, error) {
	v, err := strconv.ParseUint(s, 10, 32)
	return uint32(v), err
}

type writeRequest struct {
	Device      uint32 `json:"device"`
	ObjType     uint16 `json:"obj_type"`
	ObjInstance uint32 `json:"obj_instance"`
	Prop        uint32 `json:"prop"`
	Priority    uint8  `json:"priority"` // optional
	ValType     string `json:"val_type"` // e.g., "real", "boolean", "null"
	Value       string `json:"value"`    // stringified value, e.g., "23.5", "true", "1"
}

// writeHandler — PUT /api/write-property
func writeHandler(dc *daemon.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req writeRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonError(w, "invalid json payload", http.StatusBadRequest)
			return
		}

		if req.ValType == "" || req.Value == "" {
			if req.ValType != "null" {
				jsonError(w, "val_type and value are required (unless val_type is null)", http.StatusBadRequest)
				return
			}
		}

		raw, err := dc.Raw(daemon.Request{
			Cmd:         "write",
			Device:      req.Device,
			ObjType:     req.ObjType,
			ObjInstance: req.ObjInstance,
			Prop:        req.Prop,
			Priority:    req.Priority,
			ValType:     req.ValType,
			Value:       req.Value,
		}, 10*time.Second)

		if err != nil {
			jsonError(w, err.Error(), http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Write(raw)
	}
}

// ─── ReadPropertyMultiple ────────────────────────────────────────────────────

type rpmObjectReq struct {
	ObjType     uint16   `json:"obj_type"`
	ObjInstance uint32   `json:"obj_instance"`
	Props       []uint32 `json:"props"`
}

type rpmRequest struct {
	Device  uint32         `json:"device"`
	Objects []rpmObjectReq `json:"objects"`
}

// rpmHandler — POST /api/read-property-multiple
func rpmHandler(dc *daemon.Client) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req rpmRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonError(w, "invalid json payload", http.StatusBadRequest)
			return
		}
		if req.Device == 0 || len(req.Objects) == 0 {
			jsonError(w, "device and objects are required", http.StatusBadRequest)
			return
		}

		// Build the raw IPC JSON manually so the C side receives the exact
		// structure it expects (arrays of objects with props arrays).
		objsJSON := "["
		for i, o := range req.Objects {
			if i > 0 {
				objsJSON += ","
			}
			propsJSON := "["
			for j, p := range o.Props {
				if j > 0 {
					propsJSON += ","
				}
				propsJSON += fmt.Sprintf("%d", p)
			}
			propsJSON += "]"
			objsJSON += fmt.Sprintf(
				`{"obj_type":%d,"obj_instance":%d,"props":%s}`,
				o.ObjType, o.ObjInstance, propsJSON)
		}
		objsJSON += "]"

		rawReq := fmt.Sprintf(
			`{"cmd":"rpm","device":%d,"objects":%s}`,
			req.Device, objsJSON)

		raw, err := dc.RawJSON(rawReq, 15*time.Second)
		if err != nil {
			jsonError(w, err.Error(), http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Write(raw)
	}
}
