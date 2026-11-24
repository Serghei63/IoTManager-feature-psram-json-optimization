#include "Global.h"
#include "classes/IoTItem.h"
#include <stdint.h>
#include <math.h>
#include <list>
#include <vector>
#include <utility>
#if defined(ESP8266)
#include <core_esp8266_waveform.h>
#elif defined(ESP32)
#include <driver/gpio.h>
#include <esp_arduino_version.h>
#include <esp32-hal-ledc.h>
#include <soc/soc_caps.h>
#endif

extern IoTGpio IoTgpio;

class Blink : public IoTItem
{
private:
    enum class Mode { Periodic, Pattern, Random, Smooth, SmoothRandom };
    struct Channel {
        int pin = -1;
        bool inverse = false;
        bool on = false;
        Mode mode = Mode::Periodic;
        uint32_t start = 0, total = 0, period = 0, pulse = 0;
        std::vector<uint32_t> phases;
        size_t index = 0;
        uint32_t phaseStart = 0;
        // Random mode: independent generator and timing for each GPIO.
        uint32_t meanPeriod = 0, periodSpread = 0;
        uint32_t meanDuty = 0, dutySpread = 0;
        uint32_t rng = 1;
        uint32_t minBrightness = 0, maxBrightness = 100, chaos = 0;
        float peak = 100;
        uint32_t lastUpdate = 0;
        int lastDuty = -1;
        int pwmChannel = -1;
    };
    std::list<Channel> channels;
    // One bit per physical pin: per-pin polarity override.
    uint64_t polarityOverrides = 0, inversePins = 0;
    int _inverse = 0;
    int _indication = 1;
    // ESP32: only explicitly reserved, otherwise unused LEDC timer pairs.
    int _pwmChannelMask = 0;
    static constexpr uint32_t carrierHz = 1000;
    static constexpr uint32_t pwmMax = 1023;
    static constexpr uint32_t updateMs = 10;

    static bool isSmooth(const Channel& c) {
        return c.mode == Mode::Smooth || c.mode == Mode::SmoothRandom;
    }

    bool preparePWM(Channel& c) {
#if defined(ESP8266)
        (void)c;
        return true;
#elif defined(ESP32)
#if defined(SOC_LEDC_SUPPORT_HS_MODE) && SOC_LEDC_SUPPORT_HS_MODE
        const int count = SOC_LEDC_CHANNEL_NUM * 2;
#else
        const int count = SOC_LEDC_CHANNEL_NUM;
#endif
        if (_pwmChannelMask <= 0) return false;
        const uint32_t mask = static_cast<uint32_t>(_pwmChannelMask);
        for (int ch = 0; ch < count; ++ch) {
            // Both channels sharing a timer must be reserved for Blink.
            uint32_t pair = 3u << (ch & ~1);
            if ((mask & pair) != pair) continue;
            bool used = false;
            for (const auto& other : channels)
                if (other.pin != c.pin && other.pwmChannel == ch) used = true;
            if (!used) { c.pwmChannel = ch; return true; }
        }
        return false;
#else
        (void)c;
        return false;
#endif
    }

    bool attachPWM(Channel& c) {
#if defined(ESP8266)
        (void)c;
        return true; // core waveform API; no change of global analogWrite settings
#elif defined(ESP32)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        return ledcAttachChannel(c.pin, carrierHz, 10, c.pwmChannel);
#else
        if (ledcSetup(c.pwmChannel, carrierHz, 10) == 0) return false;
        ledcWrite(c.pwmChannel, c.inverse ? pwmMax : 0);
        ledcAttachPin(c.pin, c.pwmChannel);
        return true;
#endif
#else
        (void)c;
        return false;
#endif
    }

    bool writeBrightness(Channel& c, float brightness) {
        // Perceptual brightness (0..100) -> gamma corrected electrical duty.
        float normalized = brightness / 100.0f;
        if (normalized < 0) normalized = 0;
        if (normalized > 1) normalized = 1;
        uint32_t duty = static_cast<uint32_t>(powf(normalized, 2.2f) * pwmMax + 0.5f);
        if (c.inverse) duty = pwmMax - duty;
        if (c.lastDuty == static_cast<int>(duty)) return true;
#if defined(ESP8266)
        if (duty == 0 || duty == pwmMax) {
            IoTgpio.digitalWrite(c.pin, duty ? HIGH : LOW);
        } else {
            // 1 kHz independent waveform, no analogWriteRange/Freq side effects.
            uint32_t high = (1000u * duty + pwmMax / 2) / pwmMax;
            if (!startWaveform(c.pin, high, 1000u - high)) return false;
        }
#elif defined(ESP32)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        if (!ledcWrite(c.pin, duty)) return false;
#else
        ledcWrite(c.pwmChannel, duty);
#endif
#else
        return false;
#endif
        c.lastDuty = static_cast<int>(duty);
        return true;
    }

