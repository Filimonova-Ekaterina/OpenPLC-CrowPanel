# OpenPLC CrowPanel HMI

Industrial HMI for the CrowPanel ESP32-P4. The firmware connects to an OPC UA
server, browses its Address Space, builds a transport-independent data model,
and generates the complete LVGL interface automatically. Equipment names,
NodeIds, tag counts, access rights, metrics, and controls are never tied to the
compressed-air example.

## Architecture

```text
OPC UA server <-> open62541 client <-> Data Model <-> Generated LVGL UI
                                      |           |-> Overview
                                      |           |-> Equipment
                                      |           |-> Controls
                                      |           |-> Trends
                                      |           `-> Alarms
                                      `-> semantic entity classification
```

Main components:

- `components/open62541` — pinned OPC UA client library and ESP-IDF/lwIP port.
- `components/opcua_client` — connection, Browse/BrowseNext, Read, subscriptions,
  events, queued Write operations, and reconnect handling.
- `components/data_model` — thread-safe OPC UA objects, tags, values, entity
  classification, and active alarms.
- `components/ui_generator` — Overview, Equipment, Controls, and Alarms UI.
- `components/trends` — automatically generated 60-second charts for readable
  numeric variables.
- `components/navigation` — persistent bottom-tab navigation.
- `components/settings` — Wi-Fi setup, local configuration portal, and saved OPC
  UA endpoint.
- `simulator` — Python OPC UA server used for development and integration tests.

## Protocol-driven identity and classification

Objects and variables are uniquely identified by their complete OPC UA
`NodeId`. `DisplayName` and `BrowseName` are presentation metadata only and are
not used as identity keys.

For each Object the client stores:

- `NodeId` and `TypeDefinition`;
- hierarchical reference type (`Organizes`, `HasComponent`, or root);
- optional `EntityKind` metadata;
- parent Object and directly owned Variables.

Supported semantic kinds are:

- `System` — a parent scope containing other objects;
- `Process` — a process/network context and source of aggregate measurements;
- `ActiveEquipment` — equipment with an operating state or commands;
- `PassiveEquipment` — a physical monitored asset without a run state;
- `Group` — an organizational object.

When `EntityKind` is absent, the firmware falls back to protocol information:
hierarchy, semantic roles, readable measurements, writable commands, and status
variables. It does not inspect example-specific words such as Compressor,
Receiver, or Air Network.

The included simulator publishes the following metadata:

| OPC UA object | EntityKind | UI behaviour |
| --- | --- | --- |
| Compressed Air Station | `System` | Parent plaque in Equipment Status |
| Compressors | `ActiveEquipment` | RUNNING/READY equipment nodes |
| Air Dryer | `ActiveEquipment` | RUNNING/READY equipment node |
| Air Receiver | `PassiveEquipment` | MONITORED passive asset |
| Air Network | `Process` | Process source in Live signals and Trends |

## Generated interface

- **Overview** shows system health, active alarms, running equipment, a parent
  system plaque, active/passive asset status, and relevant live process signals.
- **Equipment** contains physical active and passive assets only. System and
  process objects are not counted as equipment. Every readable, non-command
  variable remains visible there, including accumulated runtime counters.
- **Controls** contains every writable OPC UA Variable, including system-level
  commands.
- **Trends** creates two charts per row and shows the source, metric, current
  value, actual Y range, analysis state, and `60 s ago -> now` time direction.
  Accumulating runtime/operating-hours counters are excluded by semantic role.
- **Alarms** uses OPC UA `SourceNode` plus the semantic alarm code for identity.

Overview KPI selection is semantic and generic. It prefers process/system
sources for pressure or temperature, demand or flow, and power/load. The UI
groups those values under their deepest shared Object from the OPC UA hierarchy
and displays only each metric name and value inside the signal row.

Icons are embedded in the firmware and selected from the Variable semantic role;
they are never downloaded from a server. Built-in categories cover temperature
and dew point, pressure and vacuum, demand/load, flow, electrical values, power,
runtime, level, vibration, speed/RPM/frequency, humidity, position/valves,
counters, connectivity, maintenance, operating state, alarms, and control mode.
Unknown roles use a safe Boolean or numeric fallback icon and remain visible.

## Wi-Fi and reconnect behaviour

The OPC UA client starts only after Wi-Fi association and a valid DHCP address.
When the link is lost, it detects SecureChannel and Session state changes,
disposes of the stale client, waits for a new IP address, and creates a fresh
OPC UA session.

Internet access is not required: the HMI and OPC UA server only need LAN
reachability. Periodic external ICMP probes are disabled by default to avoid
unnecessary traffic and SDIO contention on the ESP32-P4 Wi-Fi link. Connection
status is derived from association, DHCP state, and RSSI.

## Configuration

Set the OPC UA endpoint with the gear button under **Settings > OpenPLC**. A
saved endpoint is stored in NVS and applied by reconnecting the client. The same
page exposes a local QR setup portal and accepts UART JSON:

```json
{"type":"openplc_config","endpoint":"opc.tcp://server:4840"}
```

See the [CrowPanel prototype guide](docs/CROWPANEL_OPCUA_PROTOTYPE.md) and
[simulator instructions](simulator/README.md) for setup and test procedures.
