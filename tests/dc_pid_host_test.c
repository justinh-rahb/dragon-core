#include "dc_pid.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define NEAR(a,b,e) CHECK(fabsf((a) - (b)) <= (e))

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

    /* Invalid sample periods fail closed and reset state. */
    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, true, &r));
    CHECK(!dc_pid_step(&s, &c, 60.0f, 40.0f, 0.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);
    CHECK(!dc_pid_step(&s, &c, 60.0f, 40.0f, -1.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    /* Invalid/non-finite inputs fail closed at the API boundary. */
    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, true, &r));
    CHECK(!dc_pid_step(&s, &c, 60.0f, NAN, 1.0f, true, &r));
    CHECK(!s.initialized);
    CHECK(r.output == 0.0f);

    /* Corrupted controller state is rejected rather than propagated. */
    s.initialized = true;
    s.integral = NAN;
    s.prev_measurement = 40.0f;
    s.derivative_filtered = 0.0f;
    CHECK(!dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    /* Finite API inputs that overflow an intermediate calculation also fail
     * closed instead of poisoning the persistent PID state. */
    CHECK(!dc_pid_step(&s, &c, FLT_MAX, -FLT_MAX, 1.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    CHECK(dc_pid_step(&s, &c, 0.0f, 0.0f, 1.0f, false, &r));
    CHECK(!dc_pid_step(&s, &c, 0.0f, FLT_MAX, FLT_MIN, false, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    /* Invalid configuration is rejected. */
    dc_pid_config_t bad = c;
    bad.output_min = bad.output_max;
    CHECK(!dc_pid_step(&s, &bad, 60.0f, 40.0f, 1.0f, true, &r));
    CHECK(!s.initialized && r.output == 0.0f);

    puts("dc_pid host checks: PASS");
    return 0;
}