    void finish(Channel& c) {
#if defined(ESP32)
        if (isSmooth(c) && c.pwmChannel >= 0) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
            ledcDetach(c.pin);
#else
            ledcDetachPin(c.pin);
#endif
            IoTgpio.pinMode(c.pin, OUTPUT);
        }
#endif
        // ESP8266 digitalWrite stops any waveform on this pin.
        write(c, false);
    }

    static bool readUInt(const IoTValue& v, uint32_t& out)
    {
        // IoTValue.valD is float in ver4stable; all integers up to 2^24 are exact.
        double n = v.valD;
        if (!v.isDecimal || !isfinite(n) || n < 0 ||
            n > 16777216.0 || floor(n) != n) return false;
        out = static_cast<uint32_t>(n);
        return true;
    }

    static bool readPin(const IoTValue& v, int& pin)
    {
        uint32_t n;
        if (!readUInt(v, n) || n >= 64) return false;
#if defined(ESP8266)
        // GPIO6..11 are normally wired to SPI flash; conservative exclusion.
        if (n > 16 || (n >= 6 && n <= 11)) return false;
#elif defined(ESP32)
        if (!GPIO_IS_VALID_OUTPUT_GPIO(static_cast<gpio_num_t>(n))) return false;
#else
        // Physical GPIO only. Check board pinout for other supported targets.
        if (n >= NUM_DIGITAL_PINS) return false;
#endif
        pin = static_cast<int>(n);
        return true;
    }

    bool inverseFor(int pin) const
    {
        uint64_t bit = UINT64_C(1) << pin;
        return (polarityOverrides & bit) ? ((inversePins & bit) != 0) : (_inverse != 0);
    }

    void write(Channel& c, bool on)
    {
        c.on = on;
        IoTgpio.digitalWrite(c.pin, (on != c.inverse) ? HIGH : LOW);
    }

    void status(bool genEvent = false)
    {
        int active = channels.empty() ? 0 : 1;
        bool changed = value.valD != active;
        if (!changed) return;
        value.valD = active;
        value.isDecimal = true;
        value.valS = String(active);
        if (changed && _indication)
            regEvent(value.valS, "Blink", false, genEvent);
    }

    bool removePin(int pin)
    {
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            if (it->pin == pin) {
                finish(*it);
                channels.erase(it);
                return true;
            }
        }
        return false;
    }

    void clearChannels()
    {
        for (auto& c : channels) finish(c);
        channels.clear();
    }

    static uint32_t randomBelow(Channel& c, uint32_t bound)
    {
        if (bound <= 1) return 0;
        // xorshift32; channel-specific state, never zero.
        uint32_t x = c.rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c.rng = x;
        return x % bound;
    }

    static void nextRandomPeriod(Channel& c)
    {
        c.period = c.meanPeriod - c.periodSpread +
                   randomBelow(c, 2 * c.periodSpread + 1);
        uint32_t duty = c.meanDuty - c.dutySpread +
                        randomBelow(c, 2 * c.dutySpread + 1);
        c.pulse = static_cast<uint32_t>(uint64_t(c.period) * duty / 100);
    }

    static bool makeRandom(const std::vector<IoTValue>& param, Channel& c)
    {
        if (param.size() != 5 && param.size() != 6) return false;
        double frequency = param[1].valD;
        uint32_t chaos, duty, duration, dutyChaos = 0;
        // Compare as float at the endpoints because IoTValue stores float.
        if (!param[1].isDecimal || !isfinite(frequency) ||
            frequency < 0.01f || frequency > 100.0 ||
            !readUInt(param[2], chaos) || chaos > 100 ||
            !readUInt(param[3], duty) || duty > 100 ||
            !readUInt(param[4], duration) ||
            (param.size() == 6 && !readUInt(param[5], dutyChaos))) return false;
        // For a flashing signal, keep every duty value within 1..99%.
        // 0 and 100 are allowed only as constant OFF/ON.
        if (duty == 0 || duty == 100) {
            if (dutyChaos != 0) return false;
        } else if (dutyChaos >= duty || dutyChaos >= 100 - duty) return false;
        c.mode = Mode::Random;
        c.total = duration;
        c.meanPeriod = static_cast<uint32_t>(1000.0 / frequency + 0.5);
        c.meanDuty = duty;
        c.dutySpread = dutyChaos;
        uint32_t minPeriod = 2;
        if (duty > 0 && duty < 100) {
            uint32_t minDuty = duty - dutyChaos;
            minPeriod = (100 + minDuty - 1) / minDuty;
        }
        if (c.meanPeriod < minPeriod) return false;
        c.periodSpread = c.meanPeriod * chaos / 100;
        // Clip both sides equally to retain the requested average period.
        if (c.periodSpread > c.meanPeriod - minPeriod)
            c.periodSpread = c.meanPeriod - minPeriod;
        return true;
    }

    static bool makeSmooth(const std::vector<IoTValue>& param, Channel& c, bool jitter)
    {
        if (param.size() != (jitter ? 6u : 5u)) return false;
        const size_t shift = jitter ? 1 : 0;
        if (!readUInt(param[2 + shift], c.minBrightness) ||
            !readUInt(param[3 + shift], c.maxBrightness) ||
            !readUInt(param[4 + shift], c.total) ||
            c.minBrightness > c.maxBrightness || c.maxBrightness > 100) return false;
        if (jitter) {
            const double hz = param[1].valD;
            if (!param[1].isDecimal || !isfinite(hz) || hz < 0.01f || hz > 5 ||
                !readUInt(param[2], c.chaos) || c.chaos > 100) return false;
            c.mode = Mode::SmoothRandom;
            c.meanPeriod = static_cast<uint32_t>(1000.0 / hz + 0.5);
            c.periodSpread = c.meanPeriod * c.chaos / 100;
            // Keep every complete pulse >=100 ms and preserve mean period.
            if (c.periodSpread > c.meanPeriod - 100) c.periodSpread = c.meanPeriod - 100;
        } else {
            if (!readUInt(param[1], c.period) || c.period < 100) return false;
            c.mode = Mode::Smooth;
            c.peak = c.maxBrightness;
        }
        return true;
    }

    static void nextSmoothPeriod(Channel& c) {
        c.period = c.meanPeriod - c.periodSpread + randomBelow(c, 2 * c.periodSpread + 1);
        float fraction = randomBelow(c, 10001) / 10000.0f;
        c.peak = c.maxBrightness - (c.maxBrightness - c.minBrightness) *
                 (c.chaos / 100.0f) * fraction;
    }

    bool tickSmooth(Channel& c, uint32_t now, uint32_t elapsed) {
        if (c.mode == Mode::SmoothRandom) {
            unsigned steps = 0;
            while (uint32_t(now - c.phaseStart) >= c.period) {
                c.phaseStart += c.period;
                nextSmoothPeriod(c);
                if (++steps == 32 && uint32_t(now - c.phaseStart) >= c.period) {
                    c.phaseStart = now;
                    break;
                }
            }
        }
        if (c.lastDuty >= 0 && uint32_t(now - c.lastUpdate) < updateMs) return true;
        c.lastUpdate = now;
        uint32_t phase = c.mode == Mode::Smooth ? elapsed % c.period : now - c.phaseStart;
        float angle = 6.283185307179586f * (float(phase) / c.period);
        float brightness = c.minBrightness + (c.peak - c.minBrightness) *
                           (1.0f - cosf(angle)) * 0.5f;
        return writeBrightness(c, brightness);
    }

    // Returns false when the channel has finished. No delay or busy waiting.
    bool tick(Channel& c, uint32_t now)
    {
        uint32_t elapsed = now - c.start;
        if (elapsed >= c.total) return false;
        if (isSmooth(c)) return tickSmooth(c, now, elapsed);
        bool on;
        if (c.mode == Mode::Periodic) {
            on = elapsed % c.period < c.pulse;
        } else if (c.mode == Mode::Random) {
            // Catch up to elapsed time, but bound work after a long loop stall.
            unsigned steps = 0;
            while (uint32_t(now - c.phaseStart) >= c.period) {
                c.phaseStart += c.period;
                nextRandomPeriod(c);
                if (++steps == 32 && uint32_t(now - c.phaseStart) >= c.period) {
                    c.phaseStart = now;
                    break;
                }
            }
            on = uint32_t(now - c.phaseStart) < c.pulse;
        } else {
            while (c.index < c.phases.size() &&
                   uint32_t(now - c.phaseStart) >= c.phases[c.index]) {
                c.phaseStart += c.phases[c.index++];
            }
            if (c.index == c.phases.size()) return false;
            on = (c.index % 2 == 0);
        }
        if (on != c.on) write(c, on);
        return true;
    }

    bool start(Channel&& c)
    {
        // Validation and pattern allocation have already completed.
        if (c.total && isSmooth(c) && !preparePWM(c)) {
            SerialPrint("E", "Blink: PWM unavailable; check reserved channels", String(c.pin), _id);
            return false;
        }
        removePin(c.pin);
        if (c.total == 0) { status(); return true; }
        c.inverse = inverseFor(c.pin);
        IoTgpio.pinMode(c.pin, OUTPUT);
        write(c, false);
        if (isSmooth(c) && !attachPWM(c)) {
            write(c, false);
            status();
            return false;
        }
        c.start = c.phaseStart = millis();
        if (c.mode == Mode::Random || c.mode == Mode::SmoothRandom) {
            c.rng = static_cast<uint32_t>(random(1L, 2147483647L)) ^
                    (uint32_t(c.pin + 1) * UINT32_C(2654435761)) ^ c.start;
            if (!c.rng) c.rng = 1;
            if (c.mode == Mode::Random) nextRandomPeriod(c);
            else nextSmoothPeriod(c);
        }
        channels.push_back(std::move(c));
        if (!tick(channels.back(), channels.back().start)) {
            finish(channels.back());
            channels.pop_back();
            status();
            return false;
        }
        status();
        return true;
    }

    static bool nextUtf8(const unsigned char*& p, uint32_t& cp)
    {
        unsigned char a = *p++;
        if (a < 0x80) { cp = a; return true; }
        // Supported non-ASCII characters are Cyrillic (two-byte UTF-8).
        if (a < 0xC2 || a > 0xDF || !*p || (*p & 0xC0) != 0x80)
            return false;
        cp = ((a & 0x1F) << 6) | (*p++ & 0x3F);
        return true;
    }

    static const char* morseCode(uint32_t cp)
    {
        static const char* const en[] = {
            ".-","-...","-.-.","-..",".","..-.","--.","....","..",".---",
            "-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-",
            "..-","...-",".--","-..-","-.--","--.."
        };
        // А..Я in Unicode order (Ё is separate and uses the code of Е).
        static const char* const ru[] = {
            ".-","-...",".--","--.","-..",".","...-","--..","..",".---",
            "-.-",".-..","--","-.","---",".--.",".-.","...","-","..-",
            "..-.","....","-.-.","---.","----","--.-","--.--","-.--","-..-","..-..",
            "..--",".-.-"
        };
        static const char* const digits[] = {
            "-----",".----","..---","...--","....-",".....","-....","--...","---..","----."
        };
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        if (cp >= 0x0430 && cp <= 0x044F) cp -= 32;
        if (cp == 0x0401 || cp == 0x0451) cp = 0x0415;
        if (cp >= 'A' && cp <= 'Z') return en[cp - 'A'];
        if (cp >= 0x0410 && cp <= 0x042F) return ru[cp - 0x0410];
        if (cp >= '0' && cp <= '9') return digits[cp - '0'];
        switch (cp) {
            case '.': return ".-.-.-";
            case ',': return "--..--";
            case '?': return "..--..";
            case '!': return "-.-.--";
            case '/': return "-..-.";
            case '-': return "-....-";
            case ':': return "---...";
            case ';': return "-.-.-.";
            case '(': return "-.--.";
            case ')': return "-.--.-";
            case '\'': return ".----.";
            case '"': return ".-..-.";
            case '=': return "-...-";
            case '+': return ".-.-.";
            case '@': return ".--.-.";
            default: return nullptr;
        }
    }

    static bool makeMorse(const String& text, uint32_t speed, Channel& c)
    {
        if (speed < 1 || speed > 120 || text.length() > 512) return false;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text.c_str());
        uint32_t unit = (1200 + speed / 2) / speed;
        bool wordGap = false;
        size_t symbols = 0;
        while (*p) {
            uint32_t cp;
            if (!nextUtf8(p, cp)) return false;
            if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
                wordGap = true;
                continue;
            }
            const char* code = morseCode(cp);
            if (!code || ++symbols > 128) return false;
            if (!c.phases.empty()) c.phases.push_back(unit * (wordGap ? 7 : 3));
            wordGap = false;
            for (const char* s = code; *s; ++s) {
                if (s != code) c.phases.push_back(unit);
                c.phases.push_back(unit * (*s == '-' ? 3 : 1));
            }
        }
        if (c.phases.empty()) return false;
        for (uint32_t d : c.phases) c.total += d;
        c.mode = Mode::Pattern;
        return true;
    }

    IoTValue result(bool ok)
    {
        IoTValue v;
        v.valD = ok ? 1 : 0;
        v.valS = String(static_cast<int>(v.valD));
        v.isDecimal = true;
        return v;
    }

    IoTValue error(const String& command)
    {
        SerialPrint("E", "Blink: invalid parameters", command, _id);
        return result(false);
    }

