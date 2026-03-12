#ifdef COMPILE_BT

//We use a local copy of the BluetoothSerial library so that we can increase the RX buffer. See issues:
//https://github.com/sparkfun/SparkFun_RTK_Firmware/issues/23
//https://github.com/sparkfun/SparkFun_RTK_Firmware/issues/469
#include "src/BluetoothSerial/BluetoothSerial.h"

#include <BleSerial.h> //Click here to get the library: http://librarymanager/All#ESP32_BleSerial v1.0.5 by Avinab Malla
#include <BLEDevice.h>

// Standard BLE Service UUIDs
#define BLE_DEVICE_INFORMATION_SERVICE_UUID  "180A"
#define BLE_BATTERY_SERVICE_UUID             "180F"

// Device Information Service characteristic UUIDs (0x180A)
#define BLE_MANUFACTURER_NAME_UUID  "2A29"
#define BLE_MODEL_NUMBER_UUID       "2A24"
#define BLE_SERIAL_NUMBER_UUID      "2A25"
#define BLE_FIRMWARE_REVISION_UUID  "2A26"
#define BLE_HARDWARE_REVISION_UUID  "2A27"

// Battery Service characteristic UUIDs (0x180F)
#define BLE_BATTERY_LEVEL_UUID       "2A19"
#define BLE_BATTERY_POWER_STATE_UUID "2A1A"

// Custom RTK Control Service UUIDs (vendor-specific 128-bit)
#define BLE_RTK_CONTROL_SERVICE_UUID     "a0b40001-926d-4d61-98df-8c5c62ee53b3"
#define BLE_RTK_CONTROL_POINT_UUID       "a0b40002-926d-4d61-98df-8c5c62ee53b3"  // Write: command byte
#define BLE_RTK_WIFI_CONFIG_SSID_UUID    "a0b40003-926d-4d61-98df-8c5c62ee53b3"  // Read: AP SSID
#define BLE_RTK_FIRMWARE_URL_UUID        "a0b40004-926d-4d61-98df-8c5c62ee53b3"  // Read: firmware update URL
#define BLE_RTK_SYSTEM_STATE_UUID        "a0b40005-926d-4d61-98df-8c5c62ee53b3"  // Read+Notify: current system state

// RTK Control Point command values
#define RTK_CMD_ENTER_WIFI_CONFIG  0x01  // Enter WiFi AP config mode
#define RTK_CMD_REBOOT             0x02  // Reboot the device
#define RTK_CMD_ENTER_ROVER        0x03  // Enter rover mode
#define RTK_CMD_ENTER_BASE         0x04  // Enter base mode

// Battery Power State bitfield values
#define BATTERY_POWER_STATE_PRESENT_YES      (3 << 0)  // Bits 0-1: battery present
#define BATTERY_POWER_STATE_DISCHARGING_YES  (3 << 2)  // Bits 2-3: discharging
#define BATTERY_POWER_STATE_DISCHARGING_NO   (2 << 2)  // Bits 2-3: not discharging
#define BATTERY_POWER_STATE_CHARGING_YES     (3 << 4)  // Bits 4-5: charging
#define BATTERY_POWER_STATE_CHARGING_NO      (2 << 4)  // Bits 4-5: not charging
#define BATTERY_POWER_STATE_LEVEL_GOOD       (2 << 6)  // Bits 6-7: good level
#define BATTERY_POWER_STATE_LEVEL_CRITICAL   (3 << 6)  // Bits 6-7: critically low

// Forward declaration - implemented after BTLESerial
class RtkControlPointCallback;

class BTSerialInterface
{
  public:
    virtual bool begin(String deviceName, bool isMaster, uint16_t rxQueueSize, uint16_t txQueueSize) = 0;
    virtual void disconnect() = 0;
    virtual void end() = 0;
    virtual esp_err_t register_callback(esp_spp_cb_t *callback) = 0;
    virtual void setTimeout(unsigned long timeout) = 0;

