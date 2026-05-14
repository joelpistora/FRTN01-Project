#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <linux/sched.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "moberg.h"

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

#ifndef SCHED_FLAG_DL_OVERRUN
#define SCHED_FLAG_DL_OVERRUN 0x04
#endif

#define NSEC_PER_SEC 1000000000LL

/*
 * SCHED_DEADLINE uses struct sched_attr and sched_setattr().
 * On this system they are already declared by <sched.h>, so they must not
 * be redefined here.
 */

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}
/*
 * This function is called when the program receives Ctrl+C or SIGTERM.
 * It only changes keep_running to 0, so the main loop can stop safely.
 */

static int64_t timespec_to_ns(struct timespec t)
{
    return (int64_t)t.tv_sec * NSEC_PER_SEC + t.tv_nsec;
}
/*
 * Converts a timespec value into nanoseconds.
 * This makes time calculations easier because everything becomes one number.
 */

static struct timespec ns_to_timespec(int64_t ns)
{
    struct timespec t;
    t.tv_sec = ns / NSEC_PER_SEC;
    t.tv_nsec = ns % NSEC_PER_SEC;
    return t;
}
/*
 * Converts nanoseconds back into a timespec structure.
 * This is useful because Linux sleep functions use timespec.
 */

static struct timespec timespec_add_ns(struct timespec t, int64_t ns)
{
    return ns_to_timespec(timespec_to_ns(t) + ns);
}
/*
 * Adds a number of nanoseconds to a timespec.
 * I use it to calculate the next release time of the periodic loop.
 */

static double clamp(double x, double lo, double hi)
{
    if (x < lo) {
        return lo;
    }

    if (x > hi) {
        return hi;
    }

    return x;
}
/*
 * Limits a value between a minimum and a maximum.
 * In this program it is mainly used to keep the control voltage between -10 V and 10 V.
 */

static void lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        fprintf(stderr, "Warning: mlockall failed; continuing.\n");
    }
}
/*
 * Tries to lock the process memory in RAM.
 * This helps in real-time programs because it avoids delays caused by page faults.
 */

static void pin_to_cpu(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }

    printf("Pinned to CPU %d\n", cpu);
}
/*
 * Pins the program to one CPU core.
 * This reduces jitter because the process is not moved between different CPUs.
 */

static void set_scheduling_policy(const char *policy,
                                  int priority,
                                  uint64_t runtime_ns,
                                  uint64_t deadline_ns,
                                  uint64_t period_ns,
                                  int dl_overrun)
{
    if (strcmp(policy, "other") == 0) {
        struct sched_param param;
        memset(&param, 0, sizeof(param));

        if (sched_setscheduler(0, SCHED_OTHER, &param) != 0) {
            perror("sched_setscheduler SCHED_OTHER");
            exit(EXIT_FAILURE);
        }

        printf("Scheduling policy: SCHED_OTHER\n");
        return;
    }

    if (strcmp(policy, "fifo") == 0) {
        struct sched_param param;
        memset(&param, 0, sizeof(param));
        param.sched_priority = priority;

        if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
            perror("sched_setscheduler SCHED_FIFO");
            fprintf(stderr, "Run with sudo or give CAP_SYS_NICE.\n");
            exit(EXIT_FAILURE);
        }

        printf("Scheduling policy: SCHED_FIFO, priority=%d\n", priority);
        return;
    }

    if (strcmp(policy, "rr") == 0) {
        struct sched_param param;
        memset(&param, 0, sizeof(param));
        param.sched_priority = priority;

        if (sched_setscheduler(0, SCHED_RR, &param) != 0) {
            perror("sched_setscheduler SCHED_RR");
            fprintf(stderr, "Run with sudo or give CAP_SYS_NICE.\n");
            exit(EXIT_FAILURE);
        }

        printf("Scheduling policy: SCHED_RR, priority=%d\n", priority);
        return;
    }

    if (strcmp(policy, "deadline") == 0) {
        struct sched_attr attr;
        memset(&attr, 0, sizeof(attr));

        attr.size = sizeof(attr);
        attr.sched_policy = SCHED_DEADLINE;
        attr.sched_runtime = runtime_ns;
        attr.sched_deadline = deadline_ns;
        attr.sched_period = period_ns;

        if (dl_overrun) {
            attr.sched_flags |= SCHED_FLAG_DL_OVERRUN;
        }

        if (sched_setattr(0, &attr, 0) != 0) {
            perror("sched_setattr SCHED_DEADLINE");
            fprintf(stderr, "Run with sudo or give CAP_SYS_NICE.\n");
            exit(EXIT_FAILURE);
        }

        printf("Scheduling policy: SCHED_DEADLINE, runtime=%lu ns, deadline=%lu ns, period=%lu ns\n",
               runtime_ns, deadline_ns, period_ns);
        return;
    }

    fprintf(stderr, "Unknown policy: %s\n", policy);
    fprintf(stderr, "Allowed: other, fifo, rr, deadline\n");
    exit(EXIT_FAILURE);
}
/*
 * Selects the scheduling policy used by the process.
 * The program can run with normal Linux scheduling, FIFO, round-robin or deadline scheduling.
 * The real-time options usually need sudo permissions.
 */

