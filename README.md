# OpenPLC CrowPanel HMI

Industrial HMI prototype for CrowPanel ESP32-P4. The firmware connects to an
OPC UA server, browses its Address Space, stores discovered tags in a Data Model,
and generates LVGL equipment screens without hardcoded equipment names.

Active components:

- `components/open62541` — pinned OPC UA client library and ESP-IDF/lwIP port.
- `components/opcua_client` — Browse, BrowseNext, Read, Subscription, Write, and reconnect.
- `components/data_model` — thread-safe equipment and tag storage.
- `components/ui_generator` — generated indicators, values, and controls.
- `components/navigation` — generated bottom-tab navigation.
- `components/settings` — Wi-Fi setup and persistent OpenPLC endpoint settings.
- `simulator` — Python compressed-air OPC UA test server.

The OPC UA endpoint is configured with the gear button under
**Settings > OpenPLC**. The preconfigured endpoint is marked `default`; a saved
value is stored in NVS and applied immediately by reconnecting the client.
The same OpenPLC page exposes a local QR setup portal and accepts UART JSON in
the form `{"type":"openplc_config","endpoint":"opc.tcp://server:4840"}`.

See [CrowPanel prototype guide](docs/CROWPANEL_OPCUA_PROTOTYPE.md) and
[simulator instructions](simulator/README.md) for the test procedure.
