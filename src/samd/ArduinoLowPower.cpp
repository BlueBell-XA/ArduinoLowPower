#if defined(ARDUINO_ARCH_SAMD)

#include "ArduinoLowPower.h"

ArduinoLowPowerClass::ArduinoLowPowerClass() : adc_cb(nullptr), sleep_count(0), wake_count(0), spurious_wake_count(0), last_wake_intflag(0), last_wake_wakeup(0) {
	memset(wakeupSources, 0, sizeof(wakeupSources));
}

static void configGCLK6()
{
	// enable EIC clock
	GCLK->CLKCTRL.bit.CLKEN = 0; //disable GCLK module
	while (GCLK->STATUS.bit.SYNCBUSY);

	GCLK->CLKCTRL.reg = (uint16_t) (GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK6 | GCLK_CLKCTRL_ID( GCM_EIC )) ;  //EIC clock switched on GCLK6
	while (GCLK->STATUS.bit.SYNCBUSY);

	GCLK->GENCTRL.reg = (GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_ID(6));  //source for GCLK6 is OSCULP32K
	while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY);

	GCLK->GENCTRL.bit.RUNSTDBY = 1;  //GCLK6 run standby
	while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY);

	/* Errata: Make sure that the Flash does not power all the way down
     	* when in sleep mode. */

	NVMCTRL->CTRLB.bit.SLEEPPRM = NVMCTRL_CTRLB_SLEEPPRM_DISABLED_Val;
}

void ArduinoLowPowerClass::idle() {
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
	PM->SLEEP.reg = 2;
	__DSB();
	__WFI();
}

void ArduinoLowPowerClass::idle(uint32_t millis) {
	setAlarmIn(millis);
	idle();
}

