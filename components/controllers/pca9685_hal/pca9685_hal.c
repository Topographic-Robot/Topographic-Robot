#include "pca9685_hal.h"
#include "common/i2c.h"
#include <esp_log.h>

/* Constants ******************************************************************/
const uint8_t  pca9685_scl_io           = GPIO_NUM_22; /**< GPIO pin for I2C Serial Clock Line */
const uint8_t  pca9685_sda_io           = GPIO_NUM_21; /**< GPIO pin for I2C Serial Data Line */
const uint32_t pca9685_i2c_freq_hz      = 100000;      /**< I2C Bus Frequency in Hz */
const uint8_t  pca9685_i2c_address      = 0x40;        /**< Base I2C address for PCA9685 */
const uint32_t pca9685_osc_freq         = 25000000;    /**< Internal Oscillator Frequency (25 MHz) */
const uint16_t pca9685_pwm_resolution   = 4096;        /**< 12-bit PWM resolution (4096 steps) */
const uint16_t pca9685_default_pwm_freq = 50;          /**< Default PWM frequency (50 Hz) */
const uint16_t pca9685_max_pwm_value    = 4095;        /**< Maximum value for PWM duty cycle (4095) */
const uint16_t pca9685_pwm_period_us    = 20000;       /**< Total PWM period for 50Hz (20000 µs) */
const char    *pca9685_tag              = "PCA9685";   /**< Tag for logs */

/* Private Functions **********************************************************/

/**
 * @brief Private helper function to calculate prescaler value based on desired PWM 
 *        frequency.
 *
 * @param[in] pwm_freq Desired PWM frequency in Hz.
 * @return The prescaler value.
 */
static uint8_t priv_calculate_prescaler(uint16_t pwm_freq) {
  return (uint8_t)((pca9685_osc_freq / (pca9685_pwm_resolution * pwm_freq)) - 1);
}

/* Public Functions ***********************************************************/

esp_err_t pca9685_init(pca9685_board_t **controller_data, uint8_t num_boards)
{
  esp_err_t ret;

  for (uint8_t i = 0; i < num_boards; i++) {
    pca9685_board_t *current = *controller_data;

    /* Check if the board is already initialized */
    while (current != NULL) {
      if (current->board_id == i) {
        ESP_LOGI(pca9685_tag, "PCA9685 board %d already initialized", i);
        break;
      }
      current = current->next;
    }

    /* If the board is already initialized, skip it */
    if (current != NULL) {
      continue;
    }

    /* Allocate memory for the new board */
    pca9685_board_t *new_board = (pca9685_board_t *)malloc(sizeof(pca9685_board_t));
    if (new_board == NULL) {
      ESP_LOGE(pca9685_tag, "Failed to allocate memory for PCA9685 board %d", i);
      return ESP_ERR_NO_MEM;
    }

    /* Initialize the I2C communication for the board */
    new_board->i2c_bus = pca9685_i2c_address + i;
    ret = priv_i2c_init(pca9685_scl_io, pca9685_sda_io, pca9685_i2c_freq_hz, 
                        new_board->i2c_bus, pca9685_tag);
    if (ret != ESP_OK) {
      ESP_LOGE(pca9685_tag, "Failed to initialize I2C for PCA9685 board %d", i);
      free(new_board);
      return ret;
    }

    /* Put the PCA9685 into sleep mode before setting the frequency */
    ret = priv_i2c_write_byte(k_pca9685_mode1_cmd | k_pca9685_sleep_cmd, 
                              new_board->i2c_bus, pca9685_tag);
    if (ret != ESP_OK) {
      ESP_LOGE(pca9685_tag, "Failed to put PCA9685 board %d into sleep mode", i);
      free(new_board);
      return ret;
    }

    /* Set the prescaler for the PWM frequency */
    uint8_t prescaler = priv_calculate_prescaler(pca9685_default_pwm_freq);
    ret = priv_i2c_write_byte(k_pca9685_prescale_cmd, new_board->i2c_bus, pca9685_tag);
    if (ret != ESP_OK) {
      ESP_LOGE(pca9685_tag, "Failed to write prescaler for PCA9685 board %d", i);
      free(new_board);
      return ret;
    }

    ret = priv_i2c_write_byte(prescaler, new_board->i2c_bus, pca9685_tag);
    if (ret != ESP_OK) {
      ESP_LOGE(pca9685_tag, "Failed to set prescaler value for PCA9685 board %d", i);
      free(new_board);
      return ret;
    }

    /* Wake up the PCA9685 (restart mode) */
    ret = priv_i2c_write_byte(k_pca9685_mode1_cmd | k_pca9685_restart_cmd, 
                              new_board->i2c_bus, pca9685_tag);
    if (ret != ESP_OK) {
      ESP_LOGE(pca9685_tag, "Failed to restart PCA9685 board %d", i);
      free(new_board);
      return ret;
    }

    /* Set board state and link it into the list */
    new_board->state      = k_pca9685_ready;
    new_board->board_id   = i;
    new_board->num_boards = num_boards;
    new_board->next       = *controller_data;
    *controller_data      = new_board;
  }

  return ESP_OK;
}

