package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/chanchal/bacnet/internal/api"
	"github.com/chanchal/bacnet/internal/daemon"
)

func main() {
	socketPath := getEnv("BACNETD_SOCKET", "/tmp/bacnetd.sock")
	httpPort := getEnv("HTTP_PORT", "8080")
	bacnetdPath := getEnv("BACNETD_PATH", "./bacnetd/build/bacnetd")

	// DISCOVERY_INTERVAL controls how often (in seconds) the gateway
	// re-broadcasts a global Who-Is to refresh the device cache.
	// Default: 300 seconds (5 minutes). Set to 0 to disable periodic re-discovery.
	discoveryIntervalSec, _ := strconv.Atoi(getEnv("DISCOVERY_INTERVAL", "300"))

	// ----------------------------------------------------------------
	// 1. Launch the C daemon automatically
	// ----------------------------------------------------------------
	log.Printf("[api] BACnet Gateway starting")
	log.Printf("[api] Starting C daemon: %s", bacnetdPath)

	// Remove old socket file if it exists so we don't conflict
	os.Remove(socketPath)

	cmd := exec.Command(bacnetdPath, socketPath)
	cmd.Stdout = os.Stdout // Pipe C logs directly to Docker logs
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		log.Fatalf("[api] Failed to start bacnetd: %v", err)
	}

	// Give the C daemon time to initialize and bind the Unix socket
	time.Sleep(500 * time.Millisecond)

	// Ensure the C daemon is killed gracefully when the Go app shuts down
	defer func() {
		log.Println("[api] Terminating C daemon...")
		if cmd.Process != nil {
			cmd.Process.Kill()
			cmd.Wait()
		}
	}()

	log.Printf("[api] Connecting to bacnetd at %s", socketPath)
	dc := daemon.NewClient(socketPath)

	// ----------------------------------------------------------------
	// 2. Layer 1 — Startup warm-up: run global discovery immediately
	//    in the background so the cache is warm before any client hits the API.
	// ----------------------------------------------------------------
	go func() {
		log.Println("[discovery] Running startup Who-Is broadcast...")
		if _, err := dc.Discover(); err != nil {
			log.Printf("[discovery] Startup discovery error: %v", err)
		} else {
			log.Println("[discovery] Startup discovery complete")
		}
	}()

	// ----------------------------------------------------------------
	// 3. Layer 2 — Periodic re-discovery: refresh the cache on a schedule
	//    to catch new devices or devices that came back after a reboot.
	// ----------------------------------------------------------------
	if discoveryIntervalSec > 0 {
		go func() {
			ticker := time.NewTicker(time.Duration(discoveryIntervalSec) * time.Second)
			defer ticker.Stop()
			for range ticker.C {
				log.Println("[discovery] Running scheduled Who-Is broadcast...")
				if _, err := dc.Discover(); err != nil {
					log.Printf("[discovery] Scheduled discovery error: %v", err)
				} else {
					log.Println("[discovery] Scheduled discovery complete")
				}
			}
		}()
		log.Printf("[discovery] Periodic re-discovery every %ds (set DISCOVERY_INTERVAL=0 to disable)", discoveryIntervalSec)
	} else {
		log.Println("[discovery] Periodic re-discovery disabled (DISCOVERY_INTERVAL=0)")
	}

	// ----------------------------------------------------------------
	// 4. Start the HTTP server
	// ----------------------------------------------------------------
	router := api.NewRouter(dc)

	srv := &http.Server{
		Addr:         fmt.Sprintf(":%s", httpPort),
		Handler:      router,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Graceful shutdown
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		log.Printf("[api] Listening on http://localhost:%s", httpPort)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("[api] Server error: %v", err)
		}
	}()

	<-quit
	log.Println("[api] Shutting down...")

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	if err := srv.Shutdown(ctx); err != nil {
		log.Fatalf("[api] Forced shutdown: %v", err)
	}
	log.Println("[api] Done")
}

func getEnv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