void ArduinoLowPowerClass::sleep() {
	bool restoreUSBDevice = false;
	if (SERIAL_PORT_USBVIRTUAL) {
		USBDevice.standby();
	} else {
		USBDevice.detach();
		restoreUSBDevice = true;
	}
	// Disable systick interrupt:  See https://www.avrfreaks.net/forum/samd21-samd21e16b-sporadically-locks-and-does-not-wake-standby-sleep-mode
	SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;	
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;

#ifdef LOWPOWER_DEBUG_EIC
	// Before sleeping, verify the Port Pin multiplexing is still routed to Peripheral A (EIC)
	Serial.println("--- LOWPOWER DEBUG: Preparing for standby ---");
	bool pmux_ok = true;
	for (int i = 0; i < 16; i++) {
		if (wakeupSources[i].active) {
			uint32_t pin = wakeupSources[i].pin;
			uint32_t port = g_APinDescription[pin].ulPort;
			uint32_t pin_num = g_APinDescription[pin].ulPin;
			uint32_t pmux_reg = PORT->Group[port].PMUX[pin_num >> 1].reg;
			uint32_t pin_pcfg = PORT->Group[port].PINCFG[pin_num].reg;
			uint32_t pmux_val = (pin_num & 1) ? (pmux_reg >> 4) : (pmux_reg & 0xF);
			// Peripheral A is EIC (0)
			bool is_pmux_peripheral_a = (pmux_val == 0) && (pin_pcfg & PORT_PINCFG_PMUXEN);
			if (!is_pmux_peripheral_a) {
				pmux_ok = false;
				Serial.print("WARNING: Pin "); Serial.print(pin);
				Serial.print(" (EXTINT"); Serial.print(i);
				Serial.print(") is NOT configured for EIC multiplexing! PMUX="); Serial.print(pmux_val);
				Serial.print(", PINCFG="); Serial.println(pin_pcfg, HEX);
			}
		}
	}
	if (pmux_ok) {
		Serial.println("PMUX check passed: All active wake-source pins are routed to EIC.");
	}
#endif

	// Increment sleep counter
	sleep_count++;

	// 1. Disable EIC to modify configurations safely
	EIC->CTRL.bit.ENABLE = 0;
	while (EIC->STATUS.bit.SYNCBUSY);

	// 2. Clear EIC interrupt flags to prevent stale pending interrupts
	EIC->INTFLAG.reg = EIC->INTFLAG.reg;

	// 3. Rebuild EIC configuration for our registered wake sources
	uint32_t config0 = EIC->CONFIG[0].reg;
	uint32_t config1 = EIC->CONFIG[1].reg;
	uint32_t wakeup_reg = EIC->WAKEUP.reg;
	uint32_t intenset_reg = EIC->INTENSET.reg;

	for (int i = 0; i < 16; i++) {
		if (wakeupSources[i].active) {
			uint32_t pos = (i < 8) ? (i << 2) : ((i - 8) << 2);
			uint32_t sense_val = 0;
			switch (wakeupSources[i].mode) {
				#ifdef ARDUINO_API_VERSION
				case PinStatus::LOW: sense_val = EIC_CONFIG_SENSE0_LOW_Val; break;
				case PinStatus::HIGH: sense_val = EIC_CONFIG_SENSE0_HIGH_Val; break;
				case PinStatus::CHANGE: sense_val = EIC_CONFIG_SENSE0_BOTH_Val; break;
				case PinStatus::FALLING: sense_val = EIC_CONFIG_SENSE0_FALL_Val; break;
				case PinStatus::RISING: sense_val = EIC_CONFIG_SENSE0_RISE_Val; break;
				#else
				case LOW: sense_val = EIC_CONFIG_SENSE0_LOW_Val; break;
				case HIGH: sense_val = EIC_CONFIG_SENSE0_HIGH_Val; break;
				case CHANGE: sense_val = EIC_CONFIG_SENSE0_BOTH_Val; break;
				case FALLING: sense_val = EIC_CONFIG_SENSE0_FALL_Val; break;
				case RISING: sense_val = EIC_CONFIG_SENSE0_RISE_Val; break;
				#endif
				default: sense_val = EIC_CONFIG_SENSE0_NONE_Val; break;
			}

			// Modify only the SENSE bits in the target CONFIG nibble (preserving other fields)
			if (i < 8) {
				config0 &= ~(EIC_CONFIG_SENSE0_Msk << pos);
				config0 |= (sense_val << pos);
				if (wakeupSources[i].filten) {
					config0 |= (8 << pos); // Set FILTEN (bit 3 of nibble)
				} else {
					config0 &= ~(8 << pos); // Clear FILTEN
				}
			} else {
				config1 &= ~(EIC_CONFIG_SENSE0_Msk << pos);
				config1 |= (sense_val << pos);
				if (wakeupSources[i].filten) {
					config1 |= (8 << pos); // Set FILTEN
				} else {
					config1 &= ~(8 << pos); // Clear FILTEN
				}
			}

			wakeup_reg |= (1 << i);
			intenset_reg |= (1 << i);
		}
	}

	// Write rebuilding EIC configurations and synchronize after each write
	EIC->CONFIG[0].reg = config0;
	while (EIC->STATUS.bit.SYNCBUSY);

	EIC->CONFIG[1].reg = config1;
	while (EIC->STATUS.bit.SYNCBUSY);

	EIC->WAKEUP.reg = wakeup_reg;
	while (EIC->STATUS.bit.SYNCBUSY);

	EIC->INTENSET.reg = intenset_reg;
	while (EIC->STATUS.bit.SYNCBUSY);

	// 4. Re-enable EIC and synchronize
	EIC->CTRL.bit.ENABLE = 1;
	while (EIC->STATUS.bit.SYNCBUSY);

	// Sanity check: Ensure EIC is enabled
	if (EIC->CTRL.bit.ENABLE == 0) {
		#ifdef LOWPOWER_DEBUG_EIC
		Serial.println("CRITICAL ERROR: EIC failed to enable after synchronization!");
		#endif
	}

#ifdef LOWPOWER_DEBUG_EIC
	// Print active sources and register dump
	for (int i = 0; i < 16; i++) {
		if (wakeupSources[i].active) {
			Serial.print("EXTINT"); Serial.print(i);
			Serial.print(" (Pin "); Serial.print(wakeupSources[i].pin);
			Serial.print(") -> Mode: ");
			switch (wakeupSources[i].mode) {
				#ifdef ARDUINO_API_VERSION
				case PinStatus::LOW: Serial.print("LOW"); break;
				case PinStatus::HIGH: Serial.print("HIGH"); break;
				case PinStatus::CHANGE: Serial.print("CHANGE"); break;
				case PinStatus::FALLING: Serial.print("FALLING"); break;
				case PinStatus::RISING: Serial.print("RISING"); break;
				#else
				case LOW: Serial.print("LOW"); break;
				case HIGH: Serial.print("HIGH"); break;
				case CHANGE: Serial.print("CHANGE"); break;
				case FALLING: Serial.print("FALLING"); break;
				case RISING: Serial.print("RISING"); break;
				#endif
				default: Serial.print("UNKNOWN"); break;
			}
			if (wakeupSources[i].filten) {
				Serial.print(" [FILTEN]");
			}
			Serial.println(" enabled");
		}
	}
	Serial.println();
	Serial.print("WAKEUP   = 0x"); Serial.println(EIC->WAKEUP.reg, HEX);
	Serial.print("INTENSET = 0x"); Serial.println(EIC->INTENSET.reg, HEX);
	Serial.print("INTFLAG  = 0x"); Serial.println(EIC->INTFLAG.reg, HEX);
	Serial.print("CONFIG0  = 0x"); Serial.println(EIC->CONFIG[0].reg, HEX);
	Serial.print("CONFIG1  = 0x"); Serial.println(EIC->CONFIG[1].reg, HEX);
	Serial.println("Entering standby...");
	Serial.flush();
#endif

	// 5. Clear NVIC pending interrupts for EIC
	NVIC_ClearPendingIRQ(EIC_IRQn);

	// Execute deep sleep
	__DSB();
	__ISB();
	__WFI();

	// Enable systick interrupt
	SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;	

	// Capture wake-up flags immediately after waking before they are altered
	last_wake_intflag = EIC->INTFLAG.reg;
	last_wake_wakeup = EIC->WAKEUP.reg;

	// Increment wake counter
	wake_count++;

#ifdef LOWPOWER_DEBUG_EIC
	Serial.println("--- LOWPOWER DEBUG: After wake ---");
	Serial.print("INTFLAG = 0x"); Serial.println(last_wake_intflag, HEX);
	Serial.print("WAKEUP  = 0x"); Serial.println(last_wake_wakeup, HEX);
	
	bool found_source = false;
	for (int i = 0; i < 16; i++) {
		if (last_wake_intflag & (1 << i)) {
			Serial.print("Wake source detected: EXTINT"); Serial.print(i);
			if (wakeupSources[i].active) {
				Serial.print(" (Pin "); Serial.print(wakeupSources[i].pin); Serial.print(")");
			}
			Serial.println();
			found_source = true;
		}
	}
	if (!found_source) {
		spurious_wake_count++;
		Serial.println("Spurious or other wakeup source triggered wake (non-EIC).");
	}
	Serial.print("Stats: sleeps="); Serial.print(sleep_count);
	Serial.print(", wakes="); Serial.print(wake_count);
	Serial.print(", spurious_wakes="); Serial.println(spurious_wake_count);
	Serial.println("------------------------------------");
	Serial.flush();
#endif

	if (restoreUSBDevice) {
		USBDevice.attach();
	}
}

