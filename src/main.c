#include <8051.h>
#include "platform.h"
#include "config.h"
#include "hw.h"
#include "hal.h"
#include "ctl.h"

// Entry point.

void main(void)
{
    uint8_t a;
    uint8_t b;
    uint8_t code;
    uint8_t rx;
    uint8_t i;

    hw_init();
    ctl_init();

    // Give the key debouncer and pedal averager time to settle after power-up.
    for (i = 0; i < STARTUP_TICKS; i++)
    {
        while (!hw_tick_elapsed())
        {
        }

        if (hw_poll_pedals(&a, &b))
        {
            ctl_pedal_sample(0, a);
            ctl_pedal_sample(1, b);
        }
    }

    // Ignore anything pressed during start-up.
    hw_flush_events();

    for (;;)
    {
        if (hw_tick_elapsed())
        {
            ctl_tick();

            if (hw_poll_pedals(&a, &b))
            {
                ctl_pedal_sample(0, a);
                ctl_pedal_sample(1, b);
            }
        }

        // Handle any MIDI that arrived (remote stomp switch control).
        while (hal_midi_rx(&rx))
        {
            ctl_midi_in(rx);
        }

        code = hw_next_event();

        if (code != KEY_EV_NONE)
        {
            switch (KEY_EV_TYPE(code))
            {
            case KEY_EV_PRESS:
                ctl_key_press(KEY_EV_CODE(code));
                break;

            case KEY_EV_HOLD:
                ctl_key_hold(KEY_EV_CODE(code));
                break;

            case KEY_EV_RELEASE:
                ctl_key_release(KEY_EV_CODE(code));
                break;

            default:
                break;
            }
        }
    }
}
