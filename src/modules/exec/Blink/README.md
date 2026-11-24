# Blink 2.2 — IoTManager ver4stable

Copy `src/modules/exec/Blink/` into the same directory in IoTManager, enable the module using the normal build configuration and rebuild. No fixed pin in the module configuration. Use one instance with id `blink`.

Commands: `duration(pin,period,duty,duration)`, `times(pin,period,duty,times)`, `free(pin,ON,OFF,...)`, `morze(pin,"text",velocity)`, `random(pin,frequency,chaos,duty,duration[,dutyChaos])`, `smooth(pin,period,minBrightness,maxBrightness,duration)`, `smoothRandom(pin,frequency,chaos,minBrightness,maxBrightness,duration)`, `stop(pin)`, `stopAll()`, `busy(pin)`, `polarity(pin,0/1)`. Successful commands return 1; invalid parameters return 0 and preserve existing sequences. `busy` returns activity, not instantaneous GPIO state. `stop` only stops a channel owned by this instance. `blink = 0` stops all; `blink = 1` has no effect. Default active HIGH, `polarity` overrides per idle GPIO until reboot. `times` counts full periods including final OFF. Zero count/duration stops an active channel. Duty 0 and 100 mean continuous OFF and ON for the specified time. Intermediate duty uses integer milliseconds rounded down; unrepresentable ON pulses are rejected.

Physical output GPIO only. ESP8266 GPIO6–11 excluded conservatively because typically attached to flash; GPIO16 supported. ESP32 checks GPIO_IS_VALID_OUTPUT_GPIO; board-specific flash/PSRAM, USB, UART, boot straps and other occupied pins must still be excluded by the user. No coordination with other GPIO-owning modules. No virtual expander pins. Digital-mode timing is cooperative via loop(); missed transitions are not replayed. Use adequate loop frequency. No GPIO accessed at startup.

Morse: WPM, 1–120, dot = rounded 1200/WPM ms; dot/dash/gaps = 1/3/1/3/7 units. 128 non-space characters, max 512 UTF-8 bytes. Russian Ё uses Е; punctuation uses common international codes. Unknown characters reject the command. Free: 256 intervals plus pin; zero phases skipped; odd length supported. Numeric arguments limited to 2^24 due to float IoTValue; total duration capped at 2^31−1 ms. Memory dynamically allocated per active channel; practical simultaneous pattern limit depends on free heap.

Host test (mock GPIO/time, not firmware):
```
g++ -std=c++11 -Wall -Wextra -Werror -DESP8266 -Itests/stubs tests/test_blink.cpp -o /tmp/blink_test
/tmp/blink_test
```

The original project parser uses an argument vector; maximum 257 arguments has not been tested on actual firmware. ESP32/ESP8266 PlatformIO builds and hardware tests are still required. `export.json` follows the IoTManager JSON + scenario=> format, and is not a standalone JSON document.

## Random mode (2.1)

`blink.random(15,2,80,50,10000,40)` produces uniformly random periods around 500 ms (100–900 ms) and duty values 10–90%, independently per cycle. The optional sixth argument is duty jitter in percentage points (default 0). Frequency can be fractional, 0.01–100 Hz; all other arguments are integers. Duration 0 stops the selected channel. Duty 0/100 produces constant OFF/ON with no duty jitter. For intermediate duty the full duty range must stay within 1–99%. Period jitter is clipped symmetrically when necessary to guarantee at least 1 ms ON and OFF; if the mean period is too short for the requested minimum duty the command is rejected. At 0% chaos and no duty jitter the output is periodic. Actual mean flash rate is approximate (integer ms and finite sample). High rates and short phases depend on loop responsiveness. Long loop stalls skip missed transitions; after 32 overdue periods the random cycle is resynchronized without extending the overall duration. No pattern buffer or new dependencies; independent PRNG state per channel.

## Smooth modes (2.2)

- `blink.smooth(15,3000,10,100,20000)` — a 3 s breathing cycle, 10–100% perceptual brightness, for 20 s.
- `blink.smoothRandom(15,0.5,80,20,100,60000)` — random cycle periods around 2 s (400–3600 ms), random peak brightness between 36% and 100%, always returning to 20% at cycle boundaries. Chaos 0 gives regular breathing. Chaos also controls peak variation. Frequency 0.01–5 Hz; minimum cycle 100 ms, symmetric jitter clipping. `smooth` period >=100 ms. Brightness bounds integer 0–100, minimum <= maximum. Duration 0 cancels; normal completion/stop switches OFF, not the minimum brightness. Inversion supported. Gamma 2.2 fixed; electrical PWM duty = (brightness/100)^2.2. Brightness updates at most once per 10 ms; loop stalls can cause jumps. The carrier runs continuously between updates.

### ESP8266 (no additional config)

Uses Arduino core `startWaveform` (1 kHz software PWM carrier, timer-driven) directly; does not change global analogWrite frequency or range. Initial and final digitalWrite stop the waveform/PWM on that pin. Pwm8266 can resume with a new command after Blink finishes. No simultaneous control of the same pin. Other PWM pins are not intentionally reconfigured; all waveforms share core timer resources, so load and timing should be tested on hardware.

### ESP32 (explicit LEDC reservation required)

Set `pwmChannelMask` in Blink config to the decimal mask of unused LEDC channel pairs. Default 0 disables only smooth modes on ESP32. Original ESP32: e.g. 49152 (channels 14/15), ESP32-S2/S3: 192 (6/7), ESP32-C3/C6: 48 (4/5), **only if those channels and shared timers are unused by other code**. Each bit represents one channel. Reserve both members of each timer pair, and reserve the same resources in all other modules; a pair supports two simultaneous smooth channels. Arduino Core 2.x does not reliably track manually assigned LEDC channels and Core 3.x can share an explicitly assigned channel, so Blink cannot automatically detect all external allocations. Do not use the same mask in multiple Blink instances. Uses LEDC 1 kHz/10-bit, detaches the pin at completion. Unlike ESP8266, an existing Pwm32 on the same pin is not automatically restored by simply writing its duty; reattachment may be needed. Use separate pins or explicitly coordinate ownership. No automatic previous-state restoration. Unsupported MCU targets reject smooth commands. If no reserved channel is available, command returns 0 without replacing existing sequences; backend attach/update failure ends the affected channel.

Host tests mock PWM, GPIO and time. They do not validate physical timing, actual LEDC allocation or Arduino firmware linking.
