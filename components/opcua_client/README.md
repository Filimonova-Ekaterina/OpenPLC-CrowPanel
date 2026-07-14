# OPC UA client

This component owns OPC UA communication only. It does not create LVGL objects
or interpret equipment names.

Implemented operations:

- connection and automatic reconnect;
- recursive Browse from the standard Objects folder;
- BrowseNext continuation-point handling;
- DataType and UserAccessLevel reads;
- initial Value reads;
- DataChange subscriptions;
- queued Boolean and numeric writes using the exact discovered OPC UA type.

The component depends on the vendored `components/open62541` component. The
current local simulator uses anonymous `SecurityPolicy None` communication.
