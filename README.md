# Pixoo WiFi Provisioner

An **unofficial** demo helper for Divoom devices such as Pixoo64 or Times Gate, which delivers WiFi credentials to the device over Bluetooth Low Energy (BLE) — the same way the official Divoom mobile application does it.

> [!NOTE]
> This project is not affiliated with, endorsed by, sponsored by, or supported by Divoom. "Divoom", "Pixoo64", "Times Gate", and related names are trademarks or registered trademarks of their respective owners. They are used here only to identify the devices that this project is designed to interoperate with.

## Problem
Divoom devices need to be connected to a WiFi network before they can be controlled. The official way to provide WiFi credentials to a device is through the Divoom mobile application.

However, there are scenarios where using the official application is impractical or impossible:
- **Privacy-focused setups.** Users who prefer not to create a Divoom account solely for the purpose of initial device configuration, or who want to avoid granting the official app the permissions it requires and sending any data to the external servers during setup.
- **Local-only provisioning.** Users who want to connect a device they own to an isolated or offline WiFi network that is used only for local control. In such setups, the device is not intended to communicate with the external servers, and the only required step is to provide it with the correct WiFi SSID and password.
- **Offline or network-restricted environments.** In labs, workshops, classrooms, travel setups, corporate networks, or secured environments, internet access may be unavailable, unreliable, filtered, or intentionally disabled. In such cases, provisioning the device directly to a local network can be more practical than relying on an account-based mobile onboarding flow.
- **Automated or headless deployments.** Scenarios where multiple devices need to be batch-provisioned, or where the provisioning step is part of a larger automated setup pipeline — for example, combined with firmware configuration — and manual interaction with a smartphone app each time is impractical.
- **Home automation systems.** Integration with local platforms such as Home Assistant, where the entire setup is meant to be reproducible, scriptable, and free from dependency on a mobile app or external services.
- **Development and experimentation.** Developers and researchers may need a reproducible way to provision their own device while analyzing its behavior, testing custom integrations, or validating local-control workflows. Avoiding a mobile-app-dependent setup can make repeated testing simpler and more predictable.
- **Long-term device usability.** Ensuring that the device can be re-provisioned after a factory reset regardless of the future availability of Divoom's server infrastructure, app store presence, or account system.

## Analysis
By analyzing BLE traffic and the device’s GATT profile, we can understand how the official Divoom application communicates with the device over BLE.