esp_err_t pca9685_set_angle(pca9685_board_t *controller_data, uint16_t motor_mask, 
                            uint8_t board_id, float angle)
{
  if (controller_data == NULL) {
    ESP_LOGE(pca9685_tag, "Controller data is NULL");
    return ESP_ERR_INVALID_ARG;
  }

  /* Check if board_id is within the valid range of boards */
  if (board_id >= controller_data->num_boards) {
    ESP_LOGE(pca9685_tag, "Invalid board_id: %d. Number of boards: %d", board_id, 
             controller_data->num_boards);
    return ESP_ERR_INVALID_ARG;
  }

  /* Find the correct board based on board_id */
  pca9685_board_t *current_board = controller_data;
  while (current_board != NULL) {
    if (current_board->board_id == board_id) {
      if (current_board->state != k_pca9685_ready) {
        ESP_LOGE(pca9685_tag, "PCA9685 board %d is not ready for communication", 
                 current_board->board_id);
        return ESP_FAIL;
      }

      /* Calculate pulse length for the given angle */
      uint16_t pulse_length = (uint16_t)((angle / 180.0f) * pca9685_max_pwm_value);

      /* Set the angle for each motor in the mask */
      for (uint8_t channel = 0; channel < 16; ++channel) {
        /* Check if this motor's bit is set in the mask */
        if (motor_mask & (1 << channel)) {  
          uint8_t on_l_cmd = k_pca9685_channel0_on_l_cmd + 4 * channel;

          /* Set ON time to 0 (start of pulse) */
          esp_err_t ret = priv_i2c_write_byte(on_l_cmd, current_board->i2c_bus, 
                                              pca9685_tag);
          if (ret != ESP_OK) {
            ESP_LOGE(pca9685_tag, "Failed to set ON time for motor %d on PCA9685 board %d", 
                     channel, current_board->board_id);
            return ret;
          }

          /* Set OFF time to pulse length (end of pulse) */
          ret = priv_i2c_write_byte(pulse_length & 0xFF, current_board->i2c_bus, 
                                    pca9685_tag); /* Lower byte of pulse length */
          if (ret != ESP_OK) {
            ESP_LOGE(pca9685_tag, "Failed to set OFF time (low byte) for motor %d on PCA9685 board %d", 
                     channel, current_board->board_id);
            return ret;
          }
          ret = priv_i2c_write_byte((pulse_length >> 8) & 0xFF, current_board->i2c_bus, 
                                    pca9685_tag); /* Upper byte of pulse length */
          if (ret != ESP_OK) {
            ESP_LOGE(pca9685_tag, "Failed to set OFF time (high byte) for motor %d on PCA9685 board %d", 
                     channel, current_board->board_id);
            return ret;
          }
        }
      }
      return ESP_OK;
    }
    current_board = current_board->next;
  }

  ESP_LOGE(pca9685_tag, "PCA9685 board with board_id %d not found", board_id);
  return ESP_ERR_NOT_FOUND;
}

