#include "stepper.h"
#include "cmsis_os.h"

/* 8-step half-step sequence: IN1, IN2, IN3, IN4 */
static const uint8_t halfStepSeq[8][4] = {
    {1, 0, 0, 0},
    {1, 1, 0, 0},
    {0, 1, 0, 0},
    {0, 1, 1, 0},
    {0, 0, 1, 0},
    {0, 0, 1, 1},
    {0, 0, 0, 1},
    {1, 0, 0, 1},
};

void Stepper_Step(Stepper_t *motor, int8_t direction)
{
    motor->stepIndex = (motor->stepIndex + direction + 8) % 8;
    for (int i = 0; i < 4; i++) {
        HAL_GPIO_WritePin(motor->port[i], motor->pin[i],
                           halfStepSeq[motor->stepIndex][i] ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

void Stepper_RotateDegrees(Stepper_t *motor, float degrees, uint32_t stepDelayTicks)
{
    int8_t direction = (degrees >= 0.0f) ? 1 : -1;
    int32_t steps = (int32_t)((degrees < 0.0f ? -degrees : degrees) / 360.0f * STEPPER_STEPS_PER_REV);

    for (int32_t i = 0; i < steps; i++) {
        Stepper_Step(motor, direction);
        osDelay(stepDelayTicks);
    }
}

void Stepper_Release(Stepper_t *motor)
{
    for (int i = 0; i < 4; i++) {
        HAL_GPIO_WritePin(motor->port[i], motor->pin[i], GPIO_PIN_RESET);
    }
}
