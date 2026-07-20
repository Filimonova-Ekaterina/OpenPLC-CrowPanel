CROWPANEL OPC UA TEST PACKAGE
=============================

This folder contains everything needed for a Windows test:

  FirmwareFlasher.exe       CrowPanel ESP32-P4 firmware installer
  OPCUA_Server.exe          Compressed-air station OPC UA simulator
  firmware\                 Firmware images used by the flasher
  configuration\            Editable simulator equipment and alarm settings


1. INSTALL THE CROWPANEL FIRMWARE
---------------------------------

1. Connect the CrowPanel to the Windows PC with a USB-UART data cable.
2. Close any serial monitor or other program using the CrowPanel COM port.
3. Double-click Firmware_Flasher.exe. The CrowPanel serial port is detected
   automatically.
4. Check that the detected COM port belongs to the CrowPanel.
5. Wait for "Flashing completed successfully". Do not disconnect power or USB
   while the firmware is being written.
6. Press Enter to close the flasher. The panel is reset automatically.

The package writes the bootloader, partition table, and application image. It
does not erase saved NVS settings, so Wi-Fi and OPC UA settings from an older
installation may still be present.

If no serial port is detected, check the USB cable, Windows USB/serial driver, and whether another application has the port open.


2. START THE OPC UA TEST SERVER
-------------------------------

1. Connect the Windows PC to the network that the CrowPanel will use.
2. Double-click OPCUA_Server.exe.
3. If Windows Firewall asks for permission, allow access on Private networks.
4. Keep the server window open during the test.
5. Find the PC IPv4 address by running "ipconfig" in Command Prompt. Use the
   Wi-Fi or Ethernet IPv4 address, for example 192.168.1.93.

The server listens on TCP port 4840. The endpoint entered on the panel must be:

  opc.tcp://<PC-IP>:4840/compressed-air-station/

Example:

  opc.tcp://192.168.1.93:4840/compressed-air-station/

Do not enter 0.0.0.0, localhost, or 127.0.0.1 on the CrowPanel. Those addresses do not identify the Windows PC from another device.


3. CONFIGURE THE CROWPANEL
--------------------------

1. Wait until the main interface is fully visible.
2. Tap the settings gear in the top-right corner.
3. Open the Wi-Fi tab.
4. Enable Wi-Fi, select the same network used by the Windows PC, enter the
   password, and wait until the screen reports that it is connected.
5. Open the OpenPLC tab.
6. Tap the Server URL card.
7. Enter the full endpoint shown above and tap Save.
8. Leave Settings with the Back button. The OPC UA client starts and connects
   automatically after Settings closes.

The Server URL badge shows DEFAULT until a user endpoint has been saved. It
shows CONFIGURED after saving.

Alternative endpoint setup methods on the OpenPLC tab:

  QR portal: after Wi-Fi is connected, scan the displayed QR code with a phone on the same network and submit the endpoint in the local setup page.

The System tab contains persistent volume, display brightness, and sleep-time
settings. These values are restored after restart and wake-up.


4. VERIFY THE TEST
------------------

A successful connection produces these visible results:

  - The server console reports a new OPC UA session.
  - The CrowPanel connection status changes to Connected.
  - Overview displays discovered equipment and live-tag counts.
  - Compressors and other equipment are generated from the server Address Space.
  - Live values such as demand, pressure, temperature, current, and power change smoothly.
  - Controls contains writable automatic-mode, run, fault-injection, and alarm-reset commands grouped by equipment.
  - Alarms shows the equipment and reason for each active alarm.

Control behavior:

  Automatic mode ON: compressor dispatch follows air demand.
  Run command: writing RUN or STOP switches that compressor to manual mode.
  Automatic mode ON again: returns the compressor to demand-based control.
  Inject sensor fault: creates invalid sensor readings and an active alarm.
  Reset all alarms: clears resettable alarm states.

5. TROUBLESHOOTING
------------------

Server unavailable:
  - Confirm that OPCUA_Server.exe is still running.
  - Confirm that both devices are on the same local network.
  - Confirm the PC IPv4 address and the complete opc.tcp:// URL.
  - Allow OPCUA_Server.exe through Windows Firewall on Private networks.
  - Confirm that TCP port 4840 is not used by another application.

Interface does not update:
  - Leave Settings; the OPC UA client is intentionally paused while Settings is open so the local setup portal can operate reliably.
  - Wait for automatic reconnection after Wi-Fi or sleep wake-up.

Stop the simulator:
  Press Ctrl+C in the OPC UA server window.
