#pragma once

// The notification LED once a download or a transfer ends: short, long, short, green when it went well and
// red when it failed (a cancel shows nothing), again every couple of seconds until a button or the touch
// screen is pressed. Through mcu::HWC; without it, nothing happens. Any thread may ask; the UI loop plays it.
void led_blink(bool ok);
// From the UI loop, once a frame: starts what was asked for, stops it on a press.
void led_update();
// Off, when NSA closes.
void led_exit();

// A pattern of `blinks` times `on` slots lit then `off` dark, for trying timings (SITE LED). The 32 slots
// together last `delay` / 16 s; `loopDelay` / 16 s of the last slot between repeats (0xFF: played once).
Result led_pattern(bool ok, u8 delay, u8 smoothing, u8 loopDelay, u8 blinkSpeed, u32 blinks, u32 on, u32 off);
