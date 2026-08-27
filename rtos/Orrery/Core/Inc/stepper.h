#ifndef INC_STEPPER_H_
#define INC_STEPPER_H_

#include "main.h"

/* 28BYJ-48, 8-step half-step drive, output shaft (after internal gearbox) */
#define STEPPER_STEPS_PER_REV   4096u

typedef struct {
    GPIO_TypeDef *port[4];
    uint16_t      pin[4];
    uint8_t       stepIndex;
} Stepper_t;

void Stepper_Step(Stepper_t *motor, int8_t direction);
void Stepper_RotateDegrees(Stepper_t *motor, float degrees, uint32_t stepDelayTicks);
void Stepper_Release(Stepper_t *motor);

#endif /* INC_STEPPER_H_ */
