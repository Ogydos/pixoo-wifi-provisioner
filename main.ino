/*
 * DIVOOM WIFI PROVISIONER (ESP32)
 *
 * Connects to a Divoom device and sends it WiFi credentials (SSID and
 * password) so the device can join your home network — the same way the
 * official Divoom application does it.
 *
 * How it works:
 *   1. The ESP32 scans for BLE advertisements from known Divoom device names.
 *   2. Once a device is found, a GATT connection is made.
 *   3. WiFi config command (0xF2) is sent over BLE, carrying the SSID, 
 *      password, and user ID.
 *   4. The device attempts to connect to the WiFi network and sends WiFi
 *      status notifications (0xF3) back to report progress.
 *   5. When the status reports success, provisioning is done.
 *
 * Libraries:
 *   - NimBLE-Arduino 2.5.0 by h2zero (https://github.com/h2zero/NimBLE-Arduino)
 */

#include <NimBLEDevice.h>

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────

// WiFi credentials to deliver to the device
const char* WIFI_SSID     = "WiFiSSID";
const char* WIFI_PASSWORD = "Password";

// BLE service and characteristic UUIDs
const char* SERVICE_UUID        = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
const char* CHARACTERISTIC_UUID = "49535343-1e4d-4bd9-ba61-23c647249616";

// Сommand byte codes
const uint8_t COMMAND_WIFI_CONFIG = 0xF2;
const uint8_t COMMAND_WIFI_STATUS = 0xF3;

// WiFi status codes reported by the device
const uint8_t WIFI_STATUS_CONNECTING          = 0;
const uint8_t WIFI_STATUS_SSID_NOT_FOUND      = 1;
const uint8_t WIFI_STATUS_WRONG_PASSWORD      = 2;
const uint8_t WIFI_STATUS_CONNECTED           = 3;
const uint8_t WIFI_STATUS_CONNECTION_FAILED   = 4;
const uint8_t WIFI_STATUS_CONNECTED_TO_SERVER = 5;

// Divoom device names
const char* DEVICE_NAMES[] = {
	"TimeBox", "Pixoo", "Timoo", "Tivoo", "Ditoo", "Pixel-Factory",
	"Planet", "Pixoo-Backpack", "Pixel-Bag", "Pixoo-VJ",
	"Pixoo-SlingBag", "Divoom-Backpack-M", "Divoom-Backpack",
	"Divoom-Backpack-S", "Zooe", "Di-Da", "Pixoo64", "Pixoo64-Blue",
	"StarSpark", "Karaoke", "Divoom & BIPI", "PixooLCDWIFI",
	"CyberBag", "DiPow", "Divoom Tiivoo 2", "PhotoFrameConfig",
	"Cyberbag", "Divoom D-base", "Timebox-Evo", "Divoom MiniToo",
	"Divoom Emodo", "Divoom FlowToo",
	nullptr  // Marks the end of the list
};

// State machine states
enum class State {
	SCANNING_AND_CONNECTING,  // Scanning for a device; connecting once found
	CONFIGURING_WIFI,         // WiFi config command sent; waiting for WiFi status notification
	WIFI_CONFIGURED,          // Device reported WiFi connection success
	DONE                      // Provisioning complete
};

// ─────────────────────────────────────────────
//  Global objects
// ─────────────────────────────────────────────

static NimBLEClient* client = nullptr;
static NimBLEAddress address{};
static NimBLERemoteCharacteristic* characteristic = nullptr;
static volatile State state = State::SCANNING_AND_CONNECTING;

#include <NimBLEDevice.h>

/*
 * BLE scan callback.
 * onResult() fires for every advertisement
 * packet received during the active scan window.
 */
struct ScanCallbacks : public NimBLEScanCallbacks {
	void onResult(const NimBLEAdvertisedDevice* bleAdvertisedDevice) override {
		std::string bleAdvertisedDeviceName = bleAdvertisedDevice->getName();

		// Skip devices that do not broadcast a name
		if (bleAdvertisedDeviceName.empty()) return;

		// Check whether the name starts with any known Divoom prefix
		for (int i = 0; DEVICE_NAMES[i] != nullptr; i++) {
			if (bleAdvertisedDeviceName.rfind(DEVICE_NAMES[i], 0) == 0) {
				Serial.printf("Device found: \"%s\" (MAC address: %s, RSSI: %d dBm)\n",
					bleAdvertisedDeviceName.c_str(),
					bleAdvertisedDevice->getAddress().toString().c_str(),
					bleAdvertisedDevice->getRSSI());
				// Save the address
				address = bleAdvertisedDevice->getAddress();
				// Stop the scan immediately
				NimBLEDevice::getScan()->stop();
				return;
			}
		}
	}
} bleScanCallbacks;