    virtual int available() = 0;
    virtual size_t readBytes(uint8_t *buffer, size_t bufferSize) = 0;
    virtual int read() = 0;

    // virtual bool isCongested() = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) = 0;
    virtual size_t write(uint8_t value) = 0;
    virtual void flush() = 0;

    // Update BLE GATT characteristics (battery level, etc.). No-op for classic BT.
    virtual void updateBatteryService(int batteryPercent, bool charging) {}

    // Update BLE system state characteristic. No-op for classic BT.
    virtual void updateSystemState(uint8_t state) {}
};

class BTClassicSerial : public virtual BTSerialInterface, public BluetoothSerial
{
    // Everything is already implemented in BluetoothSerial since the code was
    // originally written using that class
  public:
    bool begin(String deviceName, bool isMaster, uint16_t rxQueueSize, uint16_t txQueueSize)
    {
        return BluetoothSerial::begin(deviceName, isMaster, rxQueueSize, txQueueSize);
    }

    void disconnect()
    {
        BluetoothSerial::disconnect();
    }

    void end()
    {
        BluetoothSerial::end();
    }

    esp_err_t register_callback(esp_spp_cb_t *callback)
    {
        return BluetoothSerial::register_callback(callback);
    }

    void setTimeout(unsigned long timeout)
    {
        BluetoothSerial::setTimeout(timeout);
    }

    int available()
    {
        return BluetoothSerial::available();
    }

    size_t readBytes(uint8_t *buffer, size_t bufferSize)
    {
        return BluetoothSerial::readBytes(buffer, bufferSize);
    }

    int read()
    {
        return BluetoothSerial::read();
    }

    size_t write(const uint8_t *buffer, size_t size)
    {
        return BluetoothSerial::write(buffer, size);
    }

    size_t write(uint8_t value)
    {
        return BluetoothSerial::write(value);
    }

    void flush()
    {
        BluetoothSerial::flush();
    }
};

class BTLESerial : public virtual BTSerialInterface, public BleSerial
{
  public:
    // Missing from BleSerial
    bool begin(String deviceName, bool isMaster, uint16_t rxQueueSize, uint16_t txQueueSize)
    {
        BleSerial::begin(deviceName.c_str());

        // Add standard and custom BLE services after BleSerial has created the server
        setupDeviceInfoService();
        setupBatteryService();
        setupControlService();

        // Re-start advertising so the new services are included
        Server->getAdvertising()->start();

        return true;
    }

    void disconnect()
    {
        Server->disconnect(Server->getConnId());
    }

    void end()
    {
        BleSerial::end();
    }

    esp_err_t register_callback(esp_spp_cb_t *callback)
    {
        connectionCallback = callback;
        return ESP_OK;
    }

    void setTimeout(unsigned long timeout)
    {
        BleSerial::setTimeout(timeout);
    }

    int available()
    {
        return BleSerial::available();
    }

    size_t readBytes(uint8_t *buffer, size_t bufferSize)
    {
        return BleSerial::readBytes(buffer, bufferSize);
    }

    int read()
    {
        return BleSerial::read();
    }

    size_t write(const uint8_t *buffer, size_t size)
    {
        return BleSerial::write(buffer, size);
    }

    size_t write(uint8_t value)
    {
        return BleSerial::write(value);
    }

    void flush()
    {
        BleSerial::flush();
    }

    // Update the system state characteristic so the app can track mode changes
    void updateSystemState(uint8_t state)
    {
        if (systemStateCharacteristic != nullptr)
        {
            systemStateCharacteristic->setValue(&state, 1);
            systemStateCharacteristic->notify();
        }
    }

