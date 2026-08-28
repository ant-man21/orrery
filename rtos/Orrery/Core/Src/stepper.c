#include "stepper.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

Motor    motors[NUM_MOTORS];
uint8_t  reg_buffer[4] = {0};
volatile MotorState motor_state = MOTOR_STATE_ORBITING;
volatile SystemMode system_mode = MODE_SIM;

/* Half-step sequence, bit order per nibble: IN1 IN2 IN3 IN4 */
static const uint8_t HALF_STEP[8] = {
    0b1000, 0b1100, 0b0100, 0b0110,
    0b0010, 0b0011, 0b0001, 0b1001
};

/* Ring:pinion gear ratios from model/orrery_gear_train.scad's STATIONS[]. */
static const double GEAR_RATIO[NUM_MOTORS] = {
    44.0 / 10.0,   /* Mercury */
    83.0 / 10.0,   /* Venus */
    122.0 / 10.0,  /* Earth */
    161.0 / 10.0,  /* Mars */
    1.0, 1.0, 1.0, 1.0
};

/* Ring gear tooth counts from STATIONS[] (pinion is always 10 teeth on
   every station, so it cancels out of the ratio below and doesn't need
   to appear separately). Indices 4-7 aren't built yet; parked at
   Mercury's tooth count until those stations exist. */
static const uint32_t RING_TEETH[NUM_MOTORS] = {
    44, 83, 122, 161, 44, 44, 44, 44
};

/* Real sidereal orbital periods, in whole seconds. Neptune's period
   exceeds uint32_t range, hence uint64_t. */
static const uint64_t ORBITAL_PERIOD_SECONDS[NUM_MOTORS] = {
    7600543ULL,    /* Mercury: 87.969 days */
    19414166ULL,   /* Venus:   224.701 days */
    31558149ULL,   /* Earth:   365.256 days */
    59354208ULL,   /* Mars:    686.980 days */
    374355659ULL,  /* Jupiter: 11.86 years */
    929596608ULL,  /* Saturn:  29.46 years */
    2651486640ULL, /* Uranus:  84.01 years */
    5200317600ULL  /* Neptune: 164.8 years */
};

static uint32_t SIM_CRUISE_INTERVAL_US_CALC[NUM_MOTORS];

static void computeSimCruiseIntervals(void)
{
    for (uint32_t m = 0; m < NUM_MOTORS; m++) {
        if (m >= NUM_BUILT_PLANETS) {
            SIM_CRUISE_INTERVAL_US_CALC[m] = MAX_STEP_INTERVAL_US;
            continue;
        }

        uint64_t numerator   = (uint64_t)MAX_STEP_INTERVAL_US * RING_TEETH[0] * ORBITAL_PERIOD_SECONDS[m];
        uint64_t denominator = (uint64_t)RING_TEETH[m] * ORBITAL_PERIOD_SECONDS[0];

        /* round to nearest instead of truncating */
        SIM_CRUISE_INTERVAL_US_CALC[m] = (uint32_t)((numerator + denominator / 2) / denominator);
    }
}

/* Known-good fallback */
static const uint32_t SIM_CRUISE_INTERVAL_US_FIXED[NUM_MOTORS] = {
    1000, /* Mercury */
    1354, /* Venus */
    1498, /* Earth */
    2134, /* Mars */
    1000, 1000, 1000, 1000
};

static volatile bool sim_use_fixed_values = false; /* false = calculated (BTN_SIM), true = fixed (BTN_RESET) */

void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Free-running microsecond clock built from DWT->CYCCNT deltas.
 *
 * CYCCNT is a true 32-bit counter, but at 16 MHz it rolls over every ~268 s,
 * so CYCCNT/16 only ever spans 0..2^28-1 - not a full 32-bit range. Feeding
 * that straight into the (int32_t)(now - deadline) wrap trick breaks: a
 * deadline of now + interval computed just before the rollover lands above
 * the ceiling get_micros() can ever return, so "is it due?" answers no
 * forever and the motor latches up after a few minutes.
 *
 * Fix: accumulate elapsed cycles (delta is correct across CYCCNT's own 2^32
 * wrap) and convert to whole microseconds, carrying the sub-microsecond
 * remainder so there's no drift. s_micros is then a genuine free-running
 * uint32 that wraps at 2^32 (~71 min), which the wrap trick handles.
 *
 * Caller contract: must be polled at least every ~268 s (the 1 ms motion
 * poll and the 4 s IWDG both guarantee this), and single-threaded - only
 * Motor_Poll() on MotionTask calls it. Add a lock if that changes. */
uint32_t get_micros(void)
{
    static uint32_t s_last_cyc;   /* CYCCNT sampled on the previous call     */
    static uint32_t s_cyc_rem;    /* leftover cycles, always < cyc_per_us    */
    static uint32_t s_micros;     /* accumulated us, free-running, wraps 2^32 */

    uint32_t cyc_per_us = SystemCoreClock / 1000000U;
    uint32_t cyc        = DWT->CYCCNT;
    uint32_t delta      = cyc - s_last_cyc;   /* wraps correctly at 2^32 */
    s_last_cyc = cyc;

    s_cyc_rem += delta;
    s_micros  += s_cyc_rem / cyc_per_us;
    s_cyc_rem %= cyc_per_us;
    return s_micros;
}

/* Set each motor up to resume advancing under whichever mode is now
   active, from wherever it currently sits. Shared by boot init, mode
   switches, and resuming from a reset. */
