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

#ifndef CUBE_TIMER_FACE_H_
#define CUBE_TIMER_FACE_H_

/*
 * CUBE TIMER face
 *
 * This face is a timer for 3x3 speedcubing. It uses the WCA times for the
 * inspection and the inspection penalties.
 *
 * The scrambles are random moves, not WCA random-state scrambles. A scramble
 * has 20 moves. It does not use the same face two times in a row. It does not
 * put three moves in a row on the same axis.
 *
 * The face has five screens:
 *
 *  o Scramble: The face shows a new scramble one move at a time. The top
 *    right digits show the move number. A prime move shows with a trailing
 *    dash ("R-"). A double move shows with a trailing 2 ("R2"). The faces B
 *    and D show as lowercase "b" and "d".
 *    Press ALARM to show the next move and LIGHT to show the previous move.
 *    Press ALARM after the last move, or hold ALARM, to start the inspection.
 *    Hold LIGHT to open the statistics.
 *
 *  o Inspection: The face counts down 15 seconds. The watch beeps at 8 and
 *    12 seconds. Press ALARM to start the solve. If you start the solve
 *    after 15 seconds, the result gets a +2 penalty. After 17 seconds, the
 *    result is a DNF.
 *
 *  o Solving: The face shows the time as MM:SS hh. Press ALARM to stop the
 *    solve. The LIGHT and MODE buttons do nothing. Hold MODE to leave the
 *    face. This cancels the solve.
 *
 *  o Result: The face shows the time of the solve. Press LIGHT to change
 *    the penalty from OK to +2 to DNF. Press ALARM to save the result and
 *    show a new scramble.
 *
 *  o Statistics: Press LIGHT to show the last result ("LA"), the personal
 *    best ("bE"), the average of 5 ("A5"), the average of 12 ("12") and the
 *    number of results in this session ("Ct"). The classic LCD shows " L",
 *    " b", " 5", "12" and " C". Dashes show when the session has too few
 *    results. Hold ALARM two times to clear the session. Press ALARM to go
 *    back to the scramble.
 *
 * An average removes the best and the worst result and takes the mean of
 * the other results. The face truncates the average to hundredths. The WCA
 * rounds averages, so an average on this face can be 0.01 seconds lower.
 *
 * The face keeps the last 12 results of the session in RAM. It saves the
 * personal best and the lifetime number of results in the file
 * "cubetime.dat". The face writes this file only when you set a new
 * personal best.
 */

#include "movement.h"

#define CUBE_TIMER_SCRAMBLE_LENGTH 20
#define CUBE_TIMER_HISTORY_SIZE 12

typedef struct {
    uint32_t hundredths;            // This field is the solve time in hundredths, without the penalty.
    uint8_t penalty;                // This field holds a cube_timer_penalty_t value.
} cube_timer_result_t;

typedef struct {
    uint8_t screen;                 // This field holds the cube_timer_screen_t value of the screen that shows now.
    uint8_t scramble[CUBE_TIMER_SCRAMBLE_LENGTH]; // Each move is face * 3 + modifier.
    uint8_t move_index;             // This field is the index of the scramble move that shows now.
    uint8_t warnings_played;        // Each bit is set when the related inspection warning has played.
    uint8_t penalty;                // This field is the penalty of the current result.
    uint8_t stats_page;             // This field is the statistics page that shows now.
    bool has_time;                  // This field is false if the result became a DNF before the solve started.
    bool confirm_clear;             // This field is true while the face asks to clear the session.
    bool ignore_alarm_release;      // This field is true until the user releases the ALARM button that started or stopped the solve.
    rtc_counter_t inspection_start; // This field is the RTC counter value at the start of the inspection.
    rtc_counter_t solve_start;      // This field is the RTC counter value at the start of the solve.
    uint32_t solve_hundredths;      // This field is the solve time of the current result in hundredths.
    cube_timer_result_t results[CUBE_TIMER_HISTORY_SIZE]; // This ring buffer holds the results of the session.
    uint32_t session_count;         // This field is the number of results in this session.
    uint32_t personal_best;         // This field is the best time in hundredths. It is UINT32_MAX if there is no personal best.
    uint32_t lifetime_count;        // This field is the number of results in all sessions.
} cube_timer_state_t;

void cube_timer_face_setup(uint8_t watch_face_index, void ** context_ptr);
void cube_timer_face_activate(void *context);
bool cube_timer_face_loop(movement_event_t event, void *context);
void cube_timer_face_resign(void *context);

#define cube_timer_face ((const watch_face_t){ \
    cube_timer_face_setup, \
    cube_timer_face_activate, \
    cube_timer_face_loop, \
    cube_timer_face_resign, \
    NULL, \
})

#endif // CUBE_TIMER_FACE_H_
