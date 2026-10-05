package api

import (
	"net/http"

	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"

	"github.com/chanchal/bacnet/internal/daemon"
)

// NewRouter wires all routes and returns a ready-to-serve http.Handler.
func NewRouter(dc *daemon.Client) http.Handler {
	r := chi.NewRouter()

	r.Use(middleware.Logger)
	r.Use(middleware.Recoverer)
	r.Use(middleware.RequestID)

	// Health check
	r.Get("/health", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})

	// BACnet API
	r.Route("/api", func(r chi.Router) {
		// Discover devices
		r.Get("/devices", devicesHandler(dc))

		// Easy API for the Device Object itself
		r.Get("/devices/{deviceID}/info", deviceInfoHandler(dc))

		// List objects in a device
		r.Get("/devices/{deviceID}/objects", objectListHandler(dc))

		// Generic property read
		r.Post("/read-property", genericReadHandler(dc))

		// ReadPropertyMultiple — fetch multiple props from multiple objects in one call
		r.Post("/read-property-multiple", rpmHandler(dc))

		// Generic property write
		r.Put("/write-property", writeHandler(dc))
	})

	return r
}
