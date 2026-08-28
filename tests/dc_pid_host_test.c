#include "dc_pid.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a,b,e) CHECK(fabsf((a) - (b)) <= (e))

static void check_state_equal(const dc_pid_state_t *actual,
                              const dc_pid_state_t *expected)
{
    CHECK(actual->integral == expected->integral);
    CHECK(actual->prev_measurement == expected->prev_measurement);
    CHECK(actual->derivative_filtered == expected->derivative_filtered);
    CHECK(actual->initialized == expected->initialized);
}

static void check_result_zero(const dc_pid_result_t *result)
{
    CHECK(result->output == 0.0f);
    CHECK(result->p == 0.0f);
    CHECK(result->i == 0.0f);
    CHECK(result->d == 0.0f);
    CHECK(!result->saturated_low);
    CHECK(!result->saturated_high);
}

int main(void)
{
    dc_pid_config_t c = {
        .kp = 0.04f,
        .ki = 0.0008f,
        .kd = 0.02f,
        .derivative_alpha = 0.20f,
        .output_min = 0.0f,
        .output_max = 1.0f,
        .integral_min = 0.0f,
        .integral_max = 1.0f,
    };
    dc_pid_state_t s = {0};
    dc_pid_result_t r = {0};

    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 0.5f, true, &r));
    CHECK(r.output > 0.0f && r.output <= 1.0f);
    CHECK(s.initialized);

    /* Holding integration models an external safety/actuator governor. */
    float held_i = s.integral;
    CHECK(dc_pid_step(&s, &c, 60.0f, 41.0f, 0.5f, false, &r));
    NEAR(s.integral, held_i, 1e-7f);

    /* Measurement may keep changing while output is inhibited. The integrator
     * stays frozen, while derivative history follows the live measurement so
     * re-enable does not see the whole inhibited interval as one derivative step. */
    CHECK(dc_pid_step(&s, &c, 60.0f, 45.0f, 2.0f, false, &r));
    NEAR(s.integral, held_i, 1e-7f);
    CHECK(isfinite(r.d));
    CHECK(dc_pid_step(&s, &c, 60.0f, 45.0f, 0.5f, true, &r));
    CHECK(s.integral >= held_i);
    CHECK(isfinite(r.output));

    /* Large positive error saturates without unbounded integral growth. */
    dc_pid_reset(&s);
    for (int i = 0; i < 1000; ++i)
        CHECK(dc_pid_step(&s, &c, 100.0f, 0.0f, 1.0f, true, &r));
    CHECK(r.output == 1.0f);
    CHECK(s.integral <= c.integral_max);

    /* A long but finite sample period remains bounded by the configured limits. */
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &c, 60.0f, 59.0f, 3600.0f, true, &r));
    CHECK(isfinite(r.output));
    CHECK(s.integral >= c.integral_min && s.integral <= c.integral_max);

    /* Derivative on measurement avoids setpoint kick. */
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &c, 50.0f, 40.0f, 1.0f, false, &r));
    float d_before = r.d;
    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, false, &r));
    NEAR(r.d, d_before, 1e-7f);

    /* Reset clears all controller memory before a new operating episode. */
    CHECK(s.initialized);
    dc_pid_reset(&s);
    CHECK(!s.initialized);
    CHECK(s.integral == 0.0f);
    CHECK(s.prev_measurement == 0.0f);
    CHECK(s.derivative_filtered == 0.0f);

    /* Invalid sample periods fail closed without discarding valid state. */
    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, true, &r));
    dc_pid_state_t before_failure = s;
    CHECK(!dc_pid_step(&s, &c, 60.0f, 40.0f, 0.0f, true, &r));
    check_state_equal(&s, &before_failure);
    CHECK(r.output == 0.0f);
    CHECK(!dc_pid_step(&s, &c, 60.0f, 40.0f, -1.0f, true, &r));
    check_state_equal(&s, &before_failure);
    CHECK(r.output == 0.0f);

    /* Invalid/non-finite inputs also preserve the last valid controller state. */
    CHECK(!dc_pid_step(&s, &c, 60.0f, NAN, 1.0f, true, &r));
    check_state_equal(&s, &before_failure);
    CHECK(r.output == 0.0f);

    /* Corrupted controller state is rejected rather than propagated. */
    s.initialized = true;
    s.integral = NAN;
    s.prev_measurement = 40.0f;
    s.derivative_filtered = 0.0f;
    CHECK(!dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    /* Finite inputs that overflow before derivative processing fail closed and
     * leave the last valid state untouched. */
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, true, &r));
    before_failure = s;
    CHECK(!dc_pid_step(&s, &c, FLT_MAX, -FLT_MAX, 1.0f, true, &r));
    check_state_equal(&s, &before_failure);
    CHECK(r.output == 0.0f);

    /* Derivative overflow must not partially advance measurement history. */
    CHECK(!dc_pid_step(&s, &c, 0.0f, FLT_MAX, FLT_MIN, false, &r));
    check_state_equal(&s, &before_failure);
    CHECK(r.output == 0.0f);

    /* Reproduce the reviewed mid-step failure: derivative math is valid, then
     * proportional overflow occurs. Integral and derivative history survive. */
    dc_pid_config_t overflow_after_derivative = c;
    overflow_after_derivative.kp = FLT_MAX;
    overflow_after_derivative.output_min = -FLT_MAX;
    overflow_after_derivative.output_max = FLT_MAX;
    before_failure = s;
    CHECK(!dc_pid_step(&s, &overflow_after_derivative,
                       3.0f, 1.0f, 1.0f, false, &r));
    check_state_equal(&s, &before_failure);
    check_result_zero(&r);

    /* Integral arithmetic can fail after derivative calculation too; no
     * candidate state is committed until the complete step is valid. */
    dc_pid_config_t integral_overflow = c;
    integral_overflow.kp = 0.0f;
    integral_overflow.ki = FLT_MAX;
    integral_overflow.output_min = -FLT_MAX;
    integral_overflow.output_max = FLT_MAX;
    before_failure = s;
    CHECK(!dc_pid_step(&s, &integral_overflow,
                       3.0f, 1.0f, 1.0f, true, &r));
    check_state_equal(&s, &before_failure);
    check_result_zero(&r);

    /* The next valid sample resumes from the preserved measurement history. */
    CHECK(dc_pid_step(&s, &c, 60.0f, 41.0f, 1.0f, false, &r));
    CHECK(s.initialized);
    CHECK(s.prev_measurement == 41.0f);
    CHECK(s.integral == before_failure.integral);

    /* Invalid configuration is rejected. */
    dc_pid_config_t bad = c;
    bad.output_min = bad.output_max;
    before_failure = s;
    CHECK(!dc_pid_step(&s, &bad, 60.0f, 40.0f, 1.0f, true, &r));
    check_state_equal(&s, &before_failure);
    CHECK(r.output == 0.0f);

    /* PI, P-only, and I-only controllers naturally disable derivative
     * filtering with kd=0 and derivative_alpha=0. */
    dc_pid_config_t pi = c;
    pi.kd = 0.0f;
    pi.derivative_alpha = 0.0f;
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &pi, 10.0f, 0.0f, 1.0f, true, &r));
    CHECK(r.p > 0.0f && r.i > 0.0f && r.d == 0.0f);
    CHECK(dc_pid_step(&s, &pi, 10.0f, 1.0f, 1.0f, true, &r));
    CHECK(r.d == 0.0f);
    CHECK(s.prev_measurement == 1.0f);
    CHECK(s.derivative_filtered == 0.0f);

    dc_pid_config_t p_only = pi;
    p_only.ki = 0.0f;
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &p_only, 10.0f, 0.0f, 1.0f, true, &r));
    CHECK(r.p > 0.0f && r.i == 0.0f && r.d == 0.0f);

    dc_pid_config_t i_only = pi;
    i_only.kp = 0.0f;
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &i_only, 10.0f, 0.0f, 1.0f, true, &r));
    CHECK(r.p == 0.0f && r.i > 0.0f && r.d == 0.0f);

    dc_pid_config_t all_zero = i_only;
    all_zero.ki = 0.0f;
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &all_zero, 10.0f, 0.0f, 1.0f, true, &r));
    CHECK(r.output == 0.0f && r.p == 0.0f && r.i == 0.0f && r.d == 0.0f);

    /* With D disabled, irrelevant derivative division cannot fail a valid
     * controller step, and live measurement history remains current. */
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &all_zero, 0.0f, 0.0f, 1.0f, false, &r));
    CHECK(dc_pid_step(&s, &all_zero, FLT_MAX, FLT_MAX, FLT_MIN, false, &r));
    CHECK(s.prev_measurement == FLT_MAX);
    CHECK(s.derivative_filtered == 0.0f);

    /* Derivative filtering still requires a positive alpha when D is active. */
    bad = c;
    bad.derivative_alpha = 0.0f;
    dc_pid_reset(&s);
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    /* Reverse/mixed-action gains are outside the direct-action contract. */
    bad = c;
    bad.kp = -0.01f;
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));
    bad = c;
    bad.ki = -0.01f;
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));
    bad = c;
    bad.kd = -0.01f;
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));

    /* Integral limits must contain reset value zero. Equal [0,0] limits are
     * valid and pin the integral term at zero. */
    bad = pi;
    bad.integral_min = 0.1f;
    bad.integral_max = 1.0f;
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));
    bad = pi;
    bad.integral_min = -1.0f;
    bad.integral_max = -0.1f;
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));
    bad = pi;
    bad.integral_min = 0.5f;
    bad.integral_max = -0.5f;
    CHECK(!dc_pid_step(&s, &bad, 10.0f, 0.0f, 1.0f, true, &r));

    dc_pid_config_t pinned_i = pi;
    pinned_i.integral_min = 0.0f;
    pinned_i.integral_max = 0.0f;
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &pinned_i, 10.0f, 0.0f, 1.0f, true, &r));
    CHECK(s.integral == 0.0f && r.i == 0.0f);

    /* Saturation flags report actual clamping, not exact equality at a rail. */
    dc_pid_config_t rails = all_zero;
    rails.kp = 1.0f;
    rails.output_min = -1.0f;
    rails.output_max = 1.0f;
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &rails, 1.0f, 0.0f, 1.0f, false, &r));
    CHECK(r.output == 1.0f && !r.saturated_low && !r.saturated_high);
    CHECK(dc_pid_step(&s, &rails, -1.0f, 0.0f, 1.0f, false, &r));
    CHECK(r.output == -1.0f && !r.saturated_low && !r.saturated_high);
    CHECK(dc_pid_step(&s, &rails, 2.0f, 0.0f, 1.0f, false, &r));
    CHECK(r.output == 1.0f && !r.saturated_low && r.saturated_high);
    CHECK(dc_pid_step(&s, &rails, -2.0f, 0.0f, 1.0f, false, &r));
    CHECK(r.output == -1.0f && r.saturated_low && !r.saturated_high);

    puts("dc_pid host checks: PASS");
    return 0;
}
