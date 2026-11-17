#pragma once

#include "Global.h"
#include "classes/IoTItem.h"
#include <NMEA2000.h>
#include <N2kMessages.h>

class AisParser : public IoTItem {
private:
    static AisParser* instance;

public:
    AisParser(String parameters) : IoTItem(parameters) {
        instance = this;
    }

    ~AisParser() {
        if (instance == this) {
            instance = nullptr;
        }
    }

    // Главный callback-обработчик для библиотеки NMEA2000
    static void handleMessage(const tN2kMsg &msg) {
        if (!instance) return;
        switch (msg.PGN) {
            case 129038: // AIS Class A Position Report
                instance->parsePositionA(msg);
                break;
            case 129039: // AIS Class B Position Report
                instance->parsePositionB(msg);
                break;
            case 129794: // AIS Class A Static Data
                instance->parseStaticA(msg);
                break;
            default:
                break;
        }
    }

private:
    void parsePositionA(const tN2kMsg &msg) {
        uint8_t MessageID;
        tN2kAISRepeat Repeat;
        uint32_t MMSI;
        double Latitude, Longitude, COG, SOG, Heading, ROT;
        bool Accuracy, RAIM;
        uint8_t Seconds;
        tN2kAISNavStatus NavStatus;

        if (ParseN2kAISClassAPosition(msg, MessageID, Repeat, MMSI, Latitude, Longitude,
                                      Accuracy, RAIM, Seconds, COG, SOG, Heading,
                                      ROT, NavStatus)) 
        {
            // Формируем строку данных для отправки в систему через regEvent
            String val = String(MMSI) + "," + String(Latitude, 5) + "," + String(Longitude, 5) + "," + String(SOG);
            regEvent(val, "AIS_A"); // Регистрация события с указанием подкатегории/ID

            Serial.printf("[AIS] Class A -> MMSI: %lu, Lat: %.5f, Lon: %.5f, SOG: %.2f\n",
                          (unsigned long)MMSI, Latitude, Longitude, SOG);
        }
    }

    void parsePositionB(const tN2kMsg &msg) {
        uint8_t MessageID;
        tN2kAISRepeat Repeat;
        uint32_t MMSI;
        double Latitude, Longitude, COG, SOG, Heading;
        bool Accuracy, RAIM;
        uint8_t Seconds;
        tN2kAISTransceiverInformation AISTransceiverInformation;
        tN2kAISUnit Unit;
        bool Display, DSC, Band, Msg22, State;
        tN2kAISMode Mode;

        if (ParseN2kAISClassBPosition(msg, MessageID, Repeat, MMSI, Latitude, Longitude,
                                      Accuracy, RAIM, Seconds, COG, SOG,
                                      AISTransceiverInformation, Heading,
                                      Unit, Display, DSC, Band, Msg22, Mode, State)) 
        {
            String val = String(MMSI) + "," + String(Latitude, 5) + "," + String(Longitude, 5) + "," + String(SOG);
            regEvent(val, "AIS_B");

            Serial.printf("[AIS] Class B -> MMSI: %lu, Lat: %.5f, Lon: %.5f, SOG: %.2f\n",
                          (unsigned long)MMSI, Latitude, Longitude, SOG);
        }
    }

    void parseStaticA(const tN2kMsg &msg) {
        uint8_t MessageID;
        tN2kAISRepeat Repeat;
        uint32_t MMSI, IMONumber;
        char CallSign[10];
        char Name[21];
        uint8_t VesselType;
        double Length, Beam, PosRefStbd, PosRefBow, Draught;
        uint16_t ETAdate;
        double ETAtime;
        char Destination[21];
        tN2kAISVersion AISversion;
        tN2kGNSStype GNSStype;
        tN2kAISDTE DTE;
        tN2kAISTransceiverInformation AISinfo;
        uint8_t SequenceID;

        if (ParseN2kPGN129794(msg, MessageID, Repeat, MMSI, IMONumber,
                              CallSign, sizeof(CallSign),
                              Name, sizeof(Name),
                              VesselType, Length, Beam, PosRefStbd, PosRefBow,
                              ETAdate, ETAtime, Draught,
                              Destination, sizeof(Destination),
                              AISversion, GNSStype, DTE, AISinfo, SequenceID)) 
        {
            String val = String(MMSI) + ":" + String(Name);
            regEvent(val, "AIS_STATIC");

            Serial.printf("[AIS Static] MMSI: %lu, Name: %s, Dest: %s\n",
                          (unsigned long)MMSI, Name, Destination);
        }
    }
};

// Инициализация статического указателя экземпляра
AisParser* AisParser::instance = nullptr;