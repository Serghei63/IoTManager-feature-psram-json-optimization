#include "Global.h"
#include "classes/IoTItem.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"

// Основные компоненты ESP8266Audio (с поддержкой ICY-метаданных)
#include "AudioFileSourceICYStream.h"
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
    enum class CodecType {
        None,
        MP3,
        AAC,
        WAV,
        FLAC
    };

    // Пины по умолчанию: WSEL -> 15, DIN -> 16, BCLK -> 21
    int _wclk = 15;
    int _dout = 16;
    int _bclk = 21;

    int _volume = 70; // 0..100%
    uint32_t _retryDelayMs = 2000;
    String _url = "";
    String _stationName = "Stopped";

    // Буферы ICY-метаданных (заполняются в колбэке потока, публикуются в основном loop())
    String _currentTrack = "";
    String _pendingTrack = "";
    volatile bool _trackChanged = false;

    // Отложенная публикация в основном loop() (название станции / "Stopped" / трек)
    String _mainPendingValue = "";
    String _mainPendingInfo = "";
    volatile bool _mainHasPending = false;

    // Компоненты ESP8266Audio
    AudioFileSourceICYStream* _file     = nullptr;
    AudioFileSourceBuffer*    _buff     = nullptr;
    AudioGenerator*           _decoder  = nullptr;
    CodecType                 _codec    = CodecType::None;
    AudioOutputI2S*           _out      = nullptr;
    uint8_t*                  _psramBuf = nullptr;

    TaskHandle_t _radioTaskHandle = nullptr;
    SemaphoreHandle_t _pipelineMutex = nullptr;
    SemaphoreHandle_t _taskExitedSem = nullptr;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    volatile bool _isPlaying      = false;
    volatile bool _decoderStarted = false;
    volatile bool _needsConnect   = false;
    volatile bool _needsStop      = false;
    volatile bool _taskRunning    = true;

    void lockPipeline() {
        if (_pipelineMutex) xSemaphoreTake(_pipelineMutex, portMAX_DELAY);
    }

    void unlockPipeline() {
        if (_pipelineMutex) xSemaphoreGive(_pipelineMutex);
    }

    int parsePinOrDefault(const String& token, int defaultPin) {
        String s = token;
        s.trim();
        if (s.length() == 0) return defaultPin;
        for (size_t i = 0; i < s.length(); i++) {
            if (!isDigit(s[i])) return defaultPin;
        }
        return s.toInt();
    }

    // Извлекает чистое название из строки вида "StreamTitle='Artist - Song';StreamUrl='...';"
    static String extractStreamTitle(const char* rawStr) {
        if (!rawStr) return "";
        String s(rawStr);
        s.trim();
        if (s.length() == 0) return "";

        int keyIdx = s.indexOf("StreamTitle=");
        if (keyIdx != -1) {
            int start = keyIdx + 12; // длина "StreamTitle="
            if (start < (int)s.length() && (s[start] == '\'' || s[start] == '"')) {
                char quote = s[start];
                start++;
                int end = s.indexOf(String(quote) + ";", start);
                if (end == -1) {
                    end = s.lastIndexOf(quote);
                }
                if (end > start) {
                    s = s.substring(start, end);
                } else {
                    s = s.substring(start);
                }
            } else {
                int end = s.indexOf(';', start);
                s = (end > start) ? s.substring(start, end) : s.substring(start);
            }
        }

        s.trim();
        return s;
    }

    static bool isGarbageMetadata(const String& title) {
        if (title.length() == 0 || title == "0") return true;
        if (title.startsWith("{") || title.startsWith("[")) return true;
        if (title.indexOf("\"errorCode\"") != -1 || title.indexOf("\"status\"") != -1) return true;
        return false;
    }

    void onMetadataReceived(const char* type, bool isUnicode, const char* string) {
        (void)isUnicode;
        if (!string || !string[0]) return;

        String typeStr = type ? String(type) : "";
        typeStr.trim();

        if (typeStr.equalsIgnoreCase("StreamTitle") || typeStr.length() == 0 || strstr(string, "StreamTitle=") != nullptr) {
            String title = extractStreamTitle(string);
            if (isGarbageMetadata(title)) return;

            portENTER_CRITICAL(&_mux);
            if (title != _currentTrack) {
                _currentTrack = title;
                _pendingTrack = title;
                _trackChanged = true;
            }
            portEXIT_CRITICAL(&_mux);
        }
    }

    static void icyMetadataCallback(void* cbData, const char* type, bool isUnicode, const char* string) {
        Radio1334* self = static_cast<Radio1334*>(cbData);
        if (self && self->_taskRunning) {
            self->onMetadataReceived(type, isUnicode, string);
        }
    }

    // Вызывается только из основного loop() — безопасно обновляет value, WS и события
    void publishTrackInMainLoop() {
        String trackToSend;
        bool hasUpdate = false;

        portENTER_CRITICAL(&_mux);
        if (_trackChanged) {
            trackToSend = _pendingTrack;
            _trackChanged = false;
            hasUpdate = true;
        }
        portEXIT_CRITICAL(&_mux);

        if (hasUpdate) {
            Serial.printf("[Radio1334] ICY Track -> Event & WS: %s\n", trackToSend.c_str());
            value.isDecimal = false;
            value.valS = trackToSend;
            regEvent(trackToSend, "track", false, true);
        }
    }

    void publishMainPendingIfAny() {
        String valToSend;
        String infoToSend;
        bool hasMsg = false;

        portENTER_CRITICAL(&_mux);
        if (_mainHasPending) {
            valToSend = _mainPendingValue;
            infoToSend = _mainPendingInfo;
            _mainHasPending = false;
            hasMsg = true;
        }
        portEXIT_CRITICAL(&_mux);

        if (hasMsg) {
            value.isDecimal = false;
            value.valS = valToSend;
            regEvent(valToSend, infoToSend, false, true);
        }
    }

    void queueMainEvent(const String& val, const String& info) {
        portENTER_CRITICAL(&_mux);
        _mainPendingValue = val;
        _mainPendingInfo = info;
        _mainHasPending = true;
        portEXIT_CRITICAL(&_mux);
    }

    void requestConnect(const String& url, const String& station) {
        portENTER_CRITICAL(&_mux);
        if (url.length() > 0) {
            _url = url;
        }
        if (station.length() > 0) {
            _stationName = station;
        }
        _currentTrack = "";
        _pendingTrack = "";
        _trackChanged = false;
        _retryDelayMs = 2000;
        _needsStop = false;
        _needsConnect = true;
        portEXIT_CRITICAL(&_mux);
    }

    void requestStop() {
        portENTER_CRITICAL(&_mux);
        _currentTrack = "";
        _pendingTrack = "";
        _trackChanged = false;
        _retryDelayMs = 2000;
        _needsConnect = false;
        _needsStop = true;
        portEXIT_CRITICAL(&_mux);
    }

    void waitInterruptibleMs(uint32_t delayMs) {
        uint32_t elapsed = 0;
        const uint32_t stepMs = 100;
        while (_taskRunning && elapsed < delayMs) {
            portENTER_CRITICAL(&_mux);
            bool aborted = _needsStop || _needsConnect;
            portEXIT_CRITICAL(&_mux);
            if (aborted) return;

            uint32_t chunk = (delayMs - elapsed < stepMs) ? (delayMs - elapsed) : stepMs;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            elapsed += chunk;
        }
    }

    void scheduleRetryWithBackoff() {
        uint32_t currentDelay = 2000;
        portENTER_CRITICAL(&_mux);
        if (_url.length() > 0 && !_needsStop && !_needsConnect) {
            currentDelay = _retryDelayMs;
            uint32_t nextDelay = currentDelay * 2;
            if (nextDelay > 15000) nextDelay = 15000;
            _retryDelayMs = nextDelay;
        }
        portEXIT_CRITICAL(&_mux);

        Serial.printf("[UDA1334 Task] Will retry stream in %u ms...\n", (unsigned)currentDelay);
        waitInterruptibleMs(currentDelay);

        portENTER_CRITICAL(&_mux);
        if (_taskRunning && !_needsStop && !_needsConnect && _url.length() > 0) {
            _needsConnect = true;
        }
        portEXIT_CRITICAL(&_mux);
    }

    CodecType detectCodecForUrl(const String& url) const {
        String urlLower = url;
        urlLower.toLowerCase();

        if (urlLower.indexOf(".aac") != -1 || urlLower.indexOf("/aac") != -1 || urlLower.indexOf("format=aac") != -1) {
            return CodecType::AAC;
        }
        if (urlLower.indexOf(".flac") != -1) {
            return CodecType::FLAC;
        }
        if (urlLower.indexOf(".wav") != -1) {
            return CodecType::WAV;
        }
        return CodecType::MP3;
    }

    bool ensureDecoderForCodec(CodecType targetCodec) {
        if (_decoder != nullptr && _codec == targetCodec) {
            return true;
        }

        if (_decoder != nullptr) {
            if (_decoderStarted) {
                _decoder->stop();
                _decoderStarted = false;
            }
            delete _decoder;
            _decoder = nullptr;
            _codec = CodecType::None;
        }

        switch (targetCodec) {
            case CodecType::AAC:
                Serial.println("[UDA1334 Task] Selected codec: AAC");
                _decoder = new AudioGeneratorAAC();
                break;
            case CodecType::FLAC:
                Serial.println("[UDA1334 Task] Selected codec: FLAC");
                _decoder = new AudioGeneratorFLAC();
                break;
            case CodecType::WAV:
                Serial.println("[UDA1334 Task] Selected codec: WAV");
                _decoder = new AudioGeneratorWAV();
                break;
            case CodecType::MP3:
            default:
                Serial.println("[UDA1334 Task] Selected codec: MP3");
                _decoder = new AudioGeneratorMP3();
                break;
        }

        if (_decoder != nullptr) {
            _codec = targetCodec;
            return true;
        }
        return false;
    }

    void destroyDecoderLocked() {
        if (_decoder != nullptr) {
            if (_decoderStarted) {
                _decoder->stop();
                _decoderStarted = false;
            }
            delete _decoder;
            _decoder = nullptr;
            _codec = CodecType::None;
        }
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

    void waitBufferPrefill(uint32_t minBytes = 2048, uint32_t timeoutMs = 1500) {
        if (!_buff) return;
        uint32_t start = millis();
        while (_taskRunning && _buff->isOpen() && _buff->getSize() < minBytes && (millis() - start) < timeoutMs) {
            _buff->loop();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // Останавливает воспроизведение и освобождает поток/буфер/выход, но сохраняет экземпляр _decoder
    void stopAudioPipelineLocked(bool releaseDecoder = false) {
        _isPlaying = false;

        bool wasDecoderStarted = _decoderStarted;
        _decoderStarted = false;

        AudioGenerator* dec = _decoder;
        AudioOutputI2S* out = _out;
        AudioFileSourceBuffer* buff = _buff;
        uint8_t* psramBuf = _psramBuf;
        AudioFileSourceICYStream* file = _file;

        _out      = nullptr;
        _buff     = nullptr;
        _psramBuf = nullptr;
        _file     = nullptr;

        if (releaseDecoder) {
            _decoder = nullptr;
            _codec = CodecType::None;
        }

        if (dec && wasDecoderStarted) {
            dec->stop();
        }
        if (releaseDecoder && dec) {
            delete dec;
        }
        if (out) {
            out->stop();
            delete out;
        }
        if (buff) {
            delete buff;
        }
        if (psramBuf) {
            heap_caps_free(psramBuf);
        }
        if (file) {
            file->close();
            delete file;
        }
    }

    void connectStream() {
        lockPipeline();
        stopAudioPipelineLocked(false);
        unlockPipeline();

        vTaskDelay(pdMS_TO_TICKS(150));

        if (!_taskRunning) return;

        String targetUrl;
        int currentVol;
        portENTER_CRITICAL(&_mux);
        targetUrl = _url;
        currentVol = _volume;
        portEXIT_CRITICAL(&_mux);

        if (targetUrl.length() == 0) {
            Serial.println("[UDA1334 Task] Empty URL!");
            return;
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[UDA1334 Task] Wi-Fi not connected!");
            scheduleRetryWithBackoff();
            return;
        }

        bool startedOk = false;

        lockPipeline();
        Serial.printf("[UDA1334 Task] Connecting to (ICY): %s\n", targetUrl.c_str());
        _file = new AudioFileSourceICYStream(targetUrl.c_str());
        if (_file) {
            _file->RegisterMetadataCB(Radio1334::icyMetadataCallback, this);
        }

        if (!_file || !allocateBuffer()) {
            Serial.println("[UDA1334 Task] Failed to create ICY stream or buffer!");
            stopAudioPipelineLocked(false);
            unlockPipeline();
            scheduleRetryWithBackoff();
            return;
        }

        waitBufferPrefill();

        _out = new AudioOutputI2S();
        _out->SetPinout(_bclk, _wclk, _dout);
        _out->SetGain((float)currentVol / 100.0f);

        CodecType targetCodec = detectCodecForUrl(targetUrl);
        if (!ensureDecoderForCodec(targetCodec)) {
            Serial.println("[UDA1334 Task] Decoder allocation failed!");
            stopAudioPipelineLocked(false);
            unlockPipeline();
            scheduleRetryWithBackoff();
            return;
        }

        _decoderStarted = false;
        if (_decoder && _decoder->begin(_buff, _out)) {
            _decoderStarted = true;
            _isPlaying = true;
            startedOk = true;
            Serial.println("[UDA1334 Task] Playback started!");
        } else {
            Serial.println("[UDA1334 Task] Decoder start failed (no internet or bad stream)!");
            stopAudioPipelineLocked(false);
        }
        unlockPipeline();

        if (startedOk) {
            portENTER_CRITICAL(&_mux);
            _retryDelayMs = 2000;
            portEXIT_CRITICAL(&_mux);
        } else {
            scheduleRetryWithBackoff();
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
                instance->lockPipeline();
                instance->stopAudioPipelineLocked(false);
                instance->unlockPipeline();
            } else if (doConnect) {
                instance->connectStream();
            }

            bool needReconnect = false;
            instance->lockPipeline();
            if (instance->_isPlaying && instance->_decoder && instance->_decoderStarted) {
                if (instance->_decoder->isRunning()) {
                    if (!instance->_decoder->loop()) {
                        Serial.println("[UDA1334 Task] Stream lost or finished!");
                        instance->stopAudioPipelineLocked(false);
                        needReconnect = true;
                    }
                } else {
                    instance->_isPlaying = false;
                }
            }
            instance->unlockPipeline();

            // В задаче декодера regEvent НЕ вызываем — флаг _trackChanged подхватит loop() основного потока

            if (needReconnect) {
                instance->scheduleRetryWithBackoff();
            }

            if (instance->_isPlaying) {
                vTaskDelay(1);
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        instance->lockPipeline();
        instance->stopAudioPipelineLocked(true);
        instance->unlockPipeline();

        if (instance->_taskExitedSem) {
            xSemaphoreGive(instance->_taskExitedSem);
        }
        vTaskDelete(nullptr);
    }

public:
    Radio1334(String parameters) : IoTItem(parameters) {
        _pipelineMutex = xSemaphoreCreateMutex();
        _taskExitedSem = xSemaphoreCreateBinary();

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

        if (_taskExitedSem && _radioTaskHandle) {
            xSemaphoreTake(_taskExitedSem, pdMS_TO_TICKS(2000));
        }

        lockPipeline();
        destroyDecoderLocked();
        unlockPipeline();

        if (_pipelineMutex) {
            vSemaphoreDelete(_pipelineMutex);
            _pipelineMutex = nullptr;
        }
        if (_taskExitedSem) {
            vSemaphoreDelete(_taskExitedSem);
            _taskExitedSem = nullptr;
        }
        _radioTaskHandle = nullptr;
    }

    void loop() override {
        IoTItem::loop();
        publishMainPendingIfAny();
        publishTrackInMainLoop();
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
                value.isDecimal = false;
                value.valS = nextStation;
                regEvent(nextStation, "title", false, true);
            }
        }
        else if (command == "stop") {
            requestStop();
            _stationName = "Stopped";
            value.isDecimal = false;
            value.valS = _stationName;
            regEvent(_stationName, "title", false, true);
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

                if (changed) {
                    lockPipeline();
                    if (_out) {
                        _out->SetGain((float)newVol / 100.0f);
                    }
                    unlockPipeline();
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
    radio.play("http://193.33.170.218:8000/RoyalTrance", "Trance") — включит поток и в текстовом виджете сразу загорится «Trance».

    radio.stop() — остановит воспроизведение, а на экране появится «Stopped».
*/
