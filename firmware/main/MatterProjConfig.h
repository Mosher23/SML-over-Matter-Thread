#pragma once

// Matter Basic Information identity for the SML smart-meter reader with an infrared interface.
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "SimpleIdeas"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "SML over Matter/Thread"

// Publish a useful name in commissionable-node discovery instead of letting
// controllers fall back to the generic "Matter Accessory" label.
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME 1
#define CHIP_DEVICE_CONFIG_DEVICE_NAME "SML over Matter/Thread"

// Human-readable firmware and hardware revisions.
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 25
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "1.21"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "1.0"
