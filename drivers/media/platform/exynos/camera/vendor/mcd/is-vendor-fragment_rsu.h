#ifndef IS_VENDOR_FRAGMENT_RSU_H
#define IS_VENDOR_FRAGMENT_RSU_H

/* --- RSU (e.g., S21 FE) Specific Features/Values --- */

// Sensor/ISP related
#define USE_CAMERA_2LD_4000X3000
#define USE_HI1336C_SETFILE
#define CAMERA_STANDARD_CAL_ISP_VERSION 'E'

// Buffer sizes
//#define IS_MAX_TNRISP_SIZE (0x0672E180) // Might need rechecking.
#define IS_FRONT_MAX_CAL_SIZE (64 * 1024) // Might need rechecking.
#define IS_REAR2_MAX_CAL_SIZE (64 * 1024) // Might need rechecking.

// Calibration & Sensor Count
#define CONFIG_SEC_CAL_ENABLE   // Keep enabled, seems to not need runtime checks.
#ifdef CONFIG_SEC_CAL_ENABLE
#define USES_STANDARD_CAL_RELOAD
#endif
#define IS_VENDOR_SENSOR_COUNT 4        /* FRONT_0, REAR_0, REAR_1, REAR_2 */

// LED Driver
// #define CONFIG_LEDS_KTD2692             // Handled on kconfig

// Dualized Camera
#define USE_CAMERA_DUALIZED
#ifdef USE_CAMERA_DUALIZED
#define CAMERA_UWIDE_DUALIZED SENSOR_NAME_HI1336
#endif

#endif /* IS_VENDOR_FRAGMENT_RSU_H */