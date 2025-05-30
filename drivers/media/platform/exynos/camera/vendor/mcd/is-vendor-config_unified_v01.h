#ifndef IS_VENDOR_CONFIG_COMMON_BASE_H
#define IS_VENDOR_CONFIG_COMMON_BASE_H

/*
 * This is a common base vendor configuration file.
 */

// Defines identically present in all configurations
#define VENDER_PATH
#define USE_CAMERA_SENSOR_RETENTION
#define CAMERA_REAR_DUAL_CAL
#define CAMERA_REAR2
#define CAMERA_REAR2_AF /* related to OIS */
#define CAMERA_REAR2_TILT
#define CAMERA_REAR2_MODULEID
#define CAMERA_REAR3
#define CAMERA_REAR3_AFCAL
#define CAMERA_REAR3_TILT
#define CAMERA_REAR3_MODULEID
#define CAMERA_REAR_PAFCAL

#define IS_MAX_FW_BUFFER_SIZE (4100 * 1024)
#define IS_MAX_CAL_SIZE (64 * 1024)

#define CAMERA_OIS_GYRO_OFFSET_SPEC 10000

#define CAMERA_REAR2_SENSOR_SHIFT_CROP
#define CAMERA_2ND_OIS

#define USE_CAMERA_ADAPTIVE_MIPI
#ifdef USE_CAMERA_ADAPTIVE_MIPI
//#define USE_CAMERA_ADAPTIVE_MIPI_RUNTIME
#endif

//#define USE_CAMERA_CHECK_SENSOR_REV

#define USE_CAMERA_HW_BIG_DATA
#ifdef USE_CAMERA_HW_BIG_DATA
//#define CAMERA_HW_BIG_DATA_FILE_IO
//#define CSI_SCENARIO_COMP		(0) This value follows dtsi
#define CSI_SCENARIO_SEN_FRONT	(1)
#define CSI_SCENARIO_TELE		(2)
#define CSI_SCENARIO_SECURE		(3)
#define CSI_SCENARIO_SEN_REAR	(0)
#endif

#define USE_AF_SLEEP_MODE
#define USE_NEW_PER_FRAME_CONTROL

//#define ROM_SUPPORT_APERTURE_F2	// Second step of aperture.

//#define OIS_CENTERING_SHIFT_ENABLE

#undef ENABLE_DYNAMIC_MEM

#define USE_SENSOR_LONG_EXPOSURE_SHOT
#define OIS_DUAL_CAL_DEFAULT_VALUE_WIDE 0
#define WIDE_OIS_ROM_ID ROM_ID_REAR

#define USE_OIS_HALL_DATA_FOR_VDIS

#define CONFIG_SECURE_CAMERA_USE 1

#define USE_CAMERA_IOVM_BEST_FIT

#define PDP_FAST_VVALID_THRESHOLD_TIME 1000000 /* 1s */

// Device-specific
#include "is-vendor-fragment_rsu.h"
#include "is-vendor-fragment_usu.h"

// Differing configurations
#include "is-vendor-fragment_rsu_usu_diff.h"

#endif /* IS_VENDOR_CONFIG_COMMON_BASE_H */