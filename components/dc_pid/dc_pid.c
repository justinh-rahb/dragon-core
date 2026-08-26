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

static bool state_valid(const dc_pid_state_t *state)
{
    return state &&
           isfinite(state->integral) &&
           (!state->initialized ||
            (isfinite(state->prev_measurement) &&
             isfinite(state->derivative_filtered)));
}

void dc_pid_reset(dc_pid_state_t *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static bool fail_step(dc_pid_state_t *state)
{
    if (state) dc_pid_reset(state);
    return false;
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

    if (!state || !result || !config_valid(config) || !state_valid(state) ||
        !isfinite(setpoint) || !isfinite(measurement) ||
        !isfinite(dt_s) || dt_s <= 0.0f)
        return fail_step(state);

    const float error = setpoint - measurement;
    if (!isfinite(error))
        return fail_step(state);

    if (!state->initialized) {
        state->prev_measurement = measurement;
        state->derivative_filtered = 0.0f;
        state->initialized = true;
    }

    const float measurement_delta = measurement - state->prev_measurement;
    if (!isfinite(measurement_delta))
        return fail_step(state);

    const float derivative_raw = -measurement_delta / dt_s;
    if (!isfinite(derivative_raw))
        return fail_step(state);

    const float derivative_filtered = state->derivative_filtered +
        config->derivative_alpha * (derivative_raw - state->derivative_filtered);
    if (!isfinite(derivative_filtered))
        return fail_step(state);

    state->derivative_filtered = derivative_filtered;
    state->prev_measurement = measurement;

    const float p = config->kp * error;
    const float d = config->kd * state->derivative_filtered;
    if (!isfinite(p) || !isfinite(d))
        return fail_step(state);

    if (integrate) {
        const float integral_delta = config->ki * error * dt_s;
        if (!isfinite(integral_delta))
            return fail_step(state);

        const float candidate_i_unclamped = state->integral + integral_delta;
        if (!isfinite(candidate_i_unclamped))
            return fail_step(state);

        const float candidate_i = clampf(candidate_i_unclamped,
                                         config->integral_min,
                                         config->integral_max);
        const float candidate_output = p + candidate_i + d;
        if (!isfinite(candidate_output))
            return fail_step(state);

        /* Conditional integration: do not push farther into saturation. */
        const bool pushes_high = candidate_output > config->output_max && error > 0.0f;
        const bool pushes_low = candidate_output < config->output_min && error < 0.0f;
        if (!pushes_high && !pushes_low)
            state->integral = candidate_i;
    }

    const float raw_output = p + state->integral + d;
    if (!isfinite(raw_output))
        return fail_step(state);

    result->output = clampf(raw_output, config->output_min, config->output_max);
    result->p = p;
    result->i = state->integral;
    result->d = d;
    result->saturated_low = raw_output < config->output_min;
    result->saturated_high = raw_output > config->output_max;
    return true;
}
