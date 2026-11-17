#include "Global.h"
#include "classes/IoTItem.h"
#include <NMEA2000_esp32.h>
#include "AisParser.h"

class AisDriver : public IoTItem {
private:
    tNMEA2000 *n2k;
    int rxPin;
    int txPin;
    int debugLevel;
    bool isInitialized;

public:
    AisDriver(String parameters) : IoTItem(parameters), n2k(nullptr), isInitialized(false) {
        jsonRead(parameters, "RX", rxPin);
        jsonRead(parameters, "TX", txPin);
        jsonRead(parameters, "debug", debugLevel);

        begin();
    }

    void begin() {
        if (rxPin < 0 || txPin < 0) {
            if (debugLevel) SerialPrint("E", "AIS", "Invalid RX/TX pins");
            return;
        }

        n2k = new tNMEA2000_esp32((gpio_num_t)txPin, (gpio_num_t)rxPin);

        n2k->SetProductInformation("123456", 100, "AIS Receiver", "1.0", "IoTManager");
        n2k->SetDeviceInformation(12345678, 130, 25, 2040);

        n2k->SetN2kCANSendFrameBufSize(50);
        n2k->SetN2kCANReceiveFrameBufSize(100);

        n2k->SetMode(tNMEA2000::N2km_ListenOnly);
        n2k->SetMsgHandler(AisParser::handleMessage);

        n2k->Open();
        isInitialized = true;

        if (debugLevel) {
            SerialPrint("I", "AIS", "Driver started on RX:" + String(rxPin) + " TX:" + String(txPin));
        }
    }

    void loop() override {
        if (isInitialized && n2k) {
            n2k->ParseMessages();
        }
        IoTItem::loop();
    }

    ~AisDriver() {
        if (n2k) {
            delete n2k;
            n2k = nullptr;
        }
    }
};

void *getAPI_Ais(String subtype, String param) {
    if (subtype == F("AisDriver")) {
        return new AisDriver(param);
    }
    if (subtype == F("AisParser")) {
        return new AisParser(param);
    }
    return nullptr;
}