void ArduinoLowPowerClass::sleep(uint32_t millis) {
	setAlarmIn(millis);
	sleep();
}

void ArduinoLowPowerClass::deepSleep() {
	sleep();
}

void ArduinoLowPowerClass::deepSleep(uint32_t millis) {
	sleep(millis);
}

void ArduinoLowPowerClass::setAlarmIn(uint32_t millis) {

	if (!rtc.isConfigured()) {
		attachInterruptWakeup(RTC_ALARM_WAKEUP, NULL, (irq_mode)0);
	}

	uint32_t now = rtc.getEpoch();
	rtc.setAlarmEpoch(now + millis/1000);
	rtc.enableAlarm(rtc.MATCH_YYMMDDHHMMSS);
}

void ArduinoLowPowerClass::attachInterruptWakeup(uint32_t pin, voidFuncPtr callback, irq_mode mode) {

	if (pin > PINS_COUNT) {
		// check for external wakeup sources
		// RTC library should call this API to enable the alarm subsystem
		switch (pin) {
			case RTC_ALARM_WAKEUP:
				rtc.begin(false);
				rtc.attachInterrupt(callback);
			/*case UART_WAKEUP:*/
		}
		return;
	}

	EExt_Interrupts in = g_APinDescription[pin].ulExtInt;
	if (in == NOT_AN_INTERRUPT || in == EXTERNAL_INT_NMI)
    		return;

	// Query existing FILTEN bit for this specific external interrupt channel
	uint32_t config_idx = (in < 8) ? 0 : 1;
	uint32_t pos = (in < 8) ? (in << 2) : ((in - 8) << 2);
	bool current_filten = (EIC->CONFIG[config_idx].reg & (8 << pos)) != 0;

	// Validate EXTINT collision: warn if this channel is already owned by a different pin
	if (in < 16 && wakeupSources[in].active && wakeupSources[in].pin != pin) {
		#ifdef LOWPOWER_DEBUG_EIC
		Serial.print("WARNING: EXTINT"); Serial.print(in);
		Serial.print(" collision! Pin "); Serial.print(pin);
		Serial.print(" is overwriting pin "); Serial.print(wakeupSources[in].pin);
		Serial.println(" on the same EIC channel.");
		#endif
	}

	// Store in the software wake-source list
	if (in < 16) {
		wakeupSources[in].pin = pin;
		wakeupSources[in].extint = in;
		wakeupSources[in].mode = mode;
		wakeupSources[in].callback = callback;
		wakeupSources[in].filten = current_filten;
		wakeupSources[in].active = true;
	}

	//pinMode(pin, INPUT_PULLUP);
	attachInterrupt(pin, callback, mode);

	configGCLK6();

	// Enable wakeup capability on pin in case being used during sleep
	EIC->WAKEUP.reg |= (1 << in);
}

