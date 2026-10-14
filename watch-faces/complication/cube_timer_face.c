/*
 * MIT License
 *
 * Copyright (c) 2026 <#author_name#>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// The emulator uses time() to seed the random number generator.
#if __EMSCRIPTEN__
#include <time.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cube_timer_face.h"
#include "watch.h"
#include "watch_common_display.h"
#include "watch_rtc.h"
#include "filesystem.h"

// Set this macro to 1 to check 1000 scrambles in setup and print the result.
#ifndef CUBE_TIMER_SELF_CHECK
#define CUBE_TIMER_SELF_CHECK 0
#endif

#if CUBE_TIMER_SELF_CHECK
#include <stdio.h>
#endif

#define CUBE_TIMER_FILE "cubetime.dat"

// The *_TICKS macros and the RTC counter fields use ticks of the 128 Hz RTC counter.
#define TICKS_PER_SECOND 128
#define INSPECTION_WARNING_1_TICKS (8 * TICKS_PER_SECOND)
#define INSPECTION_WARNING_2_TICKS (12 * TICKS_PER_SECOND)
#define INSPECTION_LIMIT_TICKS (15 * TICKS_PER_SECOND)
#define INSPECTION_DNF_TICKS (17 * TICKS_PER_SECOND)

// The result times use hundredths of a second.
#define PLUS_TWO_HUNDREDTHS 200
#define NO_TIME UINT32_MAX

// These values set the display refresh rate in Hz. The timekeeping always uses the 128 Hz counter.
static const uint8_t DISPLAY_INSPECTION_RATE = 4;
static const uint8_t DISPLAY_SOLVING_RATE = 16;

typedef enum {
    CUBE_TIMER_SCREEN_SCRAMBLE = 0,
    CUBE_TIMER_SCREEN_INSPECTION,
    CUBE_TIMER_SCREEN_SOLVING,
    CUBE_TIMER_SCREEN_RESULT,
    CUBE_TIMER_SCREEN_STATS,
} cube_timer_screen_t;

typedef enum {
    CUBE_TIMER_PENALTY_OK = 0,
    CUBE_TIMER_PENALTY_PLUS_TWO,
    CUBE_TIMER_PENALTY_DNF,
    CUBE_TIMER_PENALTY_COUNT,
} cube_timer_penalty_t;

typedef enum {
    CUBE_TIMER_STATS_LAST = 0,
    CUBE_TIMER_STATS_BEST,
    CUBE_TIMER_STATS_AO5,
    CUBE_TIMER_STATS_AO12,
    CUBE_TIMER_STATS_SOLVES,
    CUBE_TIMER_STATS_NUM_PAGES,
} cube_timer_stats_page_t;

// This table gives the letter of each cube face. The order is U, D, L, R, F, B, so
// face / 2 is the axis. The table uses lowercase b and d, because the uppercase
// letters look like 8 and 0 on the classic LCD.
static const char CUBE_FACES[6] = { 'U', 'd', 'L', 'R', 'F', 'b' };
// This table gives the character of each move modifier: none, prime and double.
static const char CUBE_MODIFIERS[3] = { ' ', '-', '2' };

static inline void _button_beep() {
    // Play a beep to confirm a button press, if the button sound is on.
    if (movement_button_should_sound()) watch_buzzer_play_note_with_volume(BUZZER_NOTE_C7, 50, movement_button_volume());
}

static uint32_t _random_below(uint32_t limit) {
// The emulator uses rand(). The watch uses arc4random_uniform().
#if __EMSCRIPTEN__
    return rand() % limit;
#else
    return arc4random_uniform(limit);
#endif
}

/// @brief Fills a buffer with a random 3x3 scramble.
/// @details Each move is face * 3 + modifier. The function does not use the
///          same face two times in a row. It does not put three moves in a
///          row on the same axis.
/// @param moves The buffer for the moves.
/// @param length The number of moves to generate.
static void _generate_scramble(uint8_t *moves, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        uint8_t face;
        bool valid;
        do {
            face = _random_below(6);
            valid = true;
            if (i >= 1 && face == moves[i - 1] / 3) valid = false;
            if (i >= 2 && face / 2 == (moves[i - 1] / 3) / 2 && face / 2 == (moves[i - 2] / 3) / 2) valid = false;
        } while (!valid);
        moves[i] = face * 3 + _random_below(3);
    }
}

#if CUBE_TIMER_SELF_CHECK
/// @brief Generates 1000 scrambles and checks the two scramble rules.
/// @return true if all scrambles obey the rules.
static bool _scramble_self_check(void) {
    uint8_t moves[CUBE_TIMER_SCRAMBLE_LENGTH];
    for (uint16_t n = 0; n < 1000; n++) {
        _generate_scramble(moves, CUBE_TIMER_SCRAMBLE_LENGTH);
        for (uint8_t i = 0; i < CUBE_TIMER_SCRAMBLE_LENGTH; i++) {
            uint8_t face = moves[i] / 3;
            if (face > 5) return false;
            if (i >= 1 && face == moves[i - 1] / 3) return false;
            if (i >= 2 && face / 2 == (moves[i - 1] / 3) / 2 && face / 2 == (moves[i - 2] / 3) / 2) return false;
        }
    }
    return true;
}
#endif

static uint32_t _ticks_to_hundredths(uint32_t ticks) {
    return (ticks >> 7) * 100 + (ticks & 0x7F) * 100 / 128;
}

/// @brief Returns the time of a result with the +2 penalty added.
/// @return The time in hundredths, or NO_TIME for a DNF.
static uint32_t _effective_time(cube_timer_result_t result) {
    switch (result.penalty) {
        case CUBE_TIMER_PENALTY_PLUS_TWO:
            return result.hundredths + PLUS_TWO_HUNDREDTHS;
        case CUBE_TIMER_PENALTY_DNF:
            return NO_TIME;
        default:
            return result.hundredths;
    }
}

/// @brief Returns a result of the session.
/// @param age The age of the result. The value 0 gives the most recent result, and the value 1 gives the result before it.
static cube_timer_result_t _session_result(cube_timer_state_t *state, uint8_t age) {
    return state->results[(state->session_count - 1 - age) % CUBE_TIMER_HISTORY_SIZE];
}

/// @brief Calculates the trimmed average of the most recent results.
/// @details The function adds the +2 penalties. Then it removes the best and
///          the worst result and takes the mean of the other results. A DNF
///          is the worst result. Two or more DNFs make the average a DNF.
///          The function truncates the mean to hundredths. The WCA rounds
///          averages to the nearest hundredth, so this value can be 0.01 lower.
/// @param count The number of results in the average, 5 or 12.
/// @param average The result of the calculation, or NO_TIME for a DNF.
/// @return false if the session has fewer than count results.
static bool _session_average(cube_timer_state_t *state, uint8_t count, uint32_t *average) {
    if (count < 3 || count > CUBE_TIMER_HISTORY_SIZE || state->session_count < count) return false;

    uint8_t dnf_count = 0;
    uint32_t best = NO_TIME;
    uint32_t worst = 0;
    uint32_t sum = 0;

    for (uint8_t i = 0; i < count; i++) {
        uint32_t time = _effective_time(_session_result(state, i));
        if (time == NO_TIME) {
            dnf_count++;
            worst = NO_TIME;
            continue;
        }
        sum += time;
        if (time < best) best = time;
        if (time > worst) worst = time;
    }

    if (dnf_count >= 2) {
        *average = NO_TIME;
        return true;
    }

    // Remove the best time from the sum. A DNF is the worst result, and the sum
    // does not contain it. If there is no DNF, remove the worst time from the sum.
    sum -= best;
    if (dnf_count == 0) sum -= worst;
    *average = sum / (count - 2);

    return true;
}

static void _save_records(cube_timer_state_t *state) {
    uint32_t data[2] = { state->personal_best, state->lifetime_count };
    filesystem_write_file(CUBE_TIMER_FILE, (char *)data, sizeof(data));
}

static void _load_records(cube_timer_state_t *state) {
    uint32_t data[2];

    state->personal_best = NO_TIME;
    state->lifetime_count = 0;

    if (!filesystem_file_exists(CUBE_TIMER_FILE)) return;
    if (filesystem_get_file_size(CUBE_TIMER_FILE) != sizeof(data)) return;
    if (!filesystem_read_file(CUBE_TIMER_FILE, (char *)data, sizeof(data))) return;

    state->personal_best = data[0];
    state->lifetime_count = data[1];
}

/// @brief Shows a number in the top right digits.
/// @details The classic LCD can show 0 to 39, and the custom LCD can show
///          0 to 99. The function clamps the value to the limit of the LCD.
static void _display_top_right_number(uint32_t value) {
    char buf[3];
    uint32_t limit = (watch_get_lcd_type() == WATCH_LCD_TYPE_CUSTOM) ? 99 : 39;

    if (value > limit) value = limit;
    buf[0] = value >= 10 ? '0' + value / 10 : ' ';
    buf[1] = '0' + value % 10;
    buf[2] = 0;
    watch_display_text(WATCH_POSITION_TOP_RIGHT, buf);
}

/// @brief Shows a time as MM:SS hh on the main line.
/// @param hundredths The time in hundredths, or NO_TIME for a DNF.
static void _display_time(uint32_t hundredths) {
    char buf[7];

    if (hundredths == NO_TIME) {
        // The classic LCD cannot show d or F in position 4 or 6.
        watch_clear_colon();
        watch_display_text(WATCH_POSITION_BOTTOM, " dnF  ");
        return;
    }

    uint32_t minutes = hundredths / 6000;
    uint32_t seconds = (hundredths / 100) % 60;
    uint32_t fraction = hundredths % 100;

    if (minutes > 99) {
        minutes = 99;
        seconds = 59;
        fraction = 99;
    }

    buf[0] = '0' + minutes / 10;
    buf[1] = '0' + minutes % 10;
    buf[2] = '0' + seconds / 10;
    buf[3] = '0' + seconds % 10;
    buf[4] = '0' + fraction / 10;
    buf[5] = '0' + fraction % 10;
    buf[6] = 0;

    watch_set_colon();
    watch_display_text(WATCH_POSITION_BOTTOM, buf);
}

static void _display_placeholder(void) {
    watch_clear_colon();
    watch_display_text(WATCH_POSITION_BOTTOM, "------");
}

static void _display_scramble(cube_timer_state_t *state) {
    uint8_t move = state->scramble[state->move_index];
    // The face letter shows in position 5 and the modifier in position 6.
    // The classic LCD cannot show b, d or F in position 4 or 6. The custom LCD
    // can show b and d in all positions of the main line.
    char buf[7] = { ' ', CUBE_FACES[move / 3], CUBE_MODIFIERS[move % 3], ' ', ' ', ' ', 0 };

    watch_clear_colon();
    _display_top_right_number(state->move_index + 1);
    watch_display_text(WATCH_POSITION_BOTTOM, buf);
}

static void _display_inspection(uint32_t elapsed) {
    watch_clear_colon();
    watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
    // Use a lowercase i, because the custom LCD cannot show an uppercase I in position 4.
    if (elapsed < INSPECTION_LIMIT_TICKS) {
        // Show the remaining whole seconds, rounded up.
        uint32_t remaining = (INSPECTION_LIMIT_TICKS - elapsed + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;
        char buf[7] = { 'i', 'n', 'S', 'P', '0' + remaining / 10, '0' + remaining % 10, 0 };
        watch_display_text(WATCH_POSITION_BOTTOM, buf);
    } else {
        watch_display_text(WATCH_POSITION_BOTTOM, "inSP+2");
    }
}

static void _display_penalty(uint8_t penalty) {
    if (penalty == CUBE_TIMER_PENALTY_PLUS_TWO) {
        watch_display_text_with_fallback(WATCH_POSITION_TOP_RIGHT, "+2", " 2");
    } else {
        watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
    }
}

static void _display_result(cube_timer_state_t *state) {
    cube_timer_result_t result = { state->solve_hundredths, state->penalty };

    _display_penalty(state->penalty);
    _display_time(_effective_time(result));
}

static void _display_stats(cube_timer_state_t *state) {
    uint32_t value;

    if (state->confirm_clear) {
        watch_clear_colon();
        watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
        watch_display_text(WATCH_POSITION_BOTTOM, "CLEAr ");
        return;
    }

    switch (state->stats_page) {
        case CUBE_TIMER_STATS_LAST:
            watch_display_text_with_fallback(WATCH_POSITION_TOP_RIGHT, "LA", " L");
            if (state->session_count == 0) {
                _display_placeholder();
            } else {
                _display_time(_effective_time(_session_result(state, 0)));
            }
            break;
        case CUBE_TIMER_STATS_BEST:
            watch_display_text_with_fallback(WATCH_POSITION_TOP_RIGHT, "bE", " b");
            if (state->personal_best == NO_TIME) {
                _display_placeholder();
            } else {
                _display_time(state->personal_best);
            }
            break;
        case CUBE_TIMER_STATS_AO5:
        case CUBE_TIMER_STATS_AO12:
            if (state->stats_page == CUBE_TIMER_STATS_AO5) {
                watch_display_text_with_fallback(WATCH_POSITION_TOP_RIGHT, "A5", " 5");
            } else {
                watch_display_text(WATCH_POSITION_TOP_RIGHT, "12");
            }
            if (_session_average(state, state->stats_page == CUBE_TIMER_STATS_AO5 ? 5 : 12, &value)) {
                _display_time(value);
            } else {
                _display_placeholder();
            }
            break;
        case CUBE_TIMER_STATS_SOLVES:
        default:
            {
                char buf[7];
                uint32_t count = state->session_count;
                watch_display_text_with_fallback(WATCH_POSITION_TOP_RIGHT, "Ct", " C");
                watch_clear_colon();
                if (count > 999999) count = 999999;
                for (int8_t i = 5; i >= 0; i--) {
                    buf[i] = (count || i == 5) ? '0' + count % 10 : ' ';
                    count /= 10;
                }
                buf[6] = 0;
                watch_display_text(WATCH_POSITION_BOTTOM, buf);
            }
            break;
    }
}

static void _display(cube_timer_state_t *state) {
    rtc_counter_t counter = watch_rtc_get_counter();

    switch (state->screen) {
        case CUBE_TIMER_SCREEN_SCRAMBLE:
            _display_scramble(state);
            break;
        case CUBE_TIMER_SCREEN_INSPECTION:
            _display_inspection(counter - state->inspection_start);
            break;
        case CUBE_TIMER_SCREEN_SOLVING:
            watch_display_text(WATCH_POSITION_TOP_RIGHT, "  ");
            _display_time(_ticks_to_hundredths(counter - state->solve_start));
            break;
        case CUBE_TIMER_SCREEN_RESULT:
            _display_result(state);
            break;
        case CUBE_TIMER_SCREEN_STATS:
        default:
            _display_stats(state);
            break;
    }
}

static bool _is_timing(cube_timer_state_t *state) {
    return state->screen == CUBE_TIMER_SCREEN_INSPECTION || state->screen == CUBE_TIMER_SCREEN_SOLVING;
}

static void _start_scramble(cube_timer_state_t *state) {
    _generate_scramble(state->scramble, CUBE_TIMER_SCRAMBLE_LENGTH);
    state->move_index = 0;
    state->screen = CUBE_TIMER_SCREEN_SCRAMBLE;
}

static void _start_inspection(cube_timer_state_t *state) {
    state->screen = CUBE_TIMER_SCREEN_INSPECTION;
    state->inspection_start = watch_rtc_get_counter();
    state->warnings_played = 0;
    movement_request_wake();
    movement_request_tick_frequency(DISPLAY_INSPECTION_RATE);
}

/// @brief Ends the inspection with a DNF result that has no time.
static void _inspection_dnf(cube_timer_state_t *state) {
    state->screen = CUBE_TIMER_SCREEN_RESULT;
    state->solve_hundredths = 0;
    state->penalty = CUBE_TIMER_PENALTY_DNF;
    state->has_time = false;
    movement_request_tick_frequency(1);
}

static void _start_solve(cube_timer_state_t *state, rtc_counter_t counter) {
    uint32_t elapsed = counter - state->inspection_start;

    if (elapsed >= INSPECTION_DNF_TICKS) {
        _inspection_dnf(state);
        return;
    }

    state->penalty = elapsed >= INSPECTION_LIMIT_TICKS ? CUBE_TIMER_PENALTY_PLUS_TWO : CUBE_TIMER_PENALTY_OK;
    state->screen = CUBE_TIMER_SCREEN_SOLVING;
    state->solve_start = counter;
    movement_request_tick_frequency(DISPLAY_SOLVING_RATE);
}

static void _stop_solve(cube_timer_state_t *state, rtc_counter_t counter) {
    state->solve_hundredths = _ticks_to_hundredths(counter - state->solve_start);
    state->has_time = true;
    state->screen = CUBE_TIMER_SCREEN_RESULT;
    movement_request_tick_frequency(1);
}

static void _record_result(cube_timer_state_t *state) {
    cube_timer_result_t result = { state->solve_hundredths, state->penalty };
    uint32_t time = _effective_time(result);

    state->results[state->session_count % CUBE_TIMER_HISTORY_SIZE] = result;
    state->session_count++;
    state->lifetime_count++;

    // Write the file only for a new personal best. Thus the lifetime count in
    // the file can be lower than the real count.
    if (time < state->personal_best) {
        state->personal_best = time;
        _save_records(state);
    }
}

/// @brief Checks the inspection time and plays the WCA warnings.
static void _inspection_tick(cube_timer_state_t *state, rtc_counter_t counter) {
    uint32_t elapsed = counter - state->inspection_start;

    if (elapsed >= INSPECTION_DNF_TICKS) {
        _inspection_dnf(state);
        movement_play_note(BUZZER_NOTE_C8, 500);
        return;
    }

    if (elapsed >= INSPECTION_WARNING_2_TICKS && !(state->warnings_played & 0b10)) {
        state->warnings_played |= 0b11;
        movement_play_note(BUZZER_NOTE_C8, 300);
    } else if (elapsed >= INSPECTION_WARNING_1_TICKS && !(state->warnings_played & 0b01)) {
        state->warnings_played |= 0b01;
        movement_play_note(BUZZER_NOTE_C7, 150);
    }
}

/// @brief Handles the events that all screens share.
static void _handle_common_event(cube_timer_state_t *state, movement_event_t event) {
    switch (event.event_type) {
        case EVENT_LIGHT_BUTTON_DOWN:
        case EVENT_LIGHT_BUTTON_UP:
        case EVENT_LIGHT_LONG_PRESS:
        case EVENT_LIGHT_LONG_UP:
            // The LIGHT button is a function key on this face. Do not turn on the LED.
            break;
        case EVENT_LOW_ENERGY_UPDATE:
            // The scramble, result and statistics screens are static. Keep the display as it is.
            break;
        case EVENT_TIMEOUT:
            // Do not resign during the inspection or the solve.
            if (!_is_timing(state)) movement_move_to_face(0);
            break;
        default:
            movement_default_loop_handler(event);
            break;
    }
}

static void _scramble_loop(cube_timer_state_t *state, movement_event_t event) {
    switch (event.event_type) {
        case EVENT_ALARM_BUTTON_UP:
            _button_beep();
            if (state->move_index + 1 >= CUBE_TIMER_SCRAMBLE_LENGTH) {
                _start_inspection(state);
            } else {
                state->move_index++;
            }
            _display(state);
            break;
        case EVENT_ALARM_LONG_PRESS:
            _button_beep();
            _start_inspection(state);
            _display(state);
            break;
        case EVENT_LIGHT_BUTTON_UP:
            _button_beep();
            if (state->move_index > 0) state->move_index--;
            _display(state);
            break;
        case EVENT_LIGHT_LONG_PRESS:
            _button_beep();
            state->screen = CUBE_TIMER_SCREEN_STATS;
            state->confirm_clear = false;
            _display(state);
            break;
        default:
            _handle_common_event(state, event);
            break;
    }
}

static void _inspection_loop(cube_timer_state_t *state, movement_event_t event) {
    rtc_counter_t counter = watch_rtc_get_counter();

    switch (event.event_type) {
        case EVENT_TICK:
            // Reset the inactivity timers, so that the watch does not enter low energy mode.
            if (event.subsecond == 0) movement_request_wake();
            _inspection_tick(state, counter);
            _display(state);
            break;
        case EVENT_ALARM_BUTTON_DOWN:
            _button_beep();
            _start_solve(state, counter);
            state->ignore_alarm_release = true;
            _display(state);
            break;
        default:
            _handle_common_event(state, event);
            break;
    }
}

static void _solving_loop(cube_timer_state_t *state, movement_event_t event) {
    rtc_counter_t counter = watch_rtc_get_counter();

    switch (event.event_type) {
        case EVENT_TICK:
            // Reset the inactivity timers, so that the watch does not enter low energy mode.
            if (event.subsecond == 0) movement_request_wake();
            _display(state);
            break;
        case EVENT_ALARM_BUTTON_DOWN:
            _stop_solve(state, counter);
            _button_beep();
            state->ignore_alarm_release = true;
            _display(state);
            break;
        case EVENT_ACTIVATE:
        case EVENT_TIMEOUT:
        case EVENT_LOW_ENERGY_UPDATE:
        case EVENT_MODE_LONG_PRESS:
            _handle_common_event(state, event);
            break;
        default:
            // Ignore all other buttons during the solve. This includes a short MODE press.
            break;
    }
}

static void _result_loop(cube_timer_state_t *state, movement_event_t event) {
    switch (event.event_type) {
        case EVENT_LIGHT_BUTTON_UP:
            _button_beep();
            // A result without a time can only be a DNF.
            if (state->has_time) state->penalty = (state->penalty + 1) % CUBE_TIMER_PENALTY_COUNT;
            _display(state);
            break;
        case EVENT_ALARM_BUTTON_UP:
            _button_beep();
            _record_result(state);
            _start_scramble(state);
            _display(state);
            break;
        default:
            _handle_common_event(state, event);
            break;
    }
}

static void _stats_loop(cube_timer_state_t *state, movement_event_t event) {
    switch (event.event_type) {
        case EVENT_LIGHT_BUTTON_UP:
            _button_beep();
            if (state->confirm_clear) {
                state->confirm_clear = false;
            } else {
                state->stats_page = (state->stats_page + 1) % CUBE_TIMER_STATS_NUM_PAGES;
            }
            _display(state);
            break;
        case EVENT_ALARM_BUTTON_UP:
            _button_beep();
            if (state->confirm_clear) {
                state->confirm_clear = false;
            } else {
                state->screen = CUBE_TIMER_SCREEN_SCRAMBLE;
            }
            _display(state);
            break;
        case EVENT_ALARM_LONG_PRESS:
            _button_beep();
            if (state->confirm_clear) {
                state->session_count = 0;
                state->confirm_clear = false;
            } else {
                state->confirm_clear = true;
            }
            _display(state);
            break;
        default:
            _handle_common_event(state, event);
            break;
    }
}

void cube_timer_face_setup(uint8_t watch_face_index, void ** context_ptr) {
    (void) watch_face_index;
    if (*context_ptr == NULL) {
        *context_ptr = malloc(sizeof(cube_timer_state_t));
        memset(*context_ptr, 0, sizeof(cube_timer_state_t));
        cube_timer_state_t *state = (cube_timer_state_t *)*context_ptr;
// The emulator seeds the random number generator with the current time.
#if __EMSCRIPTEN__
        srand(time(NULL));
#endif
#if CUBE_TIMER_SELF_CHECK
        printf("cube_timer_face: scramble self check %s\n", _scramble_self_check() ? "passed" : "FAILED");
#endif
        _load_records(state);
        _start_scramble(state);
    }
}

void cube_timer_face_activate(void *context) {
    cube_timer_state_t *state = (cube_timer_state_t *)context;
    state->confirm_clear = false;
    state->ignore_alarm_release = false;
    movement_request_tick_frequency(1);
}

bool cube_timer_face_loop(movement_event_t event, void *context) {
    cube_timer_state_t *state = (cube_timer_state_t *)context;

    // Ignore the release of the ALARM button that started or stopped the solve.
    if (state->ignore_alarm_release) {
        switch (event.event_type) {
            case EVENT_ALARM_BUTTON_UP:
            case EVENT_ALARM_LONG_UP:
                state->ignore_alarm_release = false;
                return !_is_timing(state);
            case EVENT_ALARM_LONG_PRESS:
            case EVENT_ALARM_REALLY_LONG_PRESS:
                return !_is_timing(state);
            default:
                break;
        }
    }

    if (event.event_type == EVENT_ACTIVATE) {
        watch_display_text_with_fallback(WATCH_POSITION_TOP_LEFT, "CUB", "CU");
        _display(state);
    }

    switch (state->screen) {
        case CUBE_TIMER_SCREEN_SCRAMBLE:
            _scramble_loop(state, event);
            break;
        case CUBE_TIMER_SCREEN_INSPECTION:
            _inspection_loop(state, event);
            break;
        case CUBE_TIMER_SCREEN_SOLVING:
            _solving_loop(state, event);
            break;
        case CUBE_TIMER_SCREEN_RESULT:
            _result_loop(state, event);
            break;
        case CUBE_TIMER_SCREEN_STATS:
        default:
            _stats_loop(state, event);
            break;
    }

    // Keep the system out of STANDBY during the inspection and the solve.
    return !_is_timing(state);
}

void cube_timer_face_resign(void *context) {
    cube_timer_state_t *state = (cube_timer_state_t *)context;

    // Cancel an inspection or a solve that is in progress. Show the same scramble again.
    if (_is_timing(state)) {
        state->screen = CUBE_TIMER_SCREEN_SCRAMBLE;
        state->move_index = 0;
    }
    state->confirm_clear = false;
    movement_request_tick_frequency(1);
}
