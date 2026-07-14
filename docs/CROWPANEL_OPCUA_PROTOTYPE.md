# CrowPanel OPC UA auto-UI prototype

The firmware now uses this runtime pipeline:

```text
settings/wifi_ctrl -> opcua_client -> data_model -> ui_generator -> LVGL
```

No equipment object name, tag name, or compressor-specific screen is compiled
into the firmware. The OPC UA client starts at the standard Objects folder,
skips the standard Server diagnostics object, traverses namespace-zero grouping
folders, and recursively discovers custom Object and Variable nodes. Browse
continuation points are consumed with BrowseNext and released when traversal is
interrupted.

## Configuration

`CONFIG_OPCUA_SERVER_ENDPOINT` supplies the first-boot default:

```text
opc.tcp://192.168.1.100:4840/compressed-air-station/
```

On the panel, open `Settings -> OpenPLC`, enter a complete `opc.tcp://...` URL,
and press **Save and connect**. The URL is stored in NVS and the OPC UA client
reconnects immediately without rebooting. The firmware configuration menu also
configures connection and reconnect timeouts, subscription rate,
Browse depth, model capacity, and OPC UA task resources. These are transport
limits only; equipment structure still comes exclusively from OPC UA.

Wi-Fi credentials continue to be owned by the existing `settings` component.
The generated Settings tab opens its Wi-Fi screen. After the station receives
an IP address, the OPC UA task connects automatically.

## What appears on screen

- The top status bar shows Wi-Fi/OPC UA connection and Browse errors.
- Overview shows discovery totals and the first readable live signals.
- Every discovered Object gets an equipment tab.
- Read-only Boolean variables become red/green indicators.
- Writable Boolean variables become ON/OFF switches.
- Read-only numeric variables become bars and numeric values.
- Writable numeric variables become sliders and numeric values.
- Controls collects every writable variable without inspecting its name.
- Settings provides separate Wi-Fi and OpenPLC tabs. The OpenPLC tab edits the
  server endpoint with an on-screen keyboard.

The UI refresh timer copies model snapshots on the LVGL thread. Subscription
callbacks never call LVGL directly. Writes are queued to the OPC UA task so a
touch event cannot block on network I/O.

## Console diagnostics

Browse output uses this format:

```text
[Object] <DisplayName> (<NodeId>)
  [Variable] <DisplayName> | <NodeId> | R/RW | <type>
```

Connection, subscription, monitored-item, write, and reconnect failures include
their open62541 status name.

## open62541 component

`components/open62541` contains pinned official open62541 v1.4.14 sources and
generates a minimal amalgamation in the ESP-IDF build directory. Subscriptions
and client services are enabled; PubSub, encryption, historizing, examples, and
tests are disabled. The small `ESP_PLATFORM` port uses ESP-IDF/lwIP TCP sockets
and excludes unsupported `getifaddrs` multicast discovery. It never replaces or
rewrites the hostname entered by the user.

No ESP-IDF build, flash, monitor, serial, JTAG, or hardware command was run while
implementing this prototype.

## Quick functional test

1. Start `simulator/main.py` on a computer reachable from the CrowPanel network.
2. Open the persistent top status bar or the bottom `Settings` tab.
3. In `Wi-Fi`, connect the panel to the same network as the simulator.
4. In `OpenPLC`, enter
   `opc.tcp://<computer-ip>:4840/compressed-air-station/` and press
   `Save and connect`.
5. The top status must progress through `CONNECTING`, `BROWSING`, and
   `CONNECTED`. The console prints every discovered Object and Variable.
6. Equipment tabs and values appear automatically. A writable Boolean in
   `Controls` can be switched to verify OPC UA Write.

The computer firewall must allow inbound TCP port 4840. `127.0.0.1` is not a
valid simulator address from the CrowPanel because it refers to the panel
itself.
