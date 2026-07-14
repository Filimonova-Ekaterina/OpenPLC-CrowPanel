# open62541 ESP-IDF component

This component vendors the official open62541 **v1.4.14** source and generates
an amalgamated client library in the ESP-IDF build directory.

Enabled features:

- OPC UA TCP client with SecurityPolicy None
- Browse, Read, and Write services
- DataChange subscriptions and monitored items
- status-code descriptions for UART diagnostics

Disabled features include server event subscriptions, PubSub, historizing,
encryption, JSON/XML encoding, examples, tests, and tools. The minimal Namespace
Zero setting affects only local server information-model generation; it does not
limit browsing a remote server's Address Space.

The component uses the POSIX-compatible TCP socket API supplied by ESP-IDF/lwIP.
The vendored POSIX layer contains a small `ESP_PLATFORM` compatibility patch:
`getifaddrs`-based multicast interface selection and the unused UDP manager are
excluded on ESP-IDF. No hostname replacement shim is used, so the endpoint
entered in **Settings > OpenPLC** is resolved and connected as written.

Upstream source: <https://github.com/open62541/open62541/tree/v1.4.14>

Licenses are preserved in `vendor/LICENSE` and `vendor/LICENSE-CC0`.