/* MOBERG I/O */

/*
 * This structure stores all the Moberg configuration.
 * The installed Moberg library uses struct moberg plus analog channel objects.
 */
typedef struct {
    struct moberg *dev;
    struct moberg_analog_in ai_vel;
    struct moberg_analog_in ai_pos;
    struct moberg_analog_out ao;

    unsigned int ai_vel_chan;
    unsigned int ai_pos_chan;
    unsigned int ao_chan;
    int ai_vel_open;
    int ai_pos_open;
    int ao_open;
} io_context_t;

static io_context_t ioctx;

static void io_init(const char *device,
                    unsigned int ai_vel_chan,
                    unsigned int ai_pos_chan,
                    unsigned int ao_chan,
                    unsigned int ai_range,
                    unsigned int ao_range,
                    unsigned int aref)
{
    /*
     * These parameters are kept so the command line interface remains similar
     * to the previous COMEDI version. The Moberg API used here does not use
     * device paths, ranges or analog references directly.
     */
    (void)device;
    (void)ai_range;
    (void)ao_range;
    (void)aref;

    memset(&ioctx, 0, sizeof(ioctx));

    ioctx.dev = moberg_new();
    if (!ioctx.dev) {
        fprintf(stderr, "Could not create Moberg device context\n");
        exit(EXIT_FAILURE);
    }

    ioctx.ai_vel_chan = ai_vel_chan;
    ioctx.ai_pos_chan = ai_pos_chan;
    ioctx.ao_chan = ao_chan;

    int ok = 1;

    ioctx.ai_vel_open = moberg_OK(moberg_analog_in_open(ioctx.dev, (int)ioctx.ai_vel_chan, &ioctx.ai_vel));
    ok &= ioctx.ai_vel_open;

    ioctx.ai_pos_open = moberg_OK(moberg_analog_in_open(ioctx.dev, (int)ioctx.ai_pos_chan, &ioctx.ai_pos));
    ok &= ioctx.ai_pos_open;

    ioctx.ao_open = moberg_OK(moberg_analog_out_open(ioctx.dev, (int)ioctx.ao_chan, &ioctx.ao));
    ok &= ioctx.ao_open;

    if (!ok) {
        fprintf(stderr, "Could not open Moberg analog channels\n");
        exit(EXIT_FAILURE);
    }

    printf("Moberg I/O initialized\n");
    printf("AI velocity channel: %u\n", ioctx.ai_vel_chan);
    printf("AI position channel: %u\n", ioctx.ai_pos_chan);
    printf("AO control channel: %u\n", ioctx.ao_chan);
}
/*
 * Initializes the Moberg device.
 * It opens the analog input and output channels needed by the controller.
 */

static double io_read_ai_volts(struct moberg_analog_in *analog_in, unsigned int channel)
{
    double volts = 0.0;

    struct moberg_status status = analog_in->read(analog_in->context, &volts);
    if (!moberg_OK(status)) {
        fprintf(stderr, "Moberg analog read failed on channel %u\n", channel);
        return 0.0;
    }

    return volts;
}
/*
 * Reads one analog input channel from the Moberg device.
 * The returned value is already in volts.
 */

static double io_read_velocity_volts(void)
{
    return io_read_ai_volts(&ioctx.ai_vel, ioctx.ai_vel_chan);
}
/*
 * Reads the velocity input channel.
 */

static double io_read_position_volts(void)
{
    return io_read_ai_volts(&ioctx.ai_pos, ioctx.ai_pos_chan);
}
/*
 * Reads the position input channel.
 */

static void io_write_control_volts(double volts)
{
    volts = clamp(volts, -10.0, 10.0);

    struct moberg_status status = ioctx.ao.write(ioctx.ao.context, volts, NULL);
    if (!moberg_OK(status)) {
        fprintf(stderr, "Moberg analog write failed on channel %u\n", ioctx.ao_chan);
    }
}
/*
 * Writes the control voltage to the analog output channel.
 * The value is saturated to +/-10 V before it is sent to the Moberg device.
 */