// ─────────────────────────────────────────────
//  BLE messaging
// ─────────────────────────────────────────────

/*
 * Sends a raw byte array to the device's characteristic.
 * If the frame fits within the negotiated ATT MTU payload it is sent in a
 * single write. Otherwise it is split into MTU-sized chunks with a pause
 * between each chunk to give the device time to process them.
 * Returns true on success, false if the characteristic is unavailable or
 * the write fails.
 */
static bool bleWrite(const uint8_t* data, int dataSize) {
	// Guard against calling this before a connection is established
	if (!characteristic) return false;
	if (!client || !client->isConnected()) return false;

	// ATT MTU minus 3 bytes of ATT header overhead = max payload per packet
	int maxDataSize = client->getMTU() - 3;

	if (dataSize <= maxDataSize) {
		// Happy path: the whole frame fits in one ATT write packet
		return characteristic->writeValue(data, dataSize, false);
	}

	// Chunked path: split into pieces
	int offset = 0;
	while (offset < dataSize) {
		int chunkSize = min(maxDataSize, dataSize - offset);
		if (!characteristic->writeValue(data + offset, chunkSize, false)) {
			return false;
		}
		offset += chunkSize;
		// Pause between chunks so the device can reassemble the frame
		if (offset < dataSize) {
			delay(500);
		}
	}

	return true;
}

/*
 * Assembles one BLE command frame into the buffer.
 * This implements the "new iOS BLE" message format documented by Divoom:
 * https://docin.divoom-gz.com/web/#/5/146
 *
 * Command frame layout:
 *
 *   Size   Description
 *   ────   ───────────────────────────────────
 *   4      Header (0xFE 0xEF 0xAA 0x55)
 *   2      Payload size
 *   1      Acknowledgement flag (0x00 or 0x01)
 *   1      Command type
 *   4      Packet number (optional)
 *   N      Command data (optional)
 *   2      Checksum
 *   1      Footer (0x02)
 *
 * Payload size is calculated as the size of the entire frame excluding
 * the header, footer, and checksum. However, it does include the size of 
 * the field itself within the frame.
 * Checksum is calculated as the sum of all frame data except the header, 
 * footer, and the checksum itself.
 * Returns total frame length in bytes, or -1 if the out buffer is too small. 
 */
static int buildCommandFrame(uint8_t command, bool acknowledgementFlag, uint32_t packetNum, const uint8_t* commandData, int commandDataSize, uint8_t* out, int outSize) {
	int totalSize = commandDataSize + (acknowledgementFlag ? 15 : 11);

	// Return -1 if out buffer too small
	if (totalSize > outSize) return -1;

	int outPos = 0;

	// Header (4 bytes)
	out[outPos++] = 0xFE;
	out[outPos++] = 0xEF;
	out[outPos++] = 0xAA;
	out[outPos++] = 0x55;

	// Payload size (2 bytes)
	int payloadSize = commandDataSize + (acknowledgementFlag ? 8 : 4);
	out[outPos++] = lowByte(payloadSize);
	out[outPos++] = highByte(payloadSize);

	// Acknowledgement flag (1 byte)
	out[outPos++] = acknowledgementFlag ? 0x01 : 0x00;

	// Command type (1 byte)
	out[outPos++] = command;

	// Packet num (4 bytes, optional)
	if (acknowledgementFlag) {
		out[outPos++] = lowByte(packetNum);
		out[outPos++] = highByte(packetNum);
		out[outPos++] = lowByte(packetNum >> 16);
		out[outPos++] = highByte(packetNum >> 16);
	}

	// Command data (N bytes, optional)
	if (commandData != nullptr && commandDataSize > 0) {
		for (int i = 0; i < commandDataSize; i++) {
			out[outPos++] = commandData[i];
		}
	}

	// Checksum (2 bytes)
	uint16_t checksum = 0;
	for (int i = 4; i < (acknowledgementFlag ? 12 : 8) + commandDataSize; i++) {
		checksum += out[i];
	}
	out[outPos++] = lowByte(checksum);
	out[outPos++] = highByte(checksum);

	// Footer (1 byte)
	out[outPos] = 0x02;

	return totalSize;
}

/*
 * Sends complete command frame to the device.
 * Returns true if the write succeeded, false otherwise.
 */