public:
    Blink(String parameters) : IoTItem(parameters)
    {
        jsonRead(parameters, "inverse", _inverse, false);
        jsonRead(parameters, "indication", _indication, false);
        jsonRead(parameters, "pwmChannelMask", _pwmChannelMask, false);
        value.valD = 0;
        value.valS = "0";
        value.isDecimal = true;
        // No GPIO configured until a valid command targets it.
    }

    void loop() override
    {
        uint32_t now = millis();
        for (auto it = channels.begin(); it != channels.end();) {
            if (!tick(*it, now)) {
                if (isSmooth(*it) && uint32_t(now - it->start) < it->total)
                    SerialPrint("E", "Blink: PWM update failed", String(it->pin), _id);
                finish(*it);
                it = channels.erase(it);
            } else ++it;
        }
        status();
    }

    IoTValue execute(String command, std::vector<IoTValue>& param) override
    {
        if (command == "stopAll") {
            if (!param.empty()) return error(command);
            clearChannels(); status();
            SerialPrint("I", "Blink, stopAll()", "OK", _id);
            return result(true);
        }
        int pin;
        if (param.empty() || !readPin(param[0], pin)) return error(command);
        if (command == "stop" || command == "busy") {
            if (param.size() != 1) return error(command);
            if (command == "busy") {
                for (const auto& c : channels) if (c.pin == pin) return result(true);
                return result(false);
            }
            removePin(pin); status();
        } else if (command == "polarity") {
            uint32_t inv;
            if (param.size() != 2 || !readUInt(param[1], inv) || inv > 1) return error(command);
            // Change polarity only when the channel is idle.
            for (const auto& c : channels) if (c.pin == pin) return error(command);
            uint64_t bit = UINT64_C(1) << pin;
            polarityOverrides |= bit;
            if (inv) inversePins |= bit; else inversePins &= ~bit;
            // Apply the new idle level immediately.
            IoTgpio.pinMode(pin, OUTPUT);
            IoTgpio.digitalWrite(pin, inv ? HIGH : LOW);
        } else {
            Channel c;
            c.pin = pin;
            if (command == "duration" || command == "times") {
                uint32_t duty, amount;
                if (param.size() != 4 || !readUInt(param[1], c.period) ||
                    !readUInt(param[2], duty) || !readUInt(param[3], amount) ||
                    c.period == 0 || duty > 100) return error(command);
                uint64_t total = command == "times" ? uint64_t(c.period) * amount : amount;
                if (total > 2147483647ULL) return error(command);
                c.total = static_cast<uint32_t>(total);
                c.pulse = static_cast<uint32_t>(uint64_t(c.period) * duty / 100);
                // Reject unrepresentable nonzero ON/OFF durations at ms resolution.
                if (duty > 0 && duty < 100 && (c.pulse == 0 || c.pulse == c.period)) return error(command);
            } else if (command == "random") {
                if (!makeRandom(param, c)) return error(command);
            } else if (command == "smooth" || command == "smoothRandom") {
                if (!makeSmooth(param, c, command == "smoothRandom")) return error(command);
            } else if (command == "free") {
                if (param.size() < 2 || param.size() > 257) return error(command);
                c.mode = Mode::Pattern;
                uint64_t total = 0;
                c.phases.reserve(param.size() - 1);
                for (size_t i = 1; i < param.size(); ++i) {
                    uint32_t d;
                    if (!readUInt(param[i], d)) return error(command);
                    c.phases.push_back(d);
                    total += d;
                }
                if (total == 0 || total > 2147483647ULL) return error(command);
                c.total = static_cast<uint32_t>(total);
            } else if (command == "morze") {
                uint32_t speed;
                if (param.size() != 3 || param[1].isDecimal ||
                    !readUInt(param[2], speed) || !makeMorse(param[1].valS, speed, c)) return error(command);
            } else return error(command);
            if (!start(std::move(c))) return result(false);
        }
        SerialPrint("I", String("Blink, ") + command + "()", String("pin = ") + String(pin), _id);
        return result(true);
    }

    void setValue(const IoTValue& v, bool genEvent = true) override
    {
        // Assignment 0 is an emergency stop; 1 cannot select a GPIO.
        if (v.isDecimal && v.valD == 0) clearChannels();
        status(genEvent);
    }

    String getValue() override { return String(channels.empty() ? 0 : 1); }
    ~Blink() override { clearChannels(); }
};

void* getAPI_Blink(String subtype, String param)
{
    if (subtype == F("Blink")) return new Blink(param);
    return nullptr;
}
