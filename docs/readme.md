# Arduino Low Power Library (Reliable Standby Fork)

This library allows you to use the low power features of the SAMD21 MCU, which is used for all [MKR family boards](https://store.arduino.cc/collections/mkr-family) and the [Nano 33 IoT board](https://store.arduino.cc/products/arduino-nano-33-iot).

This version is a highly maintained and updated fork (v2.0.0) by **BlueBell_XA** focused on absolute standby wake-up reliability for long-term deployments.

To use this library:

```cpp
#include "ArduinoLowPower.h"
```

## 📚 Examples:

- [ExternalWakeup](../examples/ExternalWakeup/ExternalWakeup.ino) : Demonstrates how to wake your board from an external source like a button.
- [TianStandby](../examples/TianStandby/TianStandby.ino) : Demonstrates how to put a Tian in standby.
- [TimedWakeup](../examples/TimedWakeup/TimedWakeup.ino) : Demonstrates how to put your board to sleep for a certain amount of time.
- [AdcWakeup](../examples/AdcWakeup/AdcWakeup.ino) : Demonstrates how to wake your board on analog input window transitions.
