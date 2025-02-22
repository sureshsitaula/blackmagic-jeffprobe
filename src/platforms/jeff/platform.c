/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2018  Flirc Inc.
 * Written by Jason Kotzin <jasonkotzin@gmail.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "general.h"
#include "gdb_if.h"
#include "cdcacm.h"
#include "usbuart.h"
#include "gdb_packet.h"

#include <libopencm3/sam/d/nvic.h>
#include <libopencm3/sam/d/port.h>
#include <libopencm3/sam/d/gclk.h>
#include <libopencm3/sam/d/pm.h>
#include <libopencm3/sam/d/uart.h>
#include <libopencm3/sam/d/adc.h>

#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/scb.h>

#include <libopencm3/sam/d/tc.h>
#include <libopencm3/sam/d/eic.h>

static struct gclk_hw clock = {
	.gclk0 = SRC_DFLL48M,
	.gclk1 = SRC_OSC8M,
	.gclk1_div = 30,      /* divide clock for ADC  */
	.gclk2 = SRC_OSC8M,
	.gclk2_div = 100,     /* divide clock for TC */
	.gclk3 = SRC_DFLL48M,
	.gclk4 = SRC_DFLL48M,
	.gclk5 = SRC_DFLL48M,
	.gclk6 = SRC_DFLL48M,
	.gclk7 = SRC_DFLL48M,
};

extern void trace_tick(void);

uint8_t running_status;
static volatile uint32_t time_ms;

uint8_t button_pressed;

uint8_t tpwr_enabled;

void sys_tick_handler(void)
{
	if(running_status)
		gpio_toggle(LED_PORT, LED_IDLE_RUN);

	time_ms += 10;

	uart_pop();
}

uint32_t platform_time_ms(void)
{
	return time_ms;
}

static void usb_setup(void)
{
	/* Enable USB */
	INSERTBF(PM_APBBMASK_USB, 1, PM->apbbmask);

	/* enable clocking to usb */
	set_periph_clk(GCLK0, GCLK_ID_USB);
	periph_clk_en(GCLK_ID_USB, 1);

	gpio_config_special(PORTA, GPIO24, SOC_GPIO_PERIPH_G);
	gpio_config_special(PORTA, GPIO25, SOC_GPIO_PERIPH_G);

}

static uint32_t timing_init(void)
{
	uint32_t cal = 0;

	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_set_reload(4800);	/* Interrupt us at 10 Hz */
	systick_interrupt_enable();

	systick_counter_enable();
	return cal;
}

static void adc_init(void)
{
	gpio_config_special(ADC_PORT, ADC_POS_PIN, SOC_GPIO_PERIPH_B); /* +input */
	gpio_config_special(ADC_PORT, ADC_REF_PIN, SOC_GPIO_PERIPH_B); /* reference */

	set_periph_clk(GCLK1, GCLK_ID_ADC);
	periph_clk_en(GCLK_ID_ADC, 1);

	adc_enable(ADC_REFCTRL_VREFA,0,ADC_INPUTCTRL_GND,ADC_MUXPOS);
}

static void counter_init(void)
{
	/* enable bus and clock */
	INSERTBF(PM_APBCMASK_TC3, 1, PM->apbcmask);

	set_periph_clk(GCLK2, GCLK_ID_TC3);
	periph_clk_en(GCLK_ID_TC3, 1);

	/* reset */
	tc_reset(3);

	/* set CTRLA.PRESCALER and CTRLA.PRESYNC */
	tc_config_ctrla(3,1,(7<<8));

	/* set CC0 (approx. 5 seconds delay) */
	tc_set_cc(3,0,1000);

	/* enable MC0 interrupt */
	tc_enable_interrupt(3,(1<<4));
	nvic_enable_irq(NVIC_TC3_IRQ);
}

static void button_init(void)
{
	gpio_config_special(BUTTON_PORT, BUTTON_PIN, SOC_GPIO_PERIPH_A);

	/* enable bus and clock */
	INSERTBF(PM_APBAMASK_EIC, 1, PM->apbamask);

	set_periph_clk(GCLK0, GCLK_ID_EIC);
	periph_clk_en(GCLK_ID_EIC, 1);

	/* configure r/f edge, enable filtering */
	eic_set_config(15, 1, EIC_FALL);

	/* enable the IEC */
	eic_enable(1);

	/* enable interrupts */
	eic_enable_interrupt((1<<15));
	nvic_enable_irq(NVIC_EIC_IRQ);
}