static bool sendCommand(uint8_t command, bool acknowledgementFlag, uint32_t packetNum, const uint8_t* commandData, int commandDataSize) {
	uint8_t frame[128];
	// Zero the buffer before building the frame
	memset(frame, 0, sizeof(frame));

	int frameSize = buildCommandFrame(command, acknowledgementFlag, packetNum, commandData, commandDataSize, frame, sizeof(frame));
	if (frameSize < 0) {
		Serial.println("Command frame buffer is too small");
		return false;
	}

	// Print the complete frame as a hex string for debugging
	Serial.print("Outgoing command frame:");
	for (int i = 0; i < frameSize; i++) Serial.printf(" %02X", frame[i]);
	Serial.println();

	return bleWrite(frame, frameSize);
}

/*
 * Builds and sends WiFi config command.
 *
 * Command data layout:
 *
 *   Size   Description
 *   ────   ───────────────────────────────────
 *   1      WiFi SSID size
 *   N      WiFi SSID as UTF-8
 *   1      WiFi password size
 *   M      WiFi password as UTF-8
 *   4      User ID (0)
 *
 * Returns true if the write succeeded, false otherwise. 
 */
static bool sendWifiConfigCommand() {
	int ssidLength = strlen(WIFI_SSID);
	int passwordLength = strlen(WIFI_PASSWORD);

	// Use 0 for user ID when operating without an account
	uint32_t userId = 0;

	// Total command data size: 1 (SSID length field) + SSID length +
	// 1 (password length field) + password length + 4 (user ID)
	int commandDataSize = 1 + ssidLength + 1 + passwordLength + 4;
	uint8_t commandData[128];
	int pos = 0;

	// SSID length (1 byte)
	commandData[pos++] = (uint8_t)ssidLength;
	// SSID (N bytes)
	for (int i = 0; i < ssidLength; i++) {
		commandData[pos++] = (uint8_t)WIFI_SSID[i];
	}

	// Password length (1 byte)
	commandData[pos++] = (uint8_t)passwordLength;
	// Password (M bytes)
	for (int i = 0; i < passwordLength; i++) {
		commandData[pos++] = (uint8_t)WIFI_PASSWORD[i];
	}

	// User ID (4 bytes)
	commandData[pos++] = lowByte(userId);
	commandData[pos++] = highByte(userId);
	commandData[pos++] = lowByte(userId >> 16);
	commandData[pos++] = highByte(userId >> 16);

	return sendCommand(COMMAND_WIFI_CONFIG, false, 0, commandData, commandDataSize);
}

/*
 * BLE notification callback. 
 * Invoked on every incoming BLE notification from the device.
 * The device sends status updates using the "old" protocol format
 * documented by Divoom: https://docin.divoom-gz.com/web/#/5/146
 *
 * Notification frame layout:
 * 
 *   Size   Description
 *   ────   ─────────────────────────────────────
 *   1      Header (0x01)
 *   2      Payload size
 *   1      Command type prefix (0x04)
 *   1      Command type
 *   1      Acknowledgement status (0x55) 
 *   N      Command data
 *   2      Checksum
 *   1      Footer (0x02)
 */
static void notifyCallback(NimBLERemoteCharacteristic* bleCharacteristic, const uint8_t* data, size_t dataSize, bool isNotify) {
	// Validate that this looks like an "old" protocol frame:
	// starts with 0x01, ends with 0x02
	if (data[0] == 0x01 && data[dataSize - 1] == 0x02) {
		// We will not verify checksum for simplicity
		// Command type is at byte index [4]
		uint8_t command = data[4];
		if (command == COMMAND_WIFI_STATUS) {
			Serial.print("Incoming response frame:");
			for (int i = 0; i < dataSize; i++) Serial.printf(" %02X", data[i]);
			Serial.println();
			// WiFi connection status is at byte index [6]
			uint8_t status = data[6];
			const char* description;
			switch (status) {
				case WIFI_STATUS_CONNECTING:
					description = "The device is connecting to the WiFi network...";
					break;
				case WIFI_STATUS_SSID_NOT_FOUND:
					description = "The WiFi network with the specified SSID was not found";
					break;
				case WIFI_STATUS_WRONG_PASSWORD:
					description = "The specified WiFi password is incorrect";
					break;
				case WIFI_STATUS_CONNECTED:
					description = "The connection to the WiFi network has been established";
					break;
				case WIFI_STATUS_CONNECTION_FAILED:
					description = "Failed to connect to the WiFi network";
					break;
				case WIFI_STATUS_CONNECTED_TO_SERVER:
					description = "The device has connected to the Divoom server";
					break;
				default:
					description = "Unknown status";
					break;
			}
			Serial.printf("WiFi status update received: %s (%d)\n", description, status);
			// Two statuses indicate success
			if (status == WIFI_STATUS_CONNECTED || status == WIFI_STATUS_CONNECTED_TO_SERVER) {
				// The main loop will see the state change on its next iteration
				state = State::WIFI_CONFIGURED;
			}
			return;
		}
	}
}