void ArduinoLowPowerClass::detachInterruptWakeup(uint32_t pin) {

	if (pin > PINS_COUNT) {
		// RTC alarm wakeup detach: disable the RTC alarm
		switch (pin) {
			case RTC_ALARM_WAKEUP:
				rtc.disableAlarm();
				break;
		}
		return;
	}

	EExt_Interrupts in = g_APinDescription[pin].ulExtInt;
	if (in == NOT_AN_INTERRUPT || in == EXTERNAL_INT_NMI)
		return;

	// Clear the software wake-source registration
	if (in < 16 && wakeupSources[in].active && wakeupSources[in].pin == pin) {
		wakeupSources[in].active = false;
		wakeupSources[in].callback = nullptr;

		// Remove wakeup capability for this EXTINT channel
		EIC->WAKEUP.reg &= ~(1 << in);

		// Detach the Arduino-level interrupt handler
		detachInterrupt(pin);

		#ifdef LOWPOWER_DEBUG_EIC
		Serial.print("Detached wakeup: Pin "); Serial.print(pin);
		Serial.print(" (EXTINT"); Serial.print(in); Serial.println(")");
		#endif
	}
}

void ArduinoLowPowerClass::attachAdcInterrupt(uint32_t pin, voidFuncPtr callback, adc_interrupt mode, uint16_t lo, uint16_t hi)
{
	uint8_t winmode = 0;

	switch (mode) {
		case ADC_INT_BETWEEN:   winmode = ADC_WINCTRL_WINMODE_MODE3; break;
		case ADC_INT_OUTSIDE:   winmode = ADC_WINCTRL_WINMODE_MODE4; break;
		case ADC_INT_ABOVE_MIN: winmode = ADC_WINCTRL_WINMODE_MODE1; break;
		case ADC_INT_BELOW_MAX: winmode = ADC_WINCTRL_WINMODE_MODE2; break;
		default: return;
	}

	adc_cb = callback;

	configGCLK6();

	// Configure ADC to use GCLK6 (OSCULP32K)
	while (GCLK->STATUS.bit.SYNCBUSY) {}
	GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_ADC
						| GCLK_CLKCTRL_GEN_GCLK6
						| GCLK_CLKCTRL_CLKEN;
	while (GCLK->STATUS.bit.SYNCBUSY) {}

	// Set ADC prescaler as low as possible
	ADC->CTRLB.bit.PRESCALER = ADC_CTRLB_PRESCALER_DIV4;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Configure window mode
	ADC->WINLT.reg = lo;
	ADC->WINUT.reg = hi;
	ADC->WINCTRL.reg = winmode;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Enable window interrupt
	ADC->INTENSET.bit.WINMON = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Enable ADC in standby mode
	ADC->CTRLA.bit.RUNSTDBY = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Enable continuous conversions
	ADC->CTRLB.bit.FREERUN = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Configure input mux
	ADC->INPUTCTRL.bit.MUXPOS = g_APinDescription[pin].ulADCChannelNumber;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Enable the ADC
	ADC->CTRLA.bit.ENABLE = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Start continuous conversions
	ADC->SWTRIG.bit.START = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Enable the ADC interrupt
	NVIC_EnableIRQ(ADC_IRQn);
}

void ArduinoLowPowerClass::detachAdcInterrupt()
{
	// Disable the ADC interrupt
	NVIC_DisableIRQ(ADC_IRQn);

	// Disable the ADC
	ADC->CTRLA.bit.ENABLE = 0;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Disable continuous conversions
	ADC->CTRLB.bit.FREERUN = 0;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Disable ADC in standby mode
	ADC->CTRLA.bit.RUNSTDBY = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Disable window interrupt
	ADC->INTENCLR.bit.WINMON = 1;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Disable window mode
	ADC->WINCTRL.reg = ADC_WINCTRL_WINMODE_DISABLE;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Restore ADC prescaler
	ADC->CTRLB.bit.PRESCALER = ADC_CTRLB_PRESCALER_DIV512_Val;
	while (ADC->STATUS.bit.SYNCBUSY) {}

	// Restore ADC clock
	while (GCLK->STATUS.bit.SYNCBUSY) {}
	GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_ADC
						| GCLK_CLKCTRL_GEN_GCLK0
						| GCLK_CLKCTRL_CLKEN;
	while (GCLK->STATUS.bit.SYNCBUSY) {}

	adc_cb = nullptr;
}

void ADC_Handler()
{
	// Clear the interrupt flag
	ADC->INTFLAG.bit.WINMON = 1;
	LowPower.adc_cb();
}

ArduinoLowPowerClass LowPower;

#endif // ARDUINO_ARCH_SAMD