void platform_init(void)
{
	gclk_init(&clock);

	usb_setup();

	gpio_config_output(LED_PORT, LED_IDLE_RUN, 0);
	gpio_config_output(TMS_PORT, TMS_PIN, 0);
	gpio_config_output(TCK_PORT, TCK_PIN, 0);
	gpio_config_output(TDI_PORT, TDI_PIN, 0);

	gpio_config_output(TMS_PORT, TMS_DIR_PIN, 0);
	gpio_set(TMS_PORT, TMS_DIR_PIN);

	/* enable both input and output with pullup disabled by default */
	PORT_DIRSET(SWDIO_PORT) = SWDIO_PIN;
	PORT_PINCFG(SWDIO_PORT, SWDIO_PIN_NUM) |= GPIO_PINCFG_INEN | GPIO_PINCFG_PULLEN;
	gpio_clear(SWDIO_PORT, SWDIO_PIN);

	/* configure swclk_pin as output */
	gpio_config_output(SWCLK_PORT, SWCLK_PIN, 0);
	gpio_clear(SWCLK_PORT, SWCLK_PIN);

	gpio_config_input(TDO_PORT, TDO_PIN, 0);
	gpio_config_output(SRST_PORT, SRST_PIN, GPIO_OUT_FLAG_DEFAULT_HIGH);
	gpio_clear(SRST_PORT, SRST_PIN);

	/* setup uart led, disable by default*/
	gpio_config_output(LED_PORT_UART, LED_UART, 0);//GPIO_OUT_FLAG_DEFAULT_HIGH);
	gpio_clear(LED_PORT_UART, LED_UART);

	/* set up TPWR */
	gpio_set(PWR_BR_PORT, PWR_BR_PIN);
	gpio_config_output(PWR_BR_PORT, PWR_BR_PIN, GPIO_OUT_FLAG_DEFAULT_HIGH);

	timing_init();
	usbuart_init();
	cdcacm_init();
	adc_init();
	counter_init();
	button_init();
}

uint8_t srst_state;
void platform_srst_set_val(bool assert)
{
	volatile int i;
	if (!assert) {
		gpio_clear(SRST_PORT, SRST_PIN);
		for(i = 0; i < 10000; i++) asm("nop");
		srst_state = 0;
	} else {
		gpio_set(SRST_PORT, SRST_PIN);
		srst_state = 1;
	}
}

bool platform_srst_get_val(void)
{
	//return gpio_get(SRST_PORT, SRST_PIN) != 0;
	return srst_state;
}

bool platform_target_get_power(void)
{
	//return !gpio_get(PWR_BR_PORT, PWR_BR_PIN);
	return tpwr_enabled;
}

void platform_target_set_power(bool power)
{
	gpio_set_val(PWR_BR_PORT, PWR_BR_PIN, !power);
	tpwr_enabled = power;
}

void platform_delay(uint32_t ms)
{
	platform_timeout timeout;
	platform_timeout_set(&timeout, ms);
	while (!platform_timeout_is_expired(&timeout));
}

const char *platform_target_voltage(void)
{
	uint32_t voltage;
	static char out[] = "0.0V";

	adc_start();

	while (!(1&(ADC->intflag)));
	voltage = ((485*adc_result())>>12); /* 330 without divider, 485 with it */

	out[0] = '0' + (char)(voltage/100);
	out[2] = '0' + (char)((voltage/10) % 10);

	return out;
}

char *serialno_read(char *s)
{
#ifdef CUSTOM_SER
	s[0] = 'J';
	s[1] = 'E';
	s[2] = 'F';
	s[3] = 'F';
	return s;
#else
        int i;
	volatile uint32_t unique_id = *(volatile uint32_t *)0x0080A00C +
		*(volatile uint32_t *)0x0080A040 +
		*(volatile uint32_t *)0x0080A044 +
		*(volatile uint32_t *)0x0080A048;

        /* Fetch serial number from chip's unique ID */
        for(i = 0; i < 8; i++) {
                s[7-i] = ((unique_id >> (4*i)) & 0xF) + '0';
        }

        for(i = 0; i < 8; i++)
                if(s[i] > '9')
                        s[i] += 'A' - '9' - 1;
	s[8] = 0;

	return s;
#endif
}

void print_serial(void)
{
	gdb_outf("0x%08X%08X%08X%08X\n", *(volatile uint32_t *)0x0080A048,
			*(volatile uint32_t *)0x0080A044,
			*(volatile uint32_t *)0x0080A040,
			*(volatile uint32_t *)0x0080A00C);
}

void platform_request_boot(void)
{
}

void eic_isr(void)
{
	if (!button_pressed){
		/* set to rising-edge detection */
		eic_set_config(15, 1, EIC_RISE);
		
		/* enable counter */
		tc_enable(3,1);

		button_pressed = 1;
	} else {
		/* set to falling-edge detection */
		eic_set_config(15, 1, EIC_FALL);

		/* disable and reset counter */
		tc_enable(3,0);

		button_pressed = 0;
	}

	/* clear the interrupt */
	eic_clr_interrupt((1<<15));
}

void tc3_isr(void)
{
	if (tc_interrupt_flag(3) & 16)
		scb_reset_system();
}
