#include "dc_pid.h"

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

    /* Large positive error saturates without unbounded integral growth. */
    dc_pid_reset(&s);
    for (int i = 0; i < 1000; ++i)
        CHECK(dc_pid_step(&s, &c, 100.0f, 0.0f, 1.0f, true, &r));
    CHECK(r.output == 1.0f);
    CHECK(s.integral <= c.integral_max);

    /* Derivative on measurement avoids setpoint kick. */
    dc_pid_reset(&s);
    CHECK(dc_pid_step(&s, &c, 50.0f, 40.0f, 1.0f, false, &r));
    float d_before = r.d;
    CHECK(dc_pid_step(&s, &c, 60.0f, 40.0f, 1.0f, false, &r));
    NEAR(r.d, d_before, 1e-7f);

    /* Invalid input fails closed at the API boundary and resets state. */
    CHECK(!dc_pid_step(&s, &c, 60.0f, NAN, 1.0f, true, &r));
    CHECK(!s.initialized);
    CHECK(r.output == 0.0f);

    puts("dc_pid host checks: PASS");
    return 0;
}