static void io_shutdown(void)
{
    if (ioctx.ao_open) {
        io_write_control_volts(0.0);
        moberg_analog_out_close(ioctx.dev, (int)ioctx.ao_chan, ioctx.ao);
        ioctx.ao_open = 0;
    }

    if (ioctx.ai_vel_open) {
        moberg_analog_in_close(ioctx.dev, (int)ioctx.ai_vel_chan, ioctx.ai_vel);
        ioctx.ai_vel_open = 0;
    }

    if (ioctx.ai_pos_open) {
        moberg_analog_in_close(ioctx.dev, (int)ioctx.ai_pos_chan, ioctx.ai_pos);
        ioctx.ai_pos_open = 0;
    }

    if (ioctx.dev) {
        moberg_free(ioctx.dev);
        ioctx.dev = NULL;
    }
}
/*
 * Safely closes the Moberg device.
 * Before closing it sets the output voltage to 0 V so the actuator is not left powered.
 */

/* Controller */

static double velocity_pi_controller(double reference_v, double velocity_v, double h)
{
    static double I = 0.0;

    const double K = 2.6133333333333333;
    const double Ti = 0.4523076923076923;
    const double beta = 0.5;

    double error = reference_v - velocity_v;

    double u = K * beta * reference_v - K * velocity_v + I;

    I = I + (K * h / Ti) * error;

    I = clamp(I, -10.0, 10.0);
    u = clamp(u, -10.0, 10.0);

    return u;
}
/*
 * This is the PI speed controller.
 * The proportional part uses a weighted reference with beta, and the integral part remembers past error.
 * Both the integral term and the final output are limited to avoid very large control values.
 */

/*
 * This structure stores all the important values from one control iteration.
 * Later these values are written to the CSV log file.
 */
typedef struct {
    double reference_v;
    double velocity_v;
    double position_v;
    double control_v;
} sample_t;

static sample_t controller_step(double reference_v, double h)
{
    sample_t s;

    s.reference_v = reference_v;
    s.velocity_v = io_read_velocity_volts();
    s.position_v = io_read_position_volts();

    s.control_v = velocity_pi_controller(s.reference_v, s.velocity_v, h);

    io_write_control_volts(s.control_v);

    return s;
}
/*
 * Executes one complete control step.
 * It reads the sensors, calculates the PI control signal, writes the output voltage,
 * and returns the values so they can be logged.
 */

/* CLI */

static void print_usage(const char *program)
{
    printf("Usage:\n");
    printf("  %s [options]\n\n", program);

    printf("Scheduling options:\n");
    printf("  --policy other|fifo|rr|deadline   default: other\n");
    printf("  --cpu N                            default: 1\n");
    printf("  --priority N                       FIFO/RR priority, default: 80\n");
    printf("  --period-us N                      default: 50000\n");
    printf("  --runtime-us N                     SCHED_DEADLINE runtime, default: 5000\n");
    printf("  --deadline-us N                    SCHED_DEADLINE deadline, default: period\n");
    printf("  --dl-overrun                       Enable SCHED_FLAG_DL_OVERRUN\n\n");

    printf("Experiment options:\n");
    printf("  --duration-s N                     default: 20\n");
    printf("  --reference-v X                    default: 5.0\n\n");

    printf("Moberg I/O options:\n");
    printf("  --device PATH                      default: /dev/moberg0\n");
    printf("  --ai-pos N                         position AI channel, default: 0\n");
    printf("  --ai-vel N                         velocity AI channel, default: 1\n");
    printf("  --ao N                             output AO channel, default: 0\n");
    printf("  --ai-range N                       kept for compatibility, default: 0\n");
    printf("  --ao-range N                       kept for compatibility, default: 0\n");
    printf("  --aref ground|common|diff|other    kept for compatibility, default: ground\n\n");

    printf("Examples:\n");
    printf("  %s --policy other --cpu 1\n", program);
    printf("  sudo %s --policy fifo --cpu 1 --priority 80\n", program);
    printf("  sudo %s --policy deadline --cpu 1 --period-us 50000 --runtime-us 5000\n", program);
}
/*
 * Prints the help message of the program.
 * It shows all the command line options and some examples of how to run it.
 */

