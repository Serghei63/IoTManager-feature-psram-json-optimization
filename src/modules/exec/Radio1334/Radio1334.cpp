#include "Global.h"
#include "classes/IoTItem.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

// Основные компоненты ESP8266Audio
#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioOutputI2S.h"
#include "esp_task_wdt.h"

// Декодеры поддерживаемых кодеков
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorWAV.h"
#include "AudioGeneratorFLAC.h"

class Radio1334 : public IoTItem {
private:
    // Жестко привязанные на плате пины по умолчанию: WSEL -> 15, DIN -> 16, BCLK -> 21
    int _wclk = 15;
    int _dout = 16;
    int _bclk = 21;

    int _volume = 70; // 0..100%
    String _url = "";
    String _stationName = "Stopped";

    // Компоненты ESP8266Audio
    AudioFileSourceHTTPStream* _file     = nullptr;
    AudioFileSourceBuffer*     _buff     = nullptr;
    AudioGenerator*            _decoder  = nullptr;
    AudioOutputI2S*            _out      = nullptr;
    uint8_t*                   _psramBuf = nullptr;

    TaskHandle_t _radioTaskHandle = nullptr;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    volatile bool _isPlaying    = false;
    volatile bool _needsConnect = false;
    volatile bool _needsStop    = false;
    volatile bool _taskRunning  = true;

    int parsePinOrDefault(const String& token, int defaultPin) {
        String s = token;
        s.trim();
        if (s.length() == 0) return defaultPin;
        for (size_t i = 0; i < s.length(); i++) {
            if (!isDigit(s[i])) return defaultPin;
        }
        return s.toInt();
    }

    void requestConnect(const String& url, const String& station) {
        portENTER_CRITICAL(&_mux);
        if (url.length() > 0) {
            _url = url;
        }
        if (station.length() > 0) {
            _stationName = station;
        }
        _needsStop = false;
        _needsConnect = true;
        portEXIT_CRITICAL(&_mux);
    }

    void requestStop() {
        portENTER_CRITICAL(&_mux);
        _needsConnect = false;
        _needsStop = true;
        portEXIT_CRITICAL(&_mux);
    }

