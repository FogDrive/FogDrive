/*
    FogDrive (https://github.com/FogDrive/FogDrive)
    Copyright (C) 2016  Daniel Llin Ferrero

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <avr/io.h>
#include <util/delay.h>
#include "logic.h"
#include "hardware.h"
#include "ui.h"
#include MCUHEADER
#ifdef UART_ENABLED
    #include "deviface.h"
#endif


/*
 * Global variables
 */
static uint8_t local_bools = 0;
#define LB_HW_IS_FIRING   1     // if true, hardware is currently firing
#define LB_PRINT_BVMS     2     // if true, the battery voltage is printed each time its received (set via deviface)

uint8_t battery_voltage_under_load = 0; //external
uint8_t global_state = GS_ON;           //external

void logic_loop (void) {
    static uint16_t logic_main_cycle_counter = 0;           // contineously counts the cycles of the main loop and just overflows at its value range end back to 0
    #ifdef UART_ENABLED
    static uint16_t last_logic_cycle_value = 0;             // stores the logic cycle count at each 50 ms event and is used to calc the delta
    static uint16_t last_logic_cycles_per_50ms_event = 0;   // stores the number of cycles that happened between the two last 50ms events
    static uint16_t min_logic_cycles_per_50ms_event = 0;    // stores the minimum value of the above variable throughout the whole uptime
    #endif
    static uint8_t pulse_counter_for_battery_voltage_measurement = 0;   // counts 50ms events to trigger battery voltage measurements in regular intervals


    #ifdef UART_ENABLED
    deviface_putline("FogDrive  Copyright (C) 2016, the FogDrive Project");
    deviface_putline("This program is free software and comes with ABSOLUTELY NO WARRANTY.");
    deviface_putline("It is licensed under the GPLv3 (see <http://www.gnu.org/licenses/#GPL>).");
    deviface_putline("\r\nHi! This is the Mira FogDrive.\r\n");
    #endif

    // lets enter the main loop

    while(1) {
        ui_input_step();    // the user interface gets its cycle
        hardware_step();    // the hardware gets its cycle

        if (global_state == GS_AWAKENING) {
            // If the device is "waking up"...
            // (Means, an interrupt has stopped the power down mode but we have not received a switch-on command from the UI yet.)
            QueueElement* e = queue_get_read_element(&ui_event_queue);
            // We just check for the very next UI event that is coming...
            if (e != 0) {
                if (e->bytes.a == UI__SWITCH_ON) {
                    // user asked to switch on the device again... we wake up
                    hardware_power_up();
                    ui_power_up();
                    global_state = GS_ON;
                    #ifdef UART_ENABLED
                    deviface_putline("DEVICE UP");
                    #endif
                } else if(e->bytes.a == UI__ABORT_AWAKENING) {
                    // no switch-on action by the user, the device goes to sleep again
                    #ifdef UART_ENABLED
                    deviface_putline("DOWN AGAIN");
                    #endif
                    mcu_power_down_till_pin_change(); // go to sleep
                }
            }
        }
        else
        {
            // The device on (not awakening)
            // process UI event
            QueueElement* e = queue_get_read_element(&ui_event_queue);
            if (e != 0) {
                if (e->bytes.a == UI__FIRE_BUTTON_PRESSED) {
                    hardware_fire_on();
                }
                if (e->bytes.a == UI__FIRE_BUTTON_RELEASED) {
                    hardware_fire_off();
                }
                if (e->bytes.a == UI__SWITCH_OFF) {
                    #ifdef UART_ENABLED
                    deviface_putline("DOWN");
                    #endif
                    hardware_fire_off();
                    hardware_power_down();
                    ui_power_down();
                    mcu_power_down_till_pin_change(); // go to sleep
                    global_state = GS_AWAKENING;
                    continue;
                }
                if (e->bytes.a == UI__50MS_PULSE) {
                    // 1) do the main cycle time measurement (for development purposes only)
                    #ifdef UART_ENABLED
                    if (last_logic_cycle_value < logic_main_cycle_counter) {
                        // no overflow
                        last_logic_cycles_per_50ms_event = logic_main_cycle_counter - last_logic_cycle_value;
                        if (min_logic_cycles_per_50ms_event > last_logic_cycles_per_50ms_event) {
                            min_logic_cycles_per_50ms_event = last_logic_cycles_per_50ms_event;
                        }
                    }
                    last_logic_cycle_value = logic_main_cycle_counter;
                    #endif

                    // 2) trigger cyclical battery voltage measurement
                    if (local_bools & LB_HW_IS_FIRING) {
                        if (pulse_counter_for_battery_voltage_measurement > 3) {   // every 200ms (4*50ms) when the mod is firing
                            do_battery_measurement();
                            pulse_counter_for_battery_voltage_measurement = 0;
                        }
                    }
//                    else {
//                        if (pulse_counter_for_battery_voltage_measurement == 40) { // every ~12s (256*50ms) when the mod is _not_ firing
//                            do_battery_measurement();   // we use "40" as compare value to give the battery 2s (40*50ms) to relax after stopping firing
//                            // we do not reset the (8 bit) counter but just let it overflow (to get the 256*50ms rhythm)
//                        }
//                    }
                    ++pulse_counter_for_battery_voltage_measurement;
                }
            }
            //process HW event
            e = queue_get_read_element(&hw_event_queue);
            if (e != 0) {
                if (e->bytes.a == HW__FIRE_ON) {
                    local_bools |= LB_HW_IS_FIRING;
                    pulse_counter_for_battery_voltage_measurement = 0;
                    ui_fire_is_on();
                }
                else if (e->bytes.a == HW__FIRE_OFF) {
                    local_bools &= ~LB_HW_IS_FIRING;
                    pulse_counter_for_battery_voltage_measurement = 0;
                    ui_fire_is_off();
                }
                else if (e->bytes.a == HW__BATTERY_MEASURE) {
                    if (local_bools & LB_HW_IS_FIRING) {
                        battery_voltage_under_load = e->bytes.b;
                        // check if the battery voltage has dropped so low that we have to block firing
                        if (battery_voltage_under_load <= BATTERY_VOLTAGE_STOP_VALUE) {
                            ui_switch_off_forced();
                            hardware_fire_off();
                        }
                    }
                    #ifdef UART_ENABLED
                    if (local_bools & LB_PRINT_BVMS) {
                        deviface_putstring("BVM: ");
                        deviface_put_uint8(e->bytes.b);
                        deviface_putlineend();
                    }
                    #endif
                }
            }
    #ifdef UART_ENABLED
            //process commands from the devolper interface (deviface) (UART)
            if (uart_str_complete) {
                char in_string[UART_MAXSTRLEN + 1];
                strcpy (in_string, uart_string);
                uart_str_complete = 0;
                if (strcmp(in_string, "off") == 0) {
                    hardware_fire_off();
                }
                if (strcmp(in_string, "on") == 0) {
                    hardware_fire_on();
                }
                if (strcmp(in_string, "bvm") == 0) {
                    do_battery_measurement();
                }
                if (strcmp(in_string, "cyc l50") == 0) {
                    deviface_putstring("Last cycle number per 50ms event: ");
                    deviface_put_uint16(last_logic_cycles_per_50ms_event);
                    deviface_putlineend();
                }
                if (strcmp(in_string, "cyc m50") == 0) {
                    deviface_putstring("Minimum cycles number per 50ms event: ");
                    deviface_put_uint16(min_logic_cycles_per_50ms_event);
                    deviface_putlineend();
                }
                if (strcmp(in_string, "cyc count") == 0) {
                    deviface_putstring("Main cycle counter: ");
                    deviface_put_uint16(logic_main_cycle_counter);
                    deviface_putlineend();
                }
                if (strcmp(in_string, "ui leds") == 0) {
                    ui_print_led_info();
                }
                if (strcmp(in_string, "bv") == 0) {
                    deviface_putstring("Battery voltage under load: ");
                    deviface_put_uint8(battery_voltage_under_load);
                    deviface_putstring("\n\r");
                }
                if (strcmp(in_string, "p bvm on") == 0) {
                    local_bools |= LB_PRINT_BVMS;
                }
                if (strcmp(in_string, "p bvm off") == 0) {
                    local_bools &= ~LB_PRINT_BVMS;
                }
            }
    #endif
            logic_main_cycle_counter++;
        }
    }
    return 0;
}
