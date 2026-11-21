#include "Global.h"
#include "classes/IoTItem.h"
#include <ctime>
#include <cmath>
#include <cstdlib>

class IoTMath : public IoTItem {
private:

    // Извлечение конкретной цифры из числа по запрашиваемому разряду
    int parseDigit(float value, int position, bool hideLeadingZeros = true) {
        if (position == -99) {
            return (value < 0) ? 1 : 0;
        }

        float absVal = std::fabs(value);
        long long integerPart = static_cast<long long>(absVal);

        if (position >= 0) {
            if (hideLeadingZeros && position > 0) {
                long long threshold = 1;
                for (int i = 0; i < position; i++) {
                    threshold *= 10;
                }
                if (integerPart < threshold) {
                    return -1;
                }
            }

            for (int i = 0; i < position; i++) {
                integerPart /= 10;
            }
            return static_cast<int>(integerPart % 10);
        } 
        else {
            int precision = std::abs(position);
            float shifted = absVal * std::pow(10.0f, precision);
            long long shiftedInt = static_cast<long long>(std::round(shifted));
            return static_cast<int>(shiftedInt % 10);
        }
    }

    time_t convertTime(float day, float month, float year, float hour, float minute) {
        int d = static_cast<int>(day);
        int m = static_cast<int>(month);
        int y = static_cast<int>(year);
        int h = static_cast<int>(hour);
        int min = static_cast<int>(minute);

        if (d < 1 || d > 31 || m < 1 || m > 12 || y < 1900 || h < 0 || h > 23 || min < 0 || min > 59) {
            SerialPrint("E", F("IoTMath"), F("Invalid date or time parameters!"));
            return -1;
        }

        struct tm t;
        t.tm_year = y - 1900;
        t.tm_mon = m - 1;
        t.tm_mday = d;
        t.tm_hour = h;
        t.tm_min = min;
        t.tm_sec = 0;
        t.tm_isdst = -1;

        return mktime(&t);
    }

    bool nowInTimePeriod(String startTime, String endTime) {
        int h1 = selectToMarker(startTime, ":").toInt();
        int min1 = selectToMarkerLast(startTime, ":").toInt();
        int h2 = selectToMarker(endTime, ":").toInt();
        int min2 = selectToMarkerLast(endTime, ":").toInt();

        int sumMin1 = h1 * 60 + min1;
        int sumMin2 = h2 * 60 + min2;

        int nowMinutes = _time_local.hour * 60 + _time_local.minute;

        if (sumMin1 <= sumMin2) {
            return nowMinutes >= sumMin1 && nowMinutes <= sumMin2;
        } else {
            return (nowMinutes >= sumMin1 && nowMinutes <= 24 * 60) || (nowMinutes >= 0 && nowMinutes <= sumMin2);
        }
    }

public:
    IoTMath(String parameters) : IoTItem(parameters) {}