static unsigned int parse_aref(const char *s)
{
    if (strcmp(s, "ground") == 0) {
        return 0;
    }

    if (strcmp(s, "common") == 0) {
        return 1;
    }

    if (strcmp(s, "diff") == 0) {
        return 2;
    }

    if (strcmp(s, "other") == 0) {
        return 3;
    }

    fprintf(stderr, "Unknown aref: %s\n", s);
    fprintf(stderr, "Allowed: ground, common, diff, other\n");
    exit(EXIT_FAILURE);
}
/*
 * Converts the analog reference option from text to a number.
 * In this Moberg version the value is kept only for command line compatibility.
 */

int main(int argc, char **argv)
{
    /*
     * Default values for the program.
     * These are used if the user does not give another value from the terminal.
     */
    const char *policy = "other";
    const char *device = "/dev/moberg0";

    /*
     * Default real-time and experiment parameters.
     * cpu = CPU core where I want to run the controller.
     * priority is only used for SCHED_FIFO and SCHED_RR.
     * duration_s is how long the experiment runs.
     */
    int cpu = 1;
    int priority = 80;
    int duration_s = 20;
    int dl_overrun = 0;

    /*
     * Time parameters in microseconds.
     * By default the controller period is 50000 us = 50 ms,
     * which is the same sampling time used in the DC servo lab.
     */
    uint64_t period_us = 50000;
    uint64_t runtime_us = 5000;
    uint64_t deadline_us = 0;

    /*
     * Reference value for the velocity controller.
     * I use volts because the analog inputs and outputs are handled as voltages.
     */
    double reference_v = 5.0;

    /*
     * Default Moberg channels.
     *
     * These now match the example program:
     *
     * AI0 = position
     * AI1 = velocity
     * AO0 = control output
     */
    unsigned int ai_vel_chan = 1;
    unsigned int ai_pos_chan = 0;
    unsigned int ao_chan = 0;
    unsigned int ai_range = 0;
    unsigned int ao_range = 0;
    unsigned int aref = 0;

    /*
     * This array defines all the command line options that the program accepts.
     * For example, --policy, --cpu, --period-us, etc.
     * Each option is connected to one character that is used later in the switch.
     */
    static struct option long_options[] = {
        {"policy", required_argument, 0, 'p'},
        {"cpu", required_argument, 0, 'c'},
        {"priority", required_argument, 0, 'q'},
        {"period-us", required_argument, 0, 'T'},
        {"runtime-us", required_argument, 0, 'r'},
        {"deadline-us", required_argument, 0, 'd'},
        {"dl-overrun", no_argument, 0, 'o'},

        {"duration-s", required_argument, 0, 'D'},
        {"reference-v", required_argument, 0, 'R'},

        {"device", required_argument, 0, 'e'},
        {"ai-vel", required_argument, 0, 'v'},
        {"ai-pos", required_argument, 0, 'x'},
        {"ao", required_argument, 0, 'a'},
        {"ai-range", required_argument, 0, 'i'},
        {"ao-range", required_argument, 0, 'u'},
        {"aref", required_argument, 0, 'A'},

        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    /*
     * Read the command line arguments.
     * This lets me change the scheduling policy, CPU, period, duration,
     * Moberg channels, etc. without recompiling the program.
     */
    int opt;
    while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {

        /*
         * Scheduling options.
         */
        case 'p':
            policy = optarg;
            break;

        case 'c':
            cpu = atoi(optarg);
            break;

        case 'q':
            priority = atoi(optarg);
            break;

        case 'T':
            period_us = strtoull(optarg, NULL, 10);
            break;

        case 'r':
            runtime_us = strtoull(optarg, NULL, 10);
            break;

        case 'd':
            deadline_us = strtoull(optarg, NULL, 10);
            break;

        case 'o':
            dl_overrun = 1;
            break;

        /*
         * Experiment options.
         */
        case 'D':
            duration_s = atoi(optarg);
            break;

        case 'R':
            reference_v = atof(optarg);
            break;

        /*
         * Moberg input/output options.
         * The ranges and analog reference are kept for compatibility with the old interface.
         */
        case 'e':
            device = optarg;
            break;

        case 'v':
            ai_vel_chan = (unsigned int)strtoul(optarg, NULL, 10);
            break;

        case 'x':
            ai_pos_chan = (unsigned int)strtoul(optarg, NULL, 10);
            break;

        case 'a':
            ao_chan = (unsigned int)strtoul(optarg, NULL, 10);
            break;

        case 'i':
            ai_range = (unsigned int)strtoul(optarg, NULL, 10);
            break;

        case 'u':
            ao_range = (unsigned int)strtoul(optarg, NULL, 10);
            break;

        case 'A':
            aref = parse_aref(optarg);
            break;

        /*
         * If the user asks for help or gives a wrong option,
         * I print the usage message and exit.
         */
        case 'h':
        default:
            print_usage(argv[0]);
            return 0;
        }
    }

    /*
     * If the user does not give a specific deadline,
     * I use the period as the deadline.
     * This means every job should finish before the next period starts.
     */
    if (deadline_us == 0) {
        deadline_us = period_us;
    }

    /*
     * For SCHED_DEADLINE, Linux requires:
     *
     * runtime <= deadline <= period
     *
     * If this condition is not true, the parameters do not make sense,
     * so the program stops.
     */
    if (runtime_us > deadline_us || deadline_us > period_us) {
        fprintf(stderr, "Invalid SCHED_DEADLINE parameters: runtime <= deadline <= period required.\n");
        return EXIT_FAILURE;
    }

    /*
     * Convert the time values from microseconds to nanoseconds.
     * The Linux timing functions and SCHED_DEADLINE parameters use nanoseconds.
     */
    uint64_t period_ns = period_us * 1000ULL;
    uint64_t runtime_ns = runtime_us * 1000ULL;
    uint64_t deadline_ns = deadline_us * 1000ULL;

    /*
     * Sampling time in seconds.
     * This value is used by the PI controller.
     */
    double h = (double)period_us / 1000000.0;

    /*
     * Register signal handlers.
     * This makes Ctrl+C stop the controller safely instead of killing it immediately.
     */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /*
     * Initialize the Moberg device and the analog channels.
     * After this, the program can read velocity/position and write the control voltage.
     *
     * Default wiring:
     *
     * AI0 = position
     * AI1 = velocity
     * AO0 = control
     */
    io_init(device,
            ai_vel_chan,
            ai_pos_chan,
            ao_chan,
            ai_range,
            ao_range,
            aref);

    /*
     * Real-time setup.
     * First I lock memory to reduce page faults.
     * Then I pin the controller to one CPU.
     * Finally I set the selected scheduling policy.
     */
    lock_memory();
    pin_to_cpu(cpu);

    set_scheduling_policy(policy,
                          priority,
                          runtime_ns,
                          deadline_ns,
                          period_ns,
                          dl_overrun);

    /*
     * Open the CSV file where I save the timing and control data.
     * This file is used later to compare the scheduling policies.
     */
    FILE *log = fopen("controller_log.csv", "w");
    if (!log) {
        perror("fopen controller_log.csv");
        io_shutdown();
        return EXIT_FAILURE;
    }

    /*
     * Write the header row of the CSV file.
     * Each row after this will contain data from one controller iteration.
     */
    fprintf(log,
            "iteration,"
            "release_ns,"
            "start_ns,"
            "finish_ns,"
            "latency_ns,"
            "jitter_ns,"
            "exec_ns,"
            "deadline_miss,"
            "reference_v,"
            "velocity_v,"
            "position_v,"
            "control_v\n");

    /*
     * Get the current time and calculate the first release time.
     * The controller uses absolute time, so the next releases are based on this clock.
     */
    struct timespec start_time;
    struct timespec next_release;

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    next_release = timespec_add_ns(start_time, (int64_t)period_ns);

    /*
     * Calculate when the experiment should stop.
     */
    int64_t experiment_end_ns =
        timespec_to_ns(start_time) + (int64_t)duration_s * NSEC_PER_SEC;

    /*
     * Variables used to count iterations and deadline misses.
     */
    long iteration = 0;
    long deadline_misses = 0;

    /*
     * Variables used to calculate timing statistics.
     */
    int64_t previous_start_ns = 0;
    int64_t max_latency_ns = 0;
    int64_t max_abs_jitter_ns = 0;
    int64_t max_exec_ns = 0;

    /*
     * Print the experiment configuration so I can see what is being tested.
     */
    printf("\nController started\n");
    printf("Policy:       %s\n", policy);
    printf("CPU:          %d\n", cpu);
    printf("Period:       %lu us\n", period_us);
    printf("h:            %.6f s\n", h);
    printf("Reference:    %.3f V\n", reference_v);
    printf("Duration:     %d s\n", duration_s);
    printf("Log:          controller_log.csv\n");
    printf("Connections:  AI0=position, AI1=velocity, AO0=control\n");
    printf("Press Ctrl+C to stop.\n\n");

    /*
     * Main periodic loop.
     * Each iteration corresponds to one sample of the controller.
     */
    while (keep_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /*
         * Stop when the experiment time has passed.
         */
        if (timespec_to_ns(now) >= experiment_end_ns) {
            break;
        }

        /*
         * Sleep until the next absolute release time.
         * TIMER_ABSTIME is important because it avoids accumulating drift.
         */
        int ret = clock_nanosleep(CLOCK_MONOTONIC,
                                  TIMER_ABSTIME,
                                  &next_release,
                                  NULL);

        /*
         * If the sleep was interrupted by Ctrl+C or SIGTERM,
         * stop safely without executing another controller step.
         */
        if (ret == EINTR) {
            if (!keep_running) {
                break;
            }
            continue;
        }

        /*
         * If the sleep failed for another reason,
         * print the error and stop the loop.
         */
        if (ret != 0) {
            errno = ret;
            perror("clock_nanosleep");
            break;
        }

        /*
         * Measure the actual start time of this iteration.
         * This is used to calculate latency and jitter.
         */
        struct timespec actual_start;
        clock_gettime(CLOCK_MONOTONIC, &actual_start);

        int64_t release_ns = timespec_to_ns(next_release);
        int64_t start_ns = timespec_to_ns(actual_start);

        /*
         * Latency is how late the controller started compared to its planned release time.
         */
        int64_t latency_ns = start_ns - release_ns;

        /*
         * Jitter is the variation in the time between two consecutive starts.
         * Ideally, the difference between starts should be exactly equal to the period.
         */
        int64_t jitter_ns = 0;
        if (previous_start_ns != 0) {
            jitter_ns = (start_ns - previous_start_ns) - (int64_t)period_ns;
        }
        previous_start_ns = start_ns;

        /*
         * Execute one controller step:
         * read sensors, compute PI output, and write the control voltage.
         *
         * With the corrected default connections:
         *
         * position is read from AI0
         * velocity is read from AI1
         * control is written to AO0
         */
        sample_t s = controller_step(reference_v, h);

        /*
         * Measure when the controller step finished.
         */
        struct timespec finish;
        clock_gettime(CLOCK_MONOTONIC, &finish);

        int64_t finish_ns = timespec_to_ns(finish);

        /*
         * Execution time is the time spent inside this iteration after waking up.
         */
        int64_t exec_ns = finish_ns - start_ns;

        /*
         * Check if the controller finished after its deadline.
         * If yes, I count it as a deadline miss.
         */
        int deadline_miss = finish_ns > release_ns + (int64_t)deadline_ns;
        if (deadline_miss) {
            deadline_misses++;
        }

        /*
         * Update maximum latency, jitter and execution time.
         * These are printed at the end as a simple summary.
         */
        if (latency_ns > max_latency_ns) {
            max_latency_ns = latency_ns;
        }

        if (llabs(jitter_ns) > max_abs_jitter_ns) {
            max_abs_jitter_ns = llabs(jitter_ns);
        }

        if (exec_ns > max_exec_ns) {
            max_exec_ns = exec_ns;
        }

        /*
         * Save one line in the CSV log file.
         * This contains both timing data and control data.
         */
        fprintf(log,
                "%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%.9f,%.9f,%.9f,%.9f\n",
                iteration,
                release_ns,
                start_ns,
                finish_ns,
                latency_ns,
                jitter_ns,
                exec_ns,
                deadline_miss,
                s.reference_v,
                s.velocity_v,
                s.position_v,
                s.control_v);

        /*
         * Calculate the next release time.
         * I add the period to the previous release time instead of using "now + period",
         * because this keeps the periodic loop aligned with the original time base.
         */
        next_release = timespec_add_ns(next_release, (int64_t)period_ns);

        iteration++;
    }

    /*
     * Stop the actuator and close the Moberg device.
     * This is important so the control output is not left active.
     */
    io_shutdown();

    /*
     * Close the log file after all data has been written.
     */
    fclose(log);

    /*
     * Print the final results of the experiment.
     * These values are useful for comparing different scheduling policies.
     */
    printf("\nFinished.\n");
    printf("Iterations:          %ld\n", iteration);
    printf("Deadline misses:     %ld\n", deadline_misses);
    printf("Max latency:         %.3f us\n", max_latency_ns / 1000.0);
    printf("Max absolute jitter: %.3f us\n", max_abs_jitter_ns / 1000.0);
    printf("Max exec time:       %.3f us\n", max_exec_ns / 1000.0);
    printf("Control output set to 0 V.\n");

    return 0;
}