    bool allocateBuffer() {
        static constexpr size_t PSRAM_BUFF_SIZE = 512 * 1024;
        static constexpr size_t DRAM_BUFF_SIZE  = 64 * 1024;

#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM_SUPPORT)
        if (psramFound()) {
            _psramBuf = (uint8_t*)heap_caps_malloc(PSRAM_BUFF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (_psramBuf) {
                _buff = new AudioFileSourceBuffer(_file, _psramBuf, PSRAM_BUFF_SIZE);
                Serial.println("[UDA1334 Task] 512 KB stream buffer allocated in PSRAM");
                return (_buff != nullptr);
            }
        }
#endif
        _buff = new AudioFileSourceBuffer(_file, DRAM_BUFF_SIZE);
        Serial.println("[UDA1334 Task] 64 KB buffer allocated in DRAM (PSRAM fallback)");
        return (_buff != nullptr);
    }

    AudioGenerator* createDecoderForUrl(const String& url) {
        String urlLower = url;
        urlLower.toLowerCase();

        if (urlLower.indexOf(".aac") != -1 || urlLower.indexOf("/aac") != -1 || urlLower.indexOf("format=aac") != -1) {
            Serial.println("[UDA1334 Task] Selected codec: AAC");
            return new AudioGeneratorAAC();
        }
        if (urlLower.indexOf(".flac") != -1) {
            Serial.println("[UDA1334 Task] Selected codec: FLAC");
            return new AudioGeneratorFLAC();
        }
        if (urlLower.indexOf(".wav") != -1) {
            Serial.println("[UDA1334 Task] Selected codec: WAV");
            return new AudioGeneratorWAV();
        }
        Serial.println("[UDA1334 Task] Selected codec: MP3");
        return new AudioGeneratorMP3();
    }

    void connectStream() {
        stopAudioPipeline();
        vTaskDelay(pdMS_TO_TICKS(150));

        String targetUrl;
        int currentVol;
        portENTER_CRITICAL(&_mux);
        targetUrl = _url;
        currentVol = _volume;
        portEXIT_CRITICAL(&_mux);

        if (WiFi.status() != WL_CONNECTED || targetUrl.length() == 0) {
            Serial.println("[UDA1334 Task] Wi-Fi not connected or empty URL!");
            return;
        }

        Serial.printf("[UDA1334 Task] Connecting to: %s\n", targetUrl.c_str());
        _file = new AudioFileSourceHTTPStream(targetUrl.c_str());
        if (!_file || !allocateBuffer()) {
            Serial.println("[UDA1334 Task] Failed to create stream or buffer!");
            stopAudioPipeline();
            return;
        }

        _out = new AudioOutputI2S();
        _out->SetPinout(_bclk, _wclk, _dout);
        _out->SetGain((float)currentVol / 100.0f);

        _decoder = createDecoderForUrl(targetUrl);
        if (_decoder && _decoder->begin(_buff, _out)) {
            _isPlaying = true;
            Serial.println("[UDA1334 Task] Playback started!");
        } else {
            Serial.println("[UDA1334 Task] Decoder start failed!");
            stopAudioPipeline();
        }
    }

    static void radioWorker(void* pvParameters) {
        Radio1334* instance = static_cast<Radio1334*>(pvParameters);

        esp_task_wdt_delete(NULL);
        vTaskDelay(pdMS_TO_TICKS(200));

        while (instance->_taskRunning) {
            bool doStop = false;
            bool doConnect = false;

            portENTER_CRITICAL(&instance->_mux);
            doStop = instance->_needsStop;
            if (doStop) {
                instance->_needsStop = false;
                instance->_needsConnect = false;
            } else {
                doConnect = instance->_needsConnect;
                if (doConnect) {
                    instance->_needsConnect = false;
                }
            }
            portEXIT_CRITICAL(&instance->_mux);

            if (doStop) {
                instance->stopAudioPipeline();
            } else if (doConnect) {
                instance->connectStream();
            }

            if (instance->_isPlaying && instance->_decoder) {
                if (instance->_decoder->isRunning()) {
                    if (!instance->_decoder->loop()) {
                        Serial.println("[UDA1334 Task] Stream lost or finished! Reconnecting in 2s...");
                        instance->stopAudioPipeline();

                        for (int i = 0; i < 20 && instance->_taskRunning; i++) {
                            portENTER_CRITICAL(&instance->_mux);
                            bool aborted = instance->_needsStop || instance->_needsConnect;
                            portEXIT_CRITICAL(&instance->_mux);
                            if (aborted) break;
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }

                        portENTER_CRITICAL(&instance->_mux);
                        if (!instance->_needsStop && !instance->_needsConnect && instance->_url.length() > 0) {
                            instance->_needsConnect = true;
                        }
                        portEXIT_CRITICAL(&instance->_mux);
                    }
                } else {
                    instance->_isPlaying = false;
                }
                vTaskDelay(1);
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        instance->stopAudioPipeline();
        vTaskDelete(nullptr);
    }

    void stopAudioPipeline() {
        _isPlaying = false;

        if (_decoder) {
            if (_decoder->isRunning()) _decoder->stop();
            delete _decoder;
            _decoder = nullptr;
        }
        if (_out) {
            _out->stop();
            delete _out;
            _out = nullptr;
        }
        if (_buff) {
            delete _buff;
            _buff = nullptr;
        }
        if (_psramBuf) {
            heap_caps_free(_psramBuf);
            _psramBuf = nullptr;
        }
        if (_file) {
            _file->close();
            delete _file;
            _file = nullptr;
        }
    }

public:
    Radio1334(String parameters) : IoTItem(parameters) {
        String pins;
        if (jsonRead(parameters, "pins", pins) && pins.length() > 0) {
            _wclk = parsePinOrDefault(selectFromMarkerToMarker(pins, ",", 0), 15);
            _dout = parsePinOrDefault(selectFromMarkerToMarker(pins, ",", 1), 16);
            _bclk = parsePinOrDefault(selectFromMarkerToMarker(pins, ",", 2), 21);
        }

        jsonRead(parameters, "volume", _volume);

        xTaskCreatePinnedToCore(
            Radio1334::radioWorker,
            "UdaRadioTask",
            8192,
            this,
            12,
            &_radioTaskHandle,
            0
        );
    }

    ~Radio1334() {
        _taskRunning = false;
        requestStop();
        vTaskDelay(pdMS_TO_TICKS(200));
        _radioTaskHandle = nullptr;
    }

    void loop() override {
        IoTItem::loop();
    }

    void doByInterval() override {}

    void onModuleOrder(String &key, String &value) override {
        std::vector<IoTValue> emptyParams;
        if (key == "play") {
            execute("play", emptyParams);
        } else if (key == "stop") {
            execute("stop", emptyParams);
        } else if (key == "volume") {
            std::vector<IoTValue> params;
            IoTValue val;
            val.isDecimal = true;
            val.valD = value.toInt();
            params.push_back(val);
            execute("volume", params);
        }
    }

    IoTValue execute(String command, std::vector<IoTValue> &param) override {
        if (command == "play") {
            String nextUrl = _url;
            String nextStation = _stationName;

            if (param.size() > 0 && param[0].valS.length() > 0) {
                nextUrl = param[0].valS;
            }
            if (param.size() > 1 && param[1].valS.length() > 0) {
                nextStation = param[1].valS;
            }

            if (WiFi.status() != WL_CONNECTED) {
                SerialPrint("E", F("Radio1334"), "Connect failed: Wi-Fi not ready!");
                return {};
            }

            if (nextUrl.length() > 0) {
                requestConnect(nextUrl, nextStation);
                regEvent(nextStation, "title");
                regEvent(nextStation, "track");
            }
        }
        else if (command == "stop") {
            requestStop();
            _stationName = "Stopped";
            regEvent(_stationName, "title");
            regEvent("", "track");
        }
        else if (command == "volume") {
            if (param.size() > 0) {
                int newVol = (int)param[0].valD;
                if (newVol > 100) newVol = 100;
                if (newVol < 0) newVol = 0;

                portENTER_CRITICAL(&_mux);
                bool changed = (_volume != newVol);
                _volume = newVol;
                portEXIT_CRITICAL(&_mux);

                if (changed && _out) {
                    _out->SetGain((float)newVol / 100.0f);
                }
            }
        }
        return {};
    }
};

void* getAPI_Radio1334(String subtype, String param) {
    if (subtype == F("Radio1334")) {
        return new Radio1334(param);
    }
    return nullptr;
}
/*
myradio.play("http://stream.com/rock.mp3", "Рок ФМ") — включит поток и в текстовом виджете сразу загорится «Рок ФМ».

myradio.play("http://stream.com/jazz.mp3", "Радио Джаз") — переключит на джаз с соответствующей надписью.

myradio.stop() — остановит воспроизведение, а на экране появится «Stopped».


*/