#ifndef ACPI_POWER_H
#define ACPI_POWER_H

#include "types.h"

void acpi_power_init(void);
bool acpi_power_is_available(void);
uint64_t acpi_power_get_battery_percent(void);
bool acpi_power_is_charging(void);
uint64_t acpi_power_get_estimated_minutes_remaining(void);
const char* acpi_power_get_source_label(void);
void acpi_power_force_refresh(void);
bool acpi_power_poweroff(void);

/* Phase 2: EC access */
bool acpi_power_ec_is_available(void);
bool acpi_power_ec_read8(uint8_t reg, uint8_t* out);
bool acpi_power_ec_write8(uint8_t reg, uint8_t value);

/* Phase 2: thermal / fan telemetry */
uint64_t acpi_power_get_thermal_temperature_celsius(void);
uint64_t acpi_power_get_fan_speed_rpm(void);

/* Phase 3: CPU C-state and sleep hooks */
uint64_t acpi_power_get_cpu_cstate_count(void);
bool acpi_power_suspend_to_state(uint8_t sleep_state);
bool acpi_power_resume_from_state(uint8_t sleep_state);

/* Phase 4: backend status */
bool acpi_power_has_full_acpica(void);
const char* acpi_power_get_backend_name(void);

#endif