    IoTValue execute(String command, std::vector<IoTValue> &param) override {
        if (command == "map" && param.size() == 5) {
            float val = param[0].isDecimal ? param[0].valD : param[0].valS.toFloat();
            float in_min = param[1].isDecimal ? param[1].valD : param[1].valS.toFloat();
            float in_max = param[2].isDecimal ? param[2].valD : param[2].valS.toFloat();
            float out_min = param[3].isDecimal ? param[3].valD : param[3].valS.toFloat();
            float out_max = param[4].isDecimal ? param[4].valD : param[4].valS.toFloat();

            IoTValue valTmp;
            valTmp.isDecimal = true;
            valTmp.valD = map(val, in_min, in_max, out_min, out_max);
            valTmp.valS = String(valTmp.valD);
            return valTmp;
        } 
        else if (command == "rnd" && param.size() >= 2) {
            float minVal = param[0].isDecimal ? param[0].valD : param[0].valS.toFloat();
            float maxVal = param[1].isDecimal ? param[1].valD : param[1].valS.toFloat();

            if (minVal > maxVal) {
                float tmp = minVal;
                minVal = maxVal;
                maxVal = tmp;
            }

            int decimals = 0;
            if (param.size() >= 3) {
                decimals = param[2].isDecimal ? static_cast<int>(param[2].valD) : param[2].valS.toInt();
            }

            if (decimals < 0) decimals = 0;
            if (decimals > 6) decimals = 6;

            float scale = std::pow(10.0f, decimals);
            long long scaledMin = static_cast<long long>(std::ceil(minVal * scale));
            long long scaledMax = static_cast<long long>(std::floor(maxVal * scale));

            float result = minVal;
            if (scaledMin <= scaledMax) {
                uint64_t span = static_cast<uint64_t>(scaledMax - scaledMin) + 1ULL;
#if defined(ESP32)
                uint64_t rnd64 = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
#else
                uint64_t rnd64 = static_cast<uint64_t>(rand());
#endif
                long long chosen = scaledMin + static_cast<long long>(rnd64 % span);
                result = static_cast<float>(chosen) / scale;
            }

            IoTValue valTmp;
            valTmp.isDecimal = true;
            valTmp.valD = result;
            valTmp.valS = String(result, decimals);
            return valTmp;
        }
        else if (command == "parse" && param.size() >= 2) {
            float val = param[0].isDecimal ? param[0].valD : param[0].valS.toFloat();
            int pos = param[1].isDecimal ? static_cast<int>(param[1].valD) : param[1].valS.toInt();
            bool hideZeros = true;

            if (param.size() >= 3) {
                hideZeros = param[2].isDecimal ? (param[2].valD != 0) : (param[2].valS != "0");
            }

            int resultDigit = parseDigit(val, pos, hideZeros);

            IoTValue valTmp;
            valTmp.isDecimal = true;
            valTmp.valD = static_cast<float>(resultDigit);
            valTmp.valS = String(resultDigit);
            return valTmp;
        }
        else if (command == "clock" && param.size() >= 1) {
            int hours = 0;
            int minutes = 0;

            if (param[0].isDecimal) {
                int val = static_cast<int>(param[0].valD);
                if (val >= 100) {
                    hours = val / 100;
                    minutes = val % 100;
                } else {
                    hours = val;
                    minutes = 0;
                }
            } else {
                String timeStr = param[0].valS;
                timeStr.trim();

                int firstColon = timeStr.indexOf(':');
                if (firstColon != -1) {
                    hours = timeStr.substring(0, firstColon).toInt();
                    String rest = timeStr.substring(firstColon + 1);
                    int secondColon = rest.indexOf(':');
                    if (secondColon != -1) {
                        minutes = rest.substring(0, secondColon).toInt();
                    } else {
                        minutes = rest.toInt();
                    }
                } else {
                    hours = timeStr.toInt();
                }
            }

            hours = hours % 12;

            int type = 1;
            if (param.size() >= 2) {
                type = param[1].isDecimal ? static_cast<int>(param[1].valD) : param[1].valS.toInt();
            }

            int position = 0;
            if (type == 1) {
                position = (hours * 5) + (minutes / 12);
            } else if (type == 2) {
                position = minutes % 60;
            }

            IoTValue valTmp;
            valTmp.isDecimal = true;
            valTmp.valD = static_cast<float>(position);
            valTmp.valS = String(position);
            return valTmp;
        }
        else if (command == "convertTime" && param.size() == 5) {
            float day = param[0].isDecimal ? param[0].valD : param[0].valS.toFloat();
            float mon = param[1].isDecimal ? param[1].valD : param[1].valS.toFloat();
            float yr  = param[2].isDecimal ? param[2].valD : param[2].valS.toFloat();
            float hr  = param[3].isDecimal ? param[3].valD : param[3].valS.toFloat();
            float min = param[4].isDecimal ? param[4].valD : param[4].valS.toFloat();

            uint32_t unixTime = convertTime(day, mon, yr, hr, min);

            if (unixTime == (uint32_t)-1) {
                SerialPrint("E", F("IoTMath"), F("Failed to convert time."));
                return {};
            }

            IoTValue valTmp;
            valTmp.isDecimal = true;
            valTmp.valD = static_cast<float>(unixTime);
            valTmp.valS = String(unixTime);
            return valTmp;
        } 
        else if (command == "nowInTimePeriod" && param.size() == 2) {
            IoTValue valTmp;
            valTmp.isDecimal = true;
            valTmp.valD = nowInTimePeriod(param[0].valS, param[1].valS); 
            valTmp.valS = String((int)valTmp.valD);
            return valTmp;
        }
        else if (command == "parseTimeToMinutes" && param.size() == 1) {
            if (param[0].isDecimal) {
                return param[0]; 
            }

            String timeStr = param[0].valS;
            timeStr.trim();
            
            long days = 0;
            int dayIdx = timeStr.indexOf('d');
            
            if (dayIdx != -1) {
                days = timeStr.substring(0, dayIdx).toInt();
                timeStr = timeStr.substring(dayIdx + 1);
                timeStr.trim();
            }

            int colonIdx = timeStr.indexOf(':');
            
            IoTValue valTmp;
            valTmp.isDecimal = true;
            
            if (colonIdx == -1) {
                valTmp.valD = timeStr.toFloat() + static_cast<float>(days * 1440);
            } else {
                long hours = timeStr.substring(0, colonIdx).toInt();
                long minutes = timeStr.substring(colonIdx + 1).toInt();
                valTmp.valD = static_cast<float>((days * 1440) + (hours * 60) + minutes);
            }
            valTmp.valS = String(valTmp.valD);
            return valTmp;
        }

        SerialPrint("E", F("IoTMath"), F("Unknown command or wrong parameters."));
        return {};
    }
};

void *getAPI_IoTMath(String subtype, String param) {
    if (subtype == F("IoTMath")) {
        return new IoTMath(param);
    }
    return nullptr;
}