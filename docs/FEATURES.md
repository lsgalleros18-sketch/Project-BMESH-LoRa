# BEMS Features

This document describes the features that already exist in the project, grouped by importance.

## Major Features

### 1. LoRa Mesh Messaging
The core system is an ESP32 + SX1278 LoRa mesh that sends emergency messages between nodes.

- Builds and transmits encrypted `BEMS|...|` mesh packets.
- Receives and decrypts mesh traffic.
- Supports direct messages and broadcast-style announcements.
- Uses a small fixed message queue for operator-sent traffic.

### 2. Secure Mesh Cryptography
The firmware protects mesh traffic with encryption and integrity checks.

- Derives AES and HMAC keys from the configured network key.
- Encrypts outbound payloads before radio transmission.
- Verifies inbound frames before accepting them as valid.
- Refuses mesh radio operation when the node is not provisioned.

### 3. Node Provisioning and Identity
The project stores node identity and setup data in NVS so the device can boot into a usable state.

- Persists node ID, node name, destination defaults, and location fields.
- Generates first-boot values for the web PIN, AP password, and network key when needed.
- Supports factory reset and setup-mode reconfiguration.
- Detects Node ID collisions during setup.

### 4. Captive Web Portal
Operators manage the node through a local browser-based portal.

- Provides setup, login, send-message, time-sync, and reset flows.
- Serves a captive portal on the Wi-Fi AP.
- Shows node status, message history, and mesh health data.
- Supports authenticated access for operational actions.

## Important Supporting Features

### 5. Delivery Tracking
The messaging flow includes sender-side delivery tracking for reliability.

- Tracks acknowledgements for unicast messages.
- Distinguishes between confirmed delivery and best-effort sending.
- Retries high-priority unicast traffic with explicit tracking.
- Uses repeated broadcasts for `ALL` destinations instead of ACK waiting.

### 6. Message Storage and Deduplication
The firmware keeps a bounded message history and prevents duplicate packet processing.

- Stores recent messages in a fixed-size table.
- Tracks seen packets to avoid processing the same message twice.
- Preserves packet age and status for UI display.
- Maintains packet counters and highest-seen IDs across reboots.

### 7. Mesh Health Visibility
The portal surfaces the current state of the node and nearby traffic.

- Shows node identity, AP status, client count, and time-sync state.
- Displays message threads and per-message status.
- Tracks known peers on-device with online/offline staleness state.
- Includes a mesh health section for current network visibility.
- Warns about possible duplicate Node IDs on the mesh.

### 8. Time Synchronization
Nodes can share a coarse time reference for consistent message timestamps.

- Accepts manual time sync from the portal.
- Broadcasts time sync packets through the mesh.
- Tracks whether the node is time synchronized.

## Minor Features

### 9. Wi-Fi Access Point Setup
The device starts as a local configuration hotspot.

- Creates a setup AP with a generated password.
- Uses WPA2 for AP association.
- Serves DNS redirection so common captive-portal checks resolve locally.

### 10. Login Session Handling
Portal sessions are managed with in-memory login state.

- Issues a session token on successful login.
- Expires inactive sessions after an idle timeout.
- Applies lockout delays after repeated failed PIN attempts.

### 11. Hardware Control
The firmware also manages a few device-level peripherals.

- Controls the SX1278 radio through SPI.
- Uses an RGB LED for visual feedback.
- Supports a boot/factory-reset button.

### 12. Built-in UI Convenience
The portal includes small operator-friendly helpers.

- Message templates for common emergency payloads.
- Language toggle in the message-sending view.
- Quick access to clock sync and mesh sync actions.

## Feature Groups At A Glance

- Core system: mesh messaging, cryptography, radio, provisioning.
- Operator system: web portal, login, setup, and message submission.
- Safety and reliability: delivery tracking, deduplication, collision detection, time sync.
- Device support: AP setup, LED feedback, reset handling, and NVS persistence.
