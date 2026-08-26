#include "dc_pid.h"

#include <math.h>
#include <string.h>

static float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static bool config_valid(const dc_pid_config_t *c)
{
    return c &&
           isfinite(c->kp) && isfinite(c->ki) && isfinite(c->kd) &&
           isfinite(c->derivative_alpha) &&
           c->derivative_alpha > 0.0f && c->derivative_alpha <= 1.0f &&
           isfinite(c->output_min) && isfinite(c->output_max) &&
           c->output_min < c->output_max &&
           isfinite(c->integral_min) && isfinite(c->integral_max) &&
           c->integral_min <= c->integral_max;
}

void dc_pid_reset(dc_pid_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

bool dc_pid_step(dc_pid_state_t *state,
                 const dc_pid_config_t *config,
                 float setpoint,
                 float measurement,
                 float dt_s,
                 bool integrate,
                 dc_pid_result_t *result)
{
    if (result) memset(result, 0, sizeof(*result));

    if (!state || !result || !config_valid(config) ||
        !isfinite(setpoint) || !isfinite(measurement) ||
        !isfinite(dt_s) || dt_s <= 0.0f) {
        if (state) dc_pid_reset(state);
        return false;
    }

    const float error = setpoint - measurement;
    if (!state->initialized) {
        state->prev_measurement = measurement;
        state->derivative_filtered = 0.0f;
        state->initialized = true;
    }

    const float derivative_raw = -(measurement - state->prev_measurement) / dt_s;
    state->derivative_filtered += config->derivative_alpha *
                                  (derivative_raw - state->derivative_filtered);
    state->prev_measurement = measurement;

    const float p = config->kp * error;
    const float d = config->kd * state->derivative_filtered;

    if (integrate) {
        const float candidate_i = clampf(state->integral + config->ki * error * dt_s,
                                         config->integral_min,
                                         config->integral_max);
        const float candidate_output = p + candidate_i + d;

        /* Conditional integration: do not push farther into saturation. */
        const bool pushes_high = candidate_output > config->output_max && error > 0.0f;
        const bool pushes_low = candidate_output < config->output_min && error < 0.0f;
        if (!pushes_high && !pushes_low)
            state->integral = candidate_i;
    }

    const float raw_output = p + state->integral + d;
    result->output = clampf(raw_output, config->output_min, config->output_max);
    result->p = p;
    result->i = state->integral;
    result->d = d;
    result->saturated_low = raw_output < config->output_min;
    result->saturated_high = raw_output > config->output_max;
    return true;
}
