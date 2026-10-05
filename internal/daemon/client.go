package daemon

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// Request is a command sent to the C bacnetd over the Unix socket.
type Request struct {
	ID          string `json:"id"`
	Cmd         string `json:"cmd"`
	Device      uint32 `json:"device"`
	ObjType     uint16 `json:"obj_type"`
	ObjInstance uint32 `json:"obj_instance"`
	Prop        uint32 `json:"prop"`
	Priority    uint8  `json:"priority,omitempty"`
	ValType     string `json:"val_type,omitempty"`
	Value       string `json:"value,omitempty"`
}

// Client manages connection to the bacnetd Unix socket.
type Client struct {
	socketPath string
	mu         sync.Mutex
	counter    atomic.Uint64
}

// NewClient creates a Client for the given socket path.
func NewClient(socketPath string) *Client {
	return &Client{socketPath: socketPath}
}

// Raw sends req to the C daemon and returns the response JSON.
// It opens a new Unix socket connection per request so the C daemon
// is not blocked waiting for idle input and can shut down cleanly.
func (c *Client) Raw(req Request, timeout time.Duration) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	conn, err := net.Dial("unix", c.socketPath)
	if err != nil {
		return nil, fmt.Errorf("connect to bacnetd: %w", err)
	}
	defer conn.Close()

	scanner := bufio.NewScanner(conn)

	// Assign unique IPC request ID
	req.ID = fmt.Sprintf("r%d", c.counter.Add(1))

	data, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}

	// Write newline-delimited JSON to C daemon
	_ = conn.SetWriteDeadline(time.Now().Add(timeout))
	if _, err = fmt.Fprintf(conn, "%s\n", data); err != nil {
		return nil, fmt.Errorf("write: %w", err)
	}

	// Read single response line
	_ = conn.SetReadDeadline(time.Now().Add(timeout))
	if !scanner.Scan() {
		return nil, fmt.Errorf("read response: %w", scanner.Err())
	}

	// Strip only the internal IPC "id" field — pass everything else untouched.
	// We unmarshal into a raw map so we don't touch any values at all.
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(scanner.Bytes(), &raw); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}
	delete(raw, "id")

	return json.Marshal(raw)
}

// Discover is a convenience wrapper used by the devices list endpoint.
// It returns structured data since device discovery has a well-known shape.
func (c *Client) Discover() ([]byte, error) {
	return c.Raw(Request{Cmd: "discover"}, 10*time.Second)
}

// RawJSON sends an already-serialised JSON string to the C daemon.
// Used when the request cannot be expressed with the Request struct
// (e.g. ReadPropertyMultiple which has nested arrays).
func (c *Client) RawJSON(jsonStr string, timeout time.Duration) ([]byte, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	conn, err := net.Dial("unix", c.socketPath)
	if err != nil {
		return nil, fmt.Errorf("connect to bacnetd: %w", err)
	}
	defer conn.Close()

	scanner := bufio.NewScanner(conn)

	// Inject a unique ID into the JSON string
	id := fmt.Sprintf("r%d", c.counter.Add(1))
	// Insert "id":"..." as the first key after the opening brace
	injected := fmt.Sprintf(`{"id":"%s",%s`, id, jsonStr[1:])

	_ = conn.SetWriteDeadline(time.Now().Add(timeout))
	if _, err = fmt.Fprintf(conn, "%s\n", injected); err != nil {
		return nil, fmt.Errorf("write: %w", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(timeout))
	if !scanner.Scan() {
		return nil, fmt.Errorf("read response: %w", scanner.Err())
	}

	var raw map[string]json.RawMessage
	if err := json.Unmarshal(scanner.Bytes(), &raw); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}
	delete(raw, "id")

	return json.Marshal(raw)
}
