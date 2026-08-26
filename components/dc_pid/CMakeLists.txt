#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Generic PID gains and bounds. dc_pid has no actuator or safety authority. */
typedef struct {
    float kp;
    float ki;
    float kd;
    float derivative_alpha; /* derivative LPF coefficient in (0, 1] */
    float output_min;
    float output_max;
    float integral_min;
    float integral_max;
} dc_pid_config_t;

typedef struct {
    float integral;
    float prev_measurement;
    float derivative_filtered;
    bool initialized;
} dc_pid_state_t;

typedef struct {
    float output;
    float p;
    float i;
    float d;
    bool saturated_low;
    bool saturated_high;
} dc_pid_result_t;

void dc_pid_reset(dc_pid_state_t *state);

/**
 * Advance one PID sample.
 *
 * Derivative is taken on measurement to avoid setpoint kick. When
 * integrate=false the integrator is held, allowing product firmware to stop
 * windup while a safety governor or actuator inhibit overrides the requested
 * output.
 *
 * Returns false for invalid/non-finite inputs or invalid configuration. On
 * failure state is reset and result is zeroed. The caller remains responsible
 * for selecting the safe actuator state.
 */
bool dc_pid_step(dc_pid_state_t *state,
                 const dc_pid_config_t *config,
                 float setpoint,
                 float measurement,
                 float dt_s,
                 bool integrate,
                 dc_pid_result_t *result);

#ifdef __cplusplus
}
#endif
