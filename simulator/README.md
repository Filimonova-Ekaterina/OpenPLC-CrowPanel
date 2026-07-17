# Compressed-air OPC UA simulator

Python 3.9+ simulator for testing the CrowPanel without industrial equipment.
It publishes a self-describing compressed-air station and updates its values
every 100 ms.

## Start

From `simulator`:

```powershell
python -m venv .venv
.\.venv\Scripts\python -m pip install -r requirements.txt
.\.venv\Scripts\python main.py
```

The server listens on:

```text
opc.tcp://0.0.0.0:4840/compressed-air-station/
```

On the CrowPanel, replace `0.0.0.0` with the computer's LAN address, for example:

```text
opc.tcp://192.168.1.50:4840/compressed-air-station/
```

Allow inbound TCP port 4840 in the computer firewall. The panel and computer
must be reachable on the same network.

## Address Space

All Object, Variable, DisplayName, access-level, engineering-unit, and range
metadata is created from `configuration/equipment.yaml`. Writable command nodes
allow compressor control, sensor-fault injection, and alarm reset. Alarm state
changes also emit OPC UA events.

Each compressor exposes `AutomaticMode` and `runCommand`. In automatic mode the
run command mirrors the actual running state. Writing RUN or STOP from the HMI
first disables automatic mode and then applies the requested state; enabling
`AutomaticMode` returns the compressor to demand-based dispatch. `SemanticRole`
properties let a generic client associate commands, operating status, alarms,
and overview measurements without hardcoded equipment names.

Run the model tests without starting hardware tools:

```powershell
.\.venv\Scripts\python -m unittest discover -v
```
