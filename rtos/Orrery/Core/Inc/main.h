/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define EARTH_IN4_Pin GPIO_PIN_1
#define EARTH_IN4_GPIO_Port GPIOA
#define EARTH_IN1_Pin GPIO_PIN_0
#define EARTH_IN1_GPIO_Port GPIOB
#define EARTH_IN2_Pin GPIO_PIN_1
#define EARTH_IN2_GPIO_Port GPIOB
#define EARTH_IN3_Pin GPIO_PIN_10
#define EARTH_IN3_GPIO_Port GPIOB
#define MERCURY_IN1_Pin GPIO_PIN_12
#define MERCURY_IN1_GPIO_Port GPIOB
#define MERCURY_IN2_Pin GPIO_PIN_13
#define MERCURY_IN2_GPIO_Port GPIOB
#define MERCURY_IN3_Pin GPIO_PIN_14
#define MERCURY_IN3_GPIO_Port GPIOB
#define MERCURY_IN4_Pin GPIO_PIN_15
#define MERCURY_IN4_GPIO_Port GPIOB
#define VENUS_IN1_Pin GPIO_PIN_8
#define VENUS_IN1_GPIO_Port GPIOA
#define VENUS_IN2_Pin GPIO_PIN_9
#define VENUS_IN2_GPIO_Port GPIOA
#define VENUS_IN3_Pin GPIO_PIN_10
#define VENUS_IN3_GPIO_Port GPIOA
#define VENUS_IN4_Pin GPIO_PIN_15
#define VENUS_IN4_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