    // Update Battery Service characteristics with current values
    void updateBatteryService(int batteryPercent, bool charging)
    {
        if (batteryLevelCharacteristic == nullptr)
            return;

        // Clamp to valid range
        uint8_t level = (batteryPercent < 0) ? 0 : (batteryPercent > 100) ? 100 : (uint8_t)batteryPercent;
        batteryLevelCharacteristic->setValue(&level, 1);
        batteryLevelCharacteristic->notify();

        if (batteryPowerStateCharacteristic != nullptr)
        {
            uint8_t state = BATTERY_POWER_STATE_PRESENT_YES;
            if (charging)
            {
                state |= BATTERY_POWER_STATE_CHARGING_YES;
                state |= BATTERY_POWER_STATE_DISCHARGING_NO;
            }
            else
            {
                state |= BATTERY_POWER_STATE_CHARGING_NO;
                state |= BATTERY_POWER_STATE_DISCHARGING_YES;
            }
            state |= (level <= 10) ? BATTERY_POWER_STATE_LEVEL_CRITICAL : BATTERY_POWER_STATE_LEVEL_GOOD;
            batteryPowerStateCharacteristic->setValue(&state, 1);
            batteryPowerStateCharacteristic->notify();
        }
    }

    // override BLEServerCallbacks
    void onConnect(BLEServer *pServer)
    {
        // bleConnected = true; Removed until PR is accepted
        connectionCallback(ESP_SPP_SRV_OPEN_EVT, nullptr);
    }

    void onDisconnect(BLEServer *pServer)
    {
        // bleConnected = false; Removed until PR is accepted
        connectionCallback(ESP_SPP_CLOSE_EVT, nullptr);
        Server->startAdvertising();
    }

  private:
    esp_spp_cb_t *connectionCallback;
    BLECharacteristic *batteryLevelCharacteristic = nullptr;
    BLECharacteristic *batteryPowerStateCharacteristic = nullptr;
    BLECharacteristic *systemStateCharacteristic = nullptr;

    // Add BLE Device Information Service (0x180A) with static device info
    void setupDeviceInfoService()
    {
        BLEService *disService = Server->createService(BLEUUID(BLE_DEVICE_INFORMATION_SERVICE_UUID));

        // Manufacturer Name (0x2A29)
        BLECharacteristic *manufacturerChar = disService->createCharacteristic(
            BLEUUID(BLE_MANUFACTURER_NAME_UUID), BLECharacteristic::PROPERTY_READ);
        manufacturerChar->setValue("SparkFun Electronics");

        // Model Number (0x2A24) - use the platform prefix (e.g. "Surveyor", "Facet", "Facet L-Band")
        BLECharacteristic *modelChar = disService->createCharacteristic(
            BLEUUID(BLE_MODEL_NUMBER_UUID), BLECharacteristic::PROPERTY_READ);
        modelChar->setValue(platformPrefix);

        // Serial Number (0x2A25) - use the full BT MAC address as a unique identifier
        BLECharacteristic *serialChar = disService->createCharacteristic(
            BLEUUID(BLE_SERIAL_NUMBER_UUID), BLECharacteristic::PROPERTY_READ);
        char serialNumber[18];
        snprintf(serialNumber, sizeof(serialNumber), "%02X:%02X:%02X:%02X:%02X:%02X",
                 btMACAddress[0], btMACAddress[1], btMACAddress[2],
                 btMACAddress[3], btMACAddress[4], btMACAddress[5]);
        serialChar->setValue(serialNumber);

        // Firmware Revision (0x2A26)
        BLECharacteristic *firmwareChar = disService->createCharacteristic(
            BLEUUID(BLE_FIRMWARE_REVISION_UUID), BLECharacteristic::PROPERTY_READ);
        char firmwareVersion[30];
        getFirmwareVersion(firmwareVersion, sizeof(firmwareVersion), true);
        firmwareChar->setValue(firmwareVersion);

        // Hardware Revision (0x2A27) - use the product variant name
        BLECharacteristic *hardwareChar = disService->createCharacteristic(
            BLEUUID(BLE_HARDWARE_REVISION_UUID), BLECharacteristic::PROPERTY_READ);
        hardwareChar->setValue(platformPrefix);

        disService->start();
    }