/*
 * Cleans up any existing BLE connection and restarts the device scan
 * Called whenever a connection attempt or service discovery step fails
 */
static void restartScan() {
	Serial.println("Restarting scan...");

	// Disconnect and destroy the client if one exists
	if (client) {
		if (!client->isConnected()) client->disconnect();
		NimBLEDevice::deleteClient(client);
		client = nullptr;
	}

	// Release the characteristic reference
	if (characteristic) {
		characteristic->unsubscribe();
		characteristic = nullptr;
	}

	// Clear the saved device address
	address = NimBLEAddress{};

	// Brief pause before re-scanning so the device has time to recover
	delay(5000);

	state = State::SCANNING_AND_CONNECTING;
	Serial.println("Scanning...");
	NimBLEDevice::getScan()->start(15000);
}

// ─────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────

void setup() {
	Serial.begin(115200);

	// Wait for the Serial Monitor to connect
	unsigned long start = millis();
	while (!Serial && (millis() - start < 3000));

	Serial.println("##########################");
	Serial.println("# PIXOO WIFI PROVISIONER #");
	Serial.println("##########################");
	Serial.println(" ");

	// Initialise the NimBLE stack
	NimBLEDevice::init("");

	// Ask for a larger ATT MTU so frames can be sent in one packet.
	// If the device accepts, getMTU() will return 247 after connecting.
	// If not, bleWrite() will split the frame into 20-byte chunks.
	NimBLEDevice::setMTU(247);

	// Configure and start the BLE scanner
	NimBLEScan* bleScan = NimBLEDevice::getScan();
	bleScan->setScanCallbacks(&bleScanCallbacks); 
	bleScan->setActiveScan(true); // Active scan requests the full device name
	bleScan->setInterval(50);
	bleScan->setWindow(30);

	state = State::SCANNING_AND_CONNECTING;
	Serial.println("Scanning...");
	bleScan->start(15000);
}

// ─────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────

void loop() {
	switch (state) {

		// Wait for the scan to find a device, then connect to it and set up
		// the GATT service and characteristic
		case State::SCANNING_AND_CONNECTING:
			{
				if (!NimBLEDevice::getScan()->isScanning()) {
					if (address == NimBLEAddress{}) {
						// Scan finished without finding any device — try again
						Serial.println("No device found");
						restartScan();
					} else {
						// Device was found during scan — attempt GATT connection
						Serial.println("Connecting to device...");

						client = NimBLEDevice::createClient();
						client->setConnectTimeout(10000);
						client->setConnectRetries(3);
						client->connect(address);

						if (!client->isConnected()) {
							Serial.println("BLE connection failed");
							restartScan();
							break;
						}

						Serial.println("Connected to device");
						Serial.println("Getting service and subscribing to characteristic...");

						// Discover the BLE UART service
						NimBLERemoteService* bleRemoteService = client->getService(SERVICE_UUID);
						if (!bleRemoteService) {
							Serial.printf("Remote service %s is missing\n", SERVICE_UUID);
							restartScan();
							break;
						}

						// Get the characteristic used for both writing commands and
						// receiving status notifications
						characteristic = bleRemoteService->getCharacteristic(CHARACTERISTIC_UUID);
						if (!characteristic) {
							Serial.printf("Remote characteristic %s is missing\n", CHARACTERISTIC_UUID);
							restartScan();
							break;
						}

						// Subscribe to notifications
						characteristic->subscribe(characteristic->canNotify(), notifyCallback);
						Serial.println("Subscribed to characteristic");

						// Send the WiFi credentials immediately after subscribing
						state = State::CONFIGURING_WIFI;
						Serial.println("Sending WiFi config command...");
						sendWifiConfigCommand();
						Serial.println("WiFi config command sent");
						Serial.println("Waiting for response...");
					}
				}

				break;
			}

		// Reached when notification callback receives a Wifi success status
		// from the device
		case State::WIFI_CONFIGURED:
			{
				// There is nothing more to do — log the result and move to done state
				state = State::DONE;
				Serial.println("Done");
				break;
			}
	}

	// Delay to keep CPU usage low
	delay(20);
}
