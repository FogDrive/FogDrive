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
#include <avr/interrupt.h>
#include "ui.h"
#include "logic.h"
#include "deviface.h"
#include "led.h"
#include "button.h"
#include <avr/pgmspace.h>
#include MCUHEADER

// Bit mask for switch 0
#define CTRLMAP_SWITCH_0_MASK   (1<<HWMAP_UI_SWITCH_0_IX)
// Bit mask for all used switches
#define ALL_SWITCHES            (1<<HWMAP_UI_SWITCH_0_IX)

// Bit mask for out pin 0 (LED)
#define OUTPIN_0_MASK           (1<<HWMAP_UI_OUTPIN_0_IX)
// Bit mask for all used out pins
#define OUTPIN_ALL_MASK         (1<<HWMAP_UI_OUTPIN_0_IX)

// Low Level Event codes (for the low_level_event_queue)
#define LLE_SWITCH_PRESSED 1    // a switch was pressed, switch index in 2nd queue element
#define LLE_SWITCH_RELEASED 2   // a switch was released, switch index in 2nd queue element
#define LLE_50MS_PULSE 3        // 50 ms pulse, this event is put in the queue each 50 ms

// User interface input queue (which is an external, see @ui.h#Queue ui_input_queue) and its element array
Queue ui_event_queue;
QueueElement ui_event_queue_elements[5];

/**
 * Queue that transports low level control events to the ui state machines.
 * It's used locally in this module only.
 * Its elements carry a "Low Level Event" (LLE_*) in byte a. In case the event refers to a switch,
 * the switch index is in byte b.
 */
Queue low_level_event_queue;
/**
 * Queue array for #ui_input_queue.
 */
QueueElement low_level_event_queue_array[5];

LED led;

Button button;

uint8_t ui_init(void) {
    // init queues
    queue_initialize(&ui_event_queue, 5, ui_event_queue_elements);
    queue_initialize(&low_level_event_queue, 5, low_level_event_queue_array);

    // init switch pins
    HWMAP_UI_SWITCH_DDR &= ~ALL_SWITCHES;                // configure all input pins as input by setting the related direction bits to 0
    HWMAP_UI_SWITCH_PORT |= ALL_SWITCHES;                // turn on the pull up resistors of all input pins

    // init output pin
    HWMAP_UI_OUTPIN_DDR |= OUTPIN_ALL_MASK;             // configure output pins as output in the related data direction register
    HWMAP_UI_OUTPIN_PORT &= ~OUTPIN_ALL_MASK;           // initialize all output pins (set them off)

    // init the ui timer
    ui_timer_init_10ms_overflow();

    // init LED PWM
    mcu_init_ui_double_compare_timer_for_fast_pwm_1ms();
    led_init_led(&led, &MCU_UI_PWM_A_CR);

    // init button logic
    button_init(&button);

    return 0;
}

/**
  * ISR for timer zero.
  * Configured to be called every 10ms.
  */
ISR( HWMAP_UI_TIMER_ISR ) {
    // initialize the timer again to get the wanted trigger frequency for this ISR
    HWMAP_UI_TIMER_CMD_REINIT_FOR_10ms;

    // debouncing counter bytes
    static uint8_t ct0 = 0xFF, ct1 = 0xFF;
    // latest debounced switch state (each bit represents one switch (bit) from the input register (PIN))
    static uint8_t switch_states = 0;
    // latest changed status (1 = changed, 0 = unchanged, each bit represents one switch (bit) from the input register (PIN))
    static uint8_t changed_switches = 0;
    // counter that just counts to five times and is resetted then to identify each 5th cycle of the 10ms branch
    static uint8_t _50ms_counter = 0;
    // stores the difference of the main cycle counter between each call and the former of this ISR
    static uint8_t last_main_cycle_counter = 0;

    // here we go...
    // debouncing:
    // basic debouncing strategy based on http://www.mikrocontroller.net/articles/Entprellung#Timer-Verfahren_.28nach_Peter_Dannegger.29
    //
    // set those bits in i to 1 which represents a switch whose actual state is different from the latest unbounced state (key_state)
    uint8_t i = switch_states ^ ~HWMAP_UI_SWITCH_PIN;
    // ct0 and ct1 bytes: count the number of subsequent differences between actual and last debounced state or set back it back to 0 if it's equal.
    ct0 = ~( ct0 & i );
    ct1 = ct0 ^ (ct1 & i);
    // for each switch that had 4 times an actual state different from the last debounced state, set the corresponding bit in i to 1
    i &= ct0 & ct1;
    // set the most current _debounced_ state of all switches
    switch_states ^= i;
    // create a bitmask of all _relevant_ switches that have changed
    changed_switches = i & ALL_SWITCHES;

    // check for concrete switch changes:
    // check for switch 0
    if (CTRLMAP_SWITCH_0_MASK & changed_switches) {
        QueueElement* e = queue_get_write_element(&low_level_event_queue);
        if (CTRLMAP_SWITCH_0_MASK & switch_states) {
            e->bytes.a = LLE_SWITCH_PRESSED;
        } else {
            e->bytes.a = LLE_SWITCH_RELEASED;
        }
        e->bytes.b = HWMAP_UI_SWITCH_0_IX;
    }

    if (++_50ms_counter == 5) {
        _50ms_counter = 0;
        QueueElement* e = queue_get_write_element(&low_level_event_queue);
        e->bytes.a = LLE_50MS_PULSE;
    }
}

void ui_input_step(void) {
    QueueElement* low_level_event_e = queue_get_read_element(&low_level_event_queue);
    if (low_level_event_e != 0) {
        if (low_level_event_e->bytes.a == LLE_SWITCH_PRESSED) {
            button_pressed(&button);
        }
        else if (low_level_event_e->bytes.a == LLE_SWITCH_RELEASED) {
            button_released(&button);
        }
        else if (low_level_event_e->bytes.a == LLE_50MS_PULSE) {
            QueueElement* e = queue_get_write_element(&ui_event_queue);
            e->bytes.a = UI__50MS_PULSE;
            button_step(&button);
        }
    }
    QueueElement* button_event = queue_get_read_element(&(button.button_event_queue));
    if (button_event != 0) {
        if (button_event->bytes.a == BUTTON_EVENT_PRESSED) {
            QueueElement* e = queue_get_write_element(&ui_event_queue);
            e->bytes.a = UI__FIRE_BUTTON_PRESSED;
        }
        if (button_event->bytes.a == BUTTON_EVENT_RELEASED) {
            QueueElement* e = queue_get_write_element(&ui_event_queue);
            e->bytes.a = UI__FIRE_BUTTON_RELEASED;
        }
        if (button_event->bytes.a == BUTTON_EVENT_CLICK) {
            // nothing to do for a click sequence currently...
        }
    }
}

void ui_fire_is_on(void) {
    led_set_brightness(&led, 99);
}

void ui_fire_is_off(void) {
    led_set_brightness(&led, 0);
}
