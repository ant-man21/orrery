#ifndef INC_STEPPER_H_
#define INC_STEPPER_H_

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#define NUM_MOTORS              8u
#define NUM_BUILT_PLANETS       4u        /* stations 0-3 (Mercury-Mars) exist in the gear train and have real
                                             RING_TEETH[]; 4-7 are pin-reserved with placeholder gear data */
#define BASE_STEPS_PER_REV      4096u     /* 28BYJ-48 output shaft, 8-step half-step mode, motor's own gearbox only */
#define MAX_STEP_INTERVAL_US    1000u     /* proven sustainable ceiling - ~1000 steps/sec */

#define SIM_HORIZON_STEPS       1000000L  /* how far ahead SIM mode aims before extending again -
                                             keeps it spinning perpetually instead of stopping */

#define REALTIME_TICK_INTERVAL_US 400000u /* phase 1: fixed 0.4 second per degree for every motor,
                                              until real per-planet rates arrive from Jetson */

typedef enum {
    MOTOR_STATE_ORBITING = 0,      /* normal operation - advancing per the active SystemMode */
    MOTOR_STATE_RETURNING_TO_ZERO, /* reset pressed once: heading home under power */
    MOTOR_STATE_MANUAL             /* homed and de-energized: safe to move by hand */
} MotorState;

typedef enum {
    MODE_SIM = 0,   /* constant rotation, per-planet speed scaled to real orbital ratios */
    MODE_REALTIME   /* tick-based ("watch tick"), driven by (eventually) real Jetson data */
} SystemMode;

typedef struct {
    int32_t  current_step;          /* actual physical position */
    int32_t  burst_target_step;     /* where the active mode says it should be */
    uint32_t steps_per_rev;         /* base * real ring:pinion gear ratio */
    double   fractional_steps_owed; /* sub-step accumulator for RT mode, avoids rounding drift */
    uint32_t next_tick_due_us;      /* RT mode only */
    uint32_t interval_us;           /* current step-to-step pace - set directly to target, no ramp */
    uint32_t next_due_us;           /* per-step timing while a burst is in progress */
} Motor;

extern Motor    motors[NUM_MOTORS];
extern uint8_t  reg_buffer[4];
extern volatile MotorState motor_state;
extern volatile SystemMode system_mode;

void     DWT_Init(void);
uint32_t get_micros(void);

void Motor_InitAll(void);
void Motor_SetModeSim(void);
void Motor_SetModeRealtime(void);
void Motor_ButtonReset(void);
void Motor_Poll(void);
void Motor_Shift(void);

#endif /* INC_STEPPER_H_ */
