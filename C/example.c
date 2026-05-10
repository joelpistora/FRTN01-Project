#include <stdio.h>
#include <moberg.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

//Signals
double uc; //Input: set point
double y; //Input: Measured variable
double v; //Output: Controller Output
double u; //Output: Limited controller output
double e;
double velocity;
//States
double I = 0.0;//Integral part
double yold = 0.0; //Delayed measured variable

//Parameters
double K = 2.6133; //Proportional Gain
double B = 0.5; //Fraction of set point in prop. term
double Ti = 0.4523;//Integral time
double h = 0.05;// Sampling period
double r = 5.0; //Reference
double on = 1;//On = 1 or off = 0
double Tr =10;
double getY;


static pthread_mutex_t lock = __PTHREAD_MUTEX_INITIALIZER;


double calculateOutPut(double yref, double newY){
    pthread_mutex_lock(&lock);
    y = newY;
    e = yref-y;
    v = K*(B*yref - y)+I; 
    pthread_mutex_unlock(&lock);
    return v;
}
static void updateState(double u){
    pthread_mutex_lock(&lock);
    I = I + (K * h / Ti) * e + (h/Tr)*(u-v);
    yold = y;
    pthread_mutex_unlock(&lock);

}
double limit(double u_limit) {
    pthread_mutex_lock(&lock);
    if(u_limit > 5.0){
        u_limit = 5.0;
    }
    else if(u_limit < -5.0){
        u_limit = -5.0;
    }
    pthread_mutex_unlock(&lock);
return u_limit;
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
    long h = 100;
    long duration;
    long t = current_time_ms();

    while (on ==1) {  // or some run condition

    analog_in_position.read(analog_in_position.context, &getY);
    analog_in_velocity.read(analog_in_velocity.context, &velocity);
    y = getY;
    printf("[INFO] control signal: %f\n", u);
    r = 5;
    u = calculateOutPut(r, y);
    u = limit(u);
    printf("[INFO] control signal: %f\n", u);
    analog_out_u.write(analog_out_u.context, u, &uc);
    printf("[INFO] control signal: %f\n", u_c);
    updateState(u);
    t+= h;
    duration = t - current_time_ms();
    if(duration > 0) {
        sleep_ms(duration);
    }


}
   
    
    printf("[INFO] closing IO");
    status = moberg_OK(moberg_analog_out_close(moberg, 0, analog_out_u));
    status &= moberg_OK(moberg_analog_in_close(moberg, 0, analog_in_position));
    status &= moberg_OK(moberg_analog_in_close(moberg, 1, analog_in_velocity));

    moberg_free(moberg);
    
    return 0;
}
long current_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void sleep_ms(long ms) {
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}
