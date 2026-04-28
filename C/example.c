#include <stdio.h>
#include <moberg.h>

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
    struct moberg_analog_in analog_in_position;
    struct moberg_analog_in analog_in_velocity;

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

    double u_c;
    double u;
    double position;
    double velocity;

    printf("[INFO] Reading inputs \n");
    analog_in_position.read(analog_in_position.context, &position);
    analog_in_velocity.read(analog_in_velocity.context, &velocity);

    printf("[INFO] The inputs are pos: %f, vel %f \n", position, velocity);

    u = 1;
    printf("[INFO] sending a control signal of %f\n", u);
    analog_out_u.write(analog_out_u.context, u_c, &u);

    u = 0;
    printf("[INFO] sending a control signal of %f\n", u);
    analog_out_u.write(analog_out_u.context, u_c, &u);

    printf("[INFO] closing IO");
    status = moberg_OK(moberg_analog_out_close(moberg, 0, analog_out_u));
    status &= moberg_OK(moberg_analog_in_close(moberg, 0, analog_in_position));
    status &= moberg_OK(moberg_analog_in_close(moberg, 1, analog_in_velocity));

    moberg_free(moberg);

    return 0;
}
