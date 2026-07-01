# Arduino Low Power Library (Reliable Standby Fork)

This is a highly optimized, ultra-reliable fork of the official `ArduinoLowPower` library, maintained by **BlueBell_XA** (v2.0.0). It is designed as a drop-in replacement specifically to resolve standby wake-up unreliability and race conditions on **ATSAMD21** microcontrollers.

## 🚀 Key Improvements & Reliability Enhancements

This fork replaces the original "configure-once, sleep-forever" model with a deterministic state-machine that completely reconstructs the External Interrupt Controller (EIC) hardware state before *every single sleep cycle*.

1. **Deterministic Hardware Reconstruction:** Immediately before entering standby, the EIC is disabled, cleared, and rebuilt from scratch to ensure any accumulated or corrupted registers since boot do not cause missed wakeup events.
2. **Polite, Targeted Modifications:** Only EIC channels explicitly registered via `attachInterruptWakeup` are touched. All other channels, registers, and configurations remain completely unaffected (excellent citizen for multi-peripheral libraries).
3. **Existing State Preservation:** Fully preserves any other existing register states like the hardware noise filter (`FILTEN`).
4. **NVIC & Flag Cleaning:** Clears EIC interrupt flags (`INTFLAG`) while the EIC is disabled, and clears pending NVIC (`EIC_IRQn`) interrupts immediately before sleep to resolve Cortex-M0+ pending interrupt edge cases.
5. **No Sleep-Time Clock Jitter:** Disabling/re-enabling of GCLK clock trees is kept inside `attachInterruptWakeup()`, avoiding clock manipulation overhead inside the critical sleep path.
6. **Instruction Barriers:** Added Data Synchronization (`__DSB()`) and Instruction Synchronization (`__ISB()`) memory barriers immediately prior to entering standby (`__WFI()`) to guarantee all register writes are physically completed before execution stops.
7. **Comprehensive Debug Mode:** Features a robust, human-readable EIC debugger that prints decoded, real-time register configurations and wake sources.

---

## 🛠️ Usage & API

The public API is **100% backward-compatible** and acts as a direct, drop-in replacement for existing sketches.

```cpp
#include "ArduinoLowPower.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(8, INPUT_PULLUP);
  
  // Register pin 8 for wakeup on FALLING edge
  LowPower.attachInterruptWakeup(8, onWake, FALLING);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(1000);
  digitalWrite(LED_BUILTIN, LOW);
  
  // Put device into low-power Standby mode
  LowPower.sleep(); 
}

void onWake() {
  // Wake handler
}
```

---

## 🔍 Advanced EIC Debugging

To diagnose wake issues or monitor your sleep/wake cycles, you can enable the built-in EIC debugger.

Open `src/ArduinoLowPower.h` and uncomment the following line:

```cpp
#define LOWPOWER_DEBUG_EIC
```

When enabled, the library will print real-time register decoding and statistics to `Serial`:

### Debug Output Example:
```
--- LOWPOWER DEBUG: Preparing for standby ---
PMUX check passed: All active wake-source pins are routed to EIC.
EXTINT3 (Pin 8) -> Mode: FALLING [FILTEN] enabled

WAKEUP   = 0x0008
INTENSET = 0x0008
INTFLAG  = 0x0000
CONFIG0  = 0x00002000
CONFIG1  = 0x00000000
Entering standby...

--- LOWPOWER DEBUG: After wake ---
INTFLAG = 0x0008
WAKEUP  = 0x0008
Wake source detected: EXTINT3 (Pin 8)
Stats: sleeps=1, wakes=1, spurious_wakes=0
------------------------------------
```

---

## ⚖️ License

Copyright (c) Arduino LLC. All right reserved.
Maintained and updated by BlueBell_XA.

This library is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General Public License as published by the Free Software Foundation; either version 2.1 of the License, or (at your option) any later version.