static void primeTargetsForCurrentMode(void)
{
    for (uint32_t m = 0; m < NUM_MOTORS; m++) {
        if (system_mode == MODE_SIM) {
            motors[m].burst_target_step = motors[m].current_step + SIM_HORIZON_STEPS;
        } else {
            motors[m].burst_target_step     = motors[m].current_step;
            motors[m].fractional_steps_owed = 0.0;
            motors[m].next_tick_due_us      = 0; /* fire the first tick right away */
        }
    }
}

void Motor_InitAll(void)
{
    DWT_Init();
    computeSimCruiseIntervals();

    for (uint32_t m = 0; m < NUM_MOTORS; m++) {
        motors[m].current_step  = 0;
        motors[m].steps_per_rev = (uint32_t)(BASE_STEPS_PER_REV * GEAR_RATIO[m]);
        motors[m].interval_us   = MAX_STEP_INTERVAL_US;
        motors[m].next_due_us   = 0;
    }

    motor_state = MOTOR_STATE_ORBITING;
    system_mode = MODE_SIM; /* satisfying, obviously-moving demo by default */
    primeTargetsForCurrentMode();
}

void Motor_SetModeSim(void)
{
    system_mode = MODE_SIM;
    sim_use_fixed_values = false; /* BTN_SIM -> calculated table */
    if (motor_state == MOTOR_STATE_ORBITING) {
        primeTargetsForCurrentMode();
    }
}

void Motor_SetModeRealtime(void)
{
    system_mode = MODE_REALTIME;
    if (motor_state == MOTOR_STATE_ORBITING) {
        primeTargetsForCurrentMode();
    }
}

void Motor_Shift(void)
{
    HAL_SPI_Transmit(&hspi1, reg_buffer, 4, 10);
    HAL_GPIO_WritePin(MOTOR_LATCH_GPIO_Port, MOTOR_LATCH_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(MOTOR_LATCH_GPIO_Port, MOTOR_LATCH_Pin, GPIO_PIN_RESET);
}

/* Currently: selects the fixed SIM speed table (mirrors Motor_SetModeSim's
   role for the calculated table). The original 3-stage position-reset/
   freewheel behavior is parked - MOTOR_STATE_RETURNING_TO_ZERO/MANUAL are
   unused while this is BTN_RESET's job. */
void Motor_ButtonReset(void)
{
    system_mode = MODE_SIM;
    sim_use_fixed_values = true; /* BTN_RESET -> fixed table */
    if (motor_state == MOTOR_STATE_ORBITING) {
        primeTargetsForCurrentMode();
    }
}

void Motor_Poll(void)
{
    if (motor_state == MOTOR_STATE_MANUAL) {
        return; /* coils de-energized, nothing to drive */
    }

    uint32_t now = get_micros();
    bool changed   = false;
    bool allAtRest = true;

    for (uint32_t m = 0; m < NUM_MOTORS; m++) {
        Motor *mot = &motors[m];

        if (motor_state == MOTOR_STATE_ORBITING) {
            if (system_mode == MODE_SIM) {
                if (mot->current_step == mot->burst_target_step) {
                    /* caught up to the horizon - push it out further so SIM
                       mode spins perpetually instead of stopping */
                    mot->burst_target_step += SIM_HORIZON_STEPS;
                }
            } else { /* MODE_REALTIME */
                if ((int32_t)(now - mot->next_tick_due_us) >= 0) {
                    /* Phase 1: fixed 1 degree per tick, every motor -
                       real per-planet rates come from Jetson data later. */
                    mot->fractional_steps_owed += (double)mot->steps_per_rev / 360.0;
                    int32_t whole = (int32_t)mot->fractional_steps_owed;
                    mot->fractional_steps_owed -= whole;

                    mot->burst_target_step += whole;
                    mot->next_tick_due_us   = now + REALTIME_TICK_INTERVAL_US;
                }
            }
        }
        /* RETURNING_TO_ZERO/MANUAL are currently unreachable - BTN_RESET no
           longer drives that state machine, see Motor_ButtonReset() above */

        if (mot->current_step == mot->burst_target_step) {
            continue; /* idle - nothing to step */
        }

        allAtRest = false;

        if ((int32_t)(now - mot->next_due_us) >= 0) {
            /* No acceleration ramp - go straight to this mode's target pace. */
            if (system_mode == MODE_SIM) {
                mot->interval_us = sim_use_fixed_values ? SIM_CRUISE_INTERVAL_US_FIXED[m] : SIM_CRUISE_INTERVAL_US_CALC[m];
            } else {
                mot->interval_us = MAX_STEP_INTERVAL_US;
            }

            mot->current_step += (mot->burst_target_step > mot->current_step) ? 1 : -1;
            mot->next_due_us   = now + mot->interval_us;

            int32_t state = mot->current_step % 8;
            if (state < 0) state += 8;
            uint8_t nibble = HALF_STEP[state];

            /* SPI clocks reg_buffer[0] out first to alst chip. hence the reversal. */
            uint32_t byte_idx = (NUM_MOTORS / 2 - 1) - (m / 2);
            if (m % 2 == 0) {
                reg_buffer[byte_idx] = (reg_buffer[byte_idx] & 0xF0) | nibble;
            } else {
                reg_buffer[byte_idx] = (reg_buffer[byte_idx] & 0x0F) | (uint8_t)(nibble << 4);
            }
            changed = true;
        }
    }

    if (changed) {
        Motor_Shift();
    }

    if (motor_state == MOTOR_STATE_RETURNING_TO_ZERO && allAtRest) {
        memset(reg_buffer, 0, sizeof(reg_buffer));
        Motor_Shift();
        motor_state = MOTOR_STATE_MANUAL;
    }
}
