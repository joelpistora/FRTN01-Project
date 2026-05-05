#include <stdio.h>
#include <moberg.h>
#include <time.h>



double I = 0.0;
double K = 2.6133;
double B = 0.5;
double Ti = 0.4523;
double h = 0.05;
double r = 5.0;
double on = 0;
double u= 0.0;



static void controller(struct moberg_analog_out analog_out_u,
                       struct moberg_analog_in  analog_in_position,
                       struct moberg_analog_in  analog_in_velocity)
{
    double u_c;
    double position, velocity;
    double y, u_unsat;

    analog_in_position.read(analog_in_position.context, &position);
    analog_in_velocity.read(analog_in_velocity.context, &velocity);
    printf("[INFO] pos: %f, vel: %f\n", position, velocity);

    if (on == 1) {
        y = position;

        // PI with setpoint weighting
        u_unsat = K * B * r - K * y + I;

        // Saturate output
        if      (u_unsat >  10.0) u =  10.0;
        else if (u_unsat < -10.0) u = -10.0;
        else                      u =  u_unsat;

        printf("[INFO] control signal: %f\n", u);
        analog_out_u.write(analog_out_u.context, u, &u_c);
        printf("[INFO] actual sent:    %f\n", u_c);

        // Anti-windup: only integrate when not saturated
        if (u == u_unsat) {
            I = I + (K * h / Ti) * (r - y);
        }
    } else {
        analog_out_u.write(analog_out_u.context, 0.0, &u_c);
        I = 0.0;
    }
}


int main()
{

    printf("[INFO] initializing moberg \n");
    struct moberg *moberg = moberg_new();
    if (!moberg)
    {
        printf("[ERROR] failed to initialize moberg\n");
        return -1;
    }

    // Output
    struct moberg_analog_out analog_out_u;

    // Input
    struct moberg_analog_in analog_in_position;//gets position
    struct moberg_analog_in analog_in_velocity;//gets velocity

    printf("[INFO] opening input and output ports\n");
    int status;
    status = moberg_OK(moberg_analog_out_open(moberg, 0, &analog_out_u));

    status &= moberg_OK(moberg_analog_in_open(moberg, 0, &analog_in_position));
    status &= moberg_OK(moberg_analog_in_open(moberg, 1, &analog_in_velocity));

    if (!status)
    {
        printf("[ERROR] IO not work \n");
        return -1;
    }
    struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);

    while (on) {  // or some run condition
      controller(analog_out_u, analog_in_position, analog_in_velocity);

    // Advance deadline by h seconds
    next.tv_nsec += (long)(h * 1e9);
      if (next.tv_nsec >= 1000000000L) {
        next.tv_nsec -= 1000000000L;
        next.tv_sec  += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
}
   
    
    printf("[INFO] closing IO");
    status = moberg_OK(moberg_analog_out_close(moberg, 0, analog_out_u));
    status &= moberg_OK(moberg_analog_in_close(moberg, 0, analog_in_position));
    status &= moberg_OK(moberg_analog_in_close(moberg, 1, analog_in_velocity));

    moberg_free(moberg);
    return 0;
}