    // Add BLE Battery Service (0x180F) with level and power state
    void setupBatteryService()
    {
        BLEService *battService = Server->createService(BLEUUID(BLE_BATTERY_SERVICE_UUID));

        // Battery Level (0x2A19) - read + notify
        batteryLevelCharacteristic = battService->createCharacteristic(
            BLEUUID(BLE_BATTERY_LEVEL_UUID),
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
        uint8_t initialLevel = (battLevel < 0) ? 0 : (battLevel > 100) ? 100 : (uint8_t)battLevel;
        batteryLevelCharacteristic->setValue(&initialLevel, 1);

        // Battery Power State (0x2A1A) - read + notify
        batteryPowerStateCharacteristic = battService->createCharacteristic(
            BLEUUID(BLE_BATTERY_POWER_STATE_UUID),
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
        uint8_t initialState = BATTERY_POWER_STATE_PRESENT_YES
            | BATTERY_POWER_STATE_CHARGING_NO
            | BATTERY_POWER_STATE_DISCHARGING_YES
            | BATTERY_POWER_STATE_LEVEL_GOOD;
        batteryPowerStateCharacteristic->setValue(&initialState, 1);

        battService->start();
    }

    // Add custom RTK Control Service for app-driven commands
    void setupControlService()
    {
        BLEService *ctrlService = Server->createService(BLEUUID(BLE_RTK_CONTROL_SERVICE_UUID));

        // Control Point (writable) - accepts command bytes from the app
        BLECharacteristic *ctrlPointChar = ctrlService->createCharacteristic(
            BLEUUID(BLE_RTK_CONTROL_POINT_UUID), BLECharacteristic::PROPERTY_WRITE);
        ctrlPointChar->setCallbacks(new RtkControlPointCallback());

        // WiFi Config SSID (read-only) - the AP SSID the device will broadcast
        BLECharacteristic *ssidChar = ctrlService->createCharacteristic(
            BLEUUID(BLE_RTK_WIFI_CONFIG_SSID_UUID), BLECharacteristic::PROPERTY_READ);
        ssidChar->setValue("RTK Config");

        // Firmware Update URL (read-only) - where the app should check for updates
        BLECharacteristic *urlChar = ctrlService->createCharacteristic(
            BLEUUID(BLE_RTK_FIRMWARE_URL_UUID), BLECharacteristic::PROPERTY_READ);
        urlChar->setValue(OTA_FIRMWARE_JSON_URL);

        // System State (read + notify) - current device state for the app to track
        systemStateCharacteristic = ctrlService->createCharacteristic(
            BLEUUID(BLE_RTK_SYSTEM_STATE_UUID),
            BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
        uint8_t initialState = (uint8_t)systemState;
        systemStateCharacteristic->setValue(&initialState, 1);

        ctrlService->start();
    }
};

// BLE write callback for the RTK Control Point characteristic.
// When the app writes a command byte, this triggers the corresponding action.
class RtkControlPointCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        std::string value = pCharacteristic->getValue();
        if (value.length() < 1)
            return;

        uint8_t command = value[0];
        switch (command)
        {
            case RTK_CMD_ENTER_WIFI_CONFIG:
                systemPrintln("BLE: Requesting WiFi config mode");
                requestChangeState(STATE_WIFI_CONFIG_NOT_STARTED);
                break;

            case RTK_CMD_REBOOT:
                systemPrintln("BLE: Requesting reboot");
                ESP.restart();
                break;

            case RTK_CMD_ENTER_ROVER:
                systemPrintln("BLE: Requesting rover mode");
                requestChangeState(STATE_ROVER_NOT_STARTED);
                break;

            case RTK_CMD_ENTER_BASE:
                systemPrintln("BLE: Requesting base mode");
                requestChangeState(STATE_BASE_NOT_STARTED);
                break;

            default:
                systemPrintf("BLE: Unknown command 0x%02X\r\n", command);
                break;
        }
    }
};

#endif  // COMPILE_BT
