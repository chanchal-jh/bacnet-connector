## Project Overview

The **BACnet REST Gateway** is a production-ready protocol adapter that translates BACnet/IP (a building automation protocol used for HVAC, lighting, and sensor systems) into a simple REST API that any frontend or cloud application can consume.

### The Problem it Solves
BACnet is a binary, UDP-based, broadcast-driven protocol — it cannot be consumed from a browser, mobile app, or cloud service without a translation layer. The Gateway sits on the edge of the building network, does all the heavy BACnet protocol lifting in C, and exposes clean REST endpoints via Go.

### Core Principle
> **Go is a passthrough proxy. It never transforms or validates BACnet data.**

The C daemon owns all BACnet protocol logic. Go owns HTTP routing, request lifecycle, timeouts, and process management. The C daemon's JSON is passed untouched to the HTTP client.