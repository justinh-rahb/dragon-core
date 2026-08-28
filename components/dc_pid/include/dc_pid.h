#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Generic PID gains and bounds. dc_pid has no actuator or safety authority. */
typedef struct {
    float kp; /* non-negative; zero disables the proportional term */
    float ki; /* non-negative; zero disables integral accumulation */
    float kd; /* non-negative; zero disables the derivative term */
    /**
     * Per-step derivative EMA coefficient in (0, 1]. A value of zero is valid
     * only when kd is zero. Stable update cadence is expected because this is
     * a per-step coefficient, not a time constant or cutoff frequency.
     */
    float derivative_alpha;
    float output_min;
    float output_max;
    /**
     * Integral limits must contain zero so reset state is valid. Equal limits
     * are allowed; [0, 0] disables/pins the integral contribution at zero.
     */
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
    /** True only when raw output was below output_min and was clamped. */
    bool saturated_low;
    /** True only when raw output was above output_max and was clamped. */
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
