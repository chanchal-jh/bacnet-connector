# --- Stage 1: Build the C Daemon ---
FROM debian:bookworm-slim AS c-builder

# Install build dependencies
RUN apt-get update && apt-get install -y cmake gcc g++ make git

WORKDIR /build
COPY . .

# Build bacnetd
WORKDIR /build/bacnetd
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# --- Stage 2: Build the Go API ---
FROM golang:1.22-bookworm AS go-builder

WORKDIR /build
COPY go.mod go.sum ./
RUN go mod download

COPY . .
# Build the Go binary statically (CGO_ENABLED=0 makes it easy to run on slim images)
RUN CGO_ENABLED=0 go build -a -installsuffix cgo -o bacnet-api ./cmd/api

# --- Stage 3: Final Production Image ---
FROM debian:bookworm-slim

WORKDIR /app

# Copy the compiled binaries from Stage 1 and Stage 2
COPY --from=c-builder /build/bacnetd/build/bacnetd /app/bacnetd
COPY --from=go-builder /build/bacnet-api /app/bacnet-api

# Default environment variables
ENV BACNETD_PATH=/app/bacnetd
ENV BACNETD_SOCKET=/tmp/bacnetd.sock
ENV HTTP_PORT=8080
ENV BACNET_IP_PORT=47808

# Expose HTTP (TCP) and BACnet (UDP) ports
EXPOSE 8080/tcp
EXPOSE 47808/udp

# Set the Go API as the entrypoint. It will automatically launch the C daemon!
ENTRYPOINT ["/app/bacnet-api"]