For this purpose, we can use, for example:
- [nRF52840 Dongle](https://www.nordicsemi.com/Products/Development-hardware/nRF52840-Dongle)
- [nRF Connect for Desktop](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop) with [BLE app](https://docs.nordicsemi.com/bundle/nrf-connect-ble/page/index.html)
- [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) with [ble-sniffer command](https://docs.nordicsemi.com/bundle/nrfutil/page/nrfutil-ble-sniffer/nrfutil-ble-sniffer.html)
- and [Wireshark](https://www.wireshark.org/).

The BLE app in nRF Connect for Desktop can be used to discover the device’s service and characteristic UUIDs:

![Screenshot of nRF Connect for Desktop showing a connected Divoom Times Gate, with its discovered GATT service and two writable/notify characteristics](/times_gate_ble_discovery.png)

The service UUID `49535343-FE7D-4AE5-8FA9-9FAFD205E455` and the characteristic UUID `49535343-1E4D-4BD9-BA61-23C647249616` observed during discovery suggest that the device implements the Microchip Transparent UART service — a proprietary GATT profile originally defined for Microchip's RN4870/71 BLE modules and documented in the [RN4870/71 User's Guide](https://ww1.microchip.com/downloads/en/DeviceDoc/50002466B.pdf). This service provides a simple BLE UART bridge: data written to the writable characteristic is forwarded over a UART-like channel, and data received from the device is sent back to the client as notifications.

The specified characteristic is used to communicate with the device using two different message formats [documented by Divoom](https://docin.divoom-gz.com/web/#/5/146).

### Observation #1: WiFi Config Command
To provision the device with WiFi credentials, a command with code `0xF2` is written to the characteristic. The command uses the "new iOS LE" message format with the following frame layout:

| # | Size | Description |
| --- | --- | --- |
| 1 | 4 | Header `0xFE 0xEF 0xAA 0x55` |
| 2 | 2 | Payload size |
| 3 | 1 | Acknowledgement flag (`0x00` or `0x01`) |
| 4 | 1 | Command type |
| 5 | 4 | Packet number (optional) |
| 6 | N | Command data (optional) |
| 7 | 2 | Checksum |
| 8 | 1 | Footer `0x02`

**Payload size** is the size of the frame body, excluding the header, checksum, and footer. It includes the two-byte payload size field itself.  
**Checksum** is the sum of all frame bytes, excluding the header, footer, and the checksum itself.  
All multi-byte components (e.g., payload size or checksum) are represented in little-endian.  
If the command frame exceeds the negotiated ATT MTU data size, it must be split into MTU-sized chunks. A pause between chunks gives the device time to reassemble the frame.

The command data for the WiFi config command carries the WiFi credentials in the following format:

| # | Size | Description |
| --- | --- | --- |
| 1 | 1 | WiFi SSID size |
| 2 | N | WiFi SSID as UTF-8 |
| 3 | 1 | WiFi password size |
| 4 | M | WiFi password as UTF-8 |
| 5 | 4 | User ID |

`User ID` can be set to `0` when operating without an account.
Thus, the complete frame for the WiFi config command has the following layout:

| # | Size | Description |
| --- | --- | --- |
| 1 | 4 | Header `0xFE 0xEF 0xAA 0x55` |
| 2 | 2 | Payload size |
| 3 | 1 | Acknowledgement flag `0x00` (without acknowledgement) |
| 4 | 1 | Command type `0xF2` |
| 5 | 1 | WiFi SSID size |
| 6 | N | WiFi SSID as UTF-8 |
| 7 | 1 | WiFi password size |
| 8 | M | WiFi password as UTF-8 |
| 9 | 4 | User ID `0` |
| 10 | 2 | Checksum |
| 11 | 1 | Footer `0x02`

**Example:** To set the WiFi credentials `WiFiSSID` and `Password`, send a message frame consisting of the following parts:
| # | Bytes | Size | Description |
| --- | --- | --- | --- |
| 1 | `0xFE 0xEF 0xAA 0x55` | 4 | Header |
| 2 | `0x1A 0x00` | 2 | Payload size: 2 for payload size + 1 for acknowledgement flag + 1 for command type + 1 for WiFi SSID size + 8 for WiFi SSID `WiFiSSID` + 1 for WiFi password size + 8 for WiFi password `Password` + 4 for User ID = 26 = `0x1A` |
| 3 | `0x00` | 1 | Acknowledgement flag: `0x00` for the command without acknowledgement |
| 4 | `0xF2` | 1 | Command type: `0xF2` for the WiFi config command |
| 5 | `0x08` | 1 | WiFi SSID size: 8 for `WiFiSSID` as UTF-8 |
| 6 | `0x57 0x69 0x46 0x69 0x53 0x53 0x49 0x44` | 8 | WiFi SSID: `WiFiSSID` as UTF-8 |
| 7 | `0x08` | 1 | WiFi password size: 8 for `Password` as UTF-8 |
| 8 | `0x50 0x61 0x73 0x73 0x77 0x6F 0x72 0x64` | 8 | WiFi password: `Password` as UTF-8 |
| 9 | `0x00 0x00 0x00 0x00` | 4 | User ID: `0` |
| 10 | `0x11 0x07` | 2 | Checksum: sum from #2 to #9 = `1809` = `0x0711` = `0x11 0x07` little-endian |
| 11 | `0x02` | 1 | Footer |

### Observation #2: WiFi Status Notifications
After the WiFi config command is sent, the device replies with status notifications using command code `0xF3`. These notifications use the "old" message format with the following frame layout:

| # | Size | Description |
| --- | --- | --- |
| 1 | 1 | Header `0x01` |
| 2 | 2 | Payload size |
| 3 | 1 | Command type prefix `0x04` |
| 4 | 1 | Command type |
| 5 | 1 | Acknowledgement status `0x55` |
| 6 | N | Response data (optional) |
| 7 | 2 | Checksum |
| 8 | 1 | Footer `0x02`

The WiFi connection status byte is located at index `[6]` of the notification frame and takes one of the following values:

| Value | Meaning |
| --- | --- |
| `0` | The device is connecting to the WiFi network |
| `1` | The WiFi network with the specified SSID was not found |
| `2` | The specified WiFi password is incorrect |
| `3` | The connection to the WiFi network has been established |
| `4` | Failed to connect to the WiFi network |
| `5` | The device has connected to the Divoom server |

Status value `3` indicates that the device has connected to the WiFi network. Status value `5` indicates that it has also connected to the server. For local-only setups, receiving `3` may already be sufficient. The device may send several intermediate status updates before reaching a terminal state.


## Solution

To provision a Divoom device with WiFi credentials without the official app, we can:
- Scan for BLE advertisements from devices with known internal Divoom product names, or from a specific device if its MAC address is known.
- Once a device is found, establish a GATT connection and use the known service and characteristic UUIDs, or discover them dynamically through GATT service discovery.
- Subscribe to BLE notifications on the characteristic to receive WiFi status updates.
- Send the WiFi config command carrying the desired WiFi SSID and password.
- Listen for incoming WiFi status notifications and wait until a success status is received.

This project contains the source code of an Arduino IDE sketch for ESP32 that implements the described approach. Do not forget to change the WiFi network SSID and password in the code to the ones you want to use for your device.

## Notes
- This project is demonstrational and prioritizes simplicity over efficiency. In particular, it is designed to work with a **single device** at a time.
- The project was tested on Pixoo64 with firmware version 92057 and on Times Gate with firmware version 4000118. There is reason to believe that this approach may work with other Divoom devices that are configured through the official Divoom application. However, future firmware versions may introduce changes that make this approach unusable.
- The project was developed and tested on [Arduino Nano ESP32](https://store-usa.arduino.cc/products/nano-esp32) (ESP32-S3), but it can be run on many other boards. The approach described in the project can also be used to implement a similar mechanism in other languages and on other platforms.
- The device must be in provisioning mode and have BLE advertising active for the scan to find it. The device is typically in this mode when it is new out of the box or after a factory reset. To reset a Divoom device, you usually need to press and hold the power button for about 8 seconds. However, the reset procedure may differ between Divoom devices or change in newer hardware or firmware versions.

## Legal & Usage Notice
This software is intended only for use with devices that you own or have explicit permission to control and for educational, research or personal-use purposes. Do not use this project to configure Divoom devices without authorization.

This project does not:
- require or provide access to Divoom servers or Divoom user accounts;
- include proprietary source code, firmware images, application binaries, credentials, tokens, certificates, secrets, or confidential materials from Divoom;
- bypass paid features, licensing mechanisms, or digital content restrictions;
- attack, disrupt, or interfere with third-party networks or services.

Use this software at your own risk. The author is not responsible for any damage to hardware, software, data, accounts, networks, or services, or for any violation of applicable laws, regulations, or terms of service resulting from the use of this project.
