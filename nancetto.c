#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sndfile.h>

#define MAX_SAMPLE_DELAY 1024
#define SRAND_SEED 123487



// UTIL

double noise () {

    return rand () / (double) RAND_MAX;
}



// THE SYNTH LOL

typedef struct synth_t {

    // public
    double gain;    // master output gain
    double noise;
    double stiffness_nonlinear_coefficient;
    double stiffness_nonlinear_degree;
    double damping_nonlinear_coefficient;
    double damping_nonlinear_degree;
    double damping;
    double lips_reflection;
    double lips_coupling;
    double max_input_pressure;

    // live paramaters
    double blowing_pressure;
    double vibrato_rate;
    double vibrato_depth;
    double tremolo_rate;
    double tremolo_depth;

    // private
    double time;
    double rate;
    double fundamental;
    double stiffness;
    double lips_tension_scaling; // scales actual lips tension on log scale

    int n_delay_samples;
    int i_delay;
    double fractional_delay_amount;
    double delay_line[MAX_SAMPLE_DELAY];
    double delay_output;

    double x;   // mass displacement
    double v;   // mass velocity

} synth_t;

void synth_update_stiffness (synth_t *synth) {

    double scale = pow (2.0, synth->lips_tension_scaling);
    synth->stiffness = synth->fundamental * 2 * M_PI * scale;
    synth->stiffness *= synth->stiffness;
}

void synth_set_lips_tension_scaling (synth_t *synth, double scaling) {

    synth->lips_tension_scaling = scaling;
    synth_update_stiffness (synth);
}

void synth_set_fundamental (synth_t *synth, double fundamental) {

    synth->fundamental = fundamental;
    double samples_length = synth->rate / fundamental;
    synth->n_delay_samples = (int) samples_length;
    synth->fractional_delay_amount = samples_length - (int) samples_length;
    synth_update_stiffness (synth);
}

void synth_run_delay (synth_t *synth, double input) {

    synth->delay_output = synth->delay_line[synth->i_delay];
    synth->delay_line[synth->i_delay++] = input;
    synth->i_delay %= synth->n_delay_samples;

    // TODO: run the output through an allpass filter for fractional delay
}

double synth_process (synth_t *synth) {

    // TODO: temp
    synth_set_lips_tension_scaling (synth, synth->time);

    // TODO: optimize low frequency changes out into a "control rate" function

    // vibrato and tremolo
    // TODO: optimize
    double vibrato = 1 + sin (synth->time * 2 * M_PI * synth->vibrato_rate) * synth->vibrato_depth;
    double tremolo = 1 + sin (synth->time * 2 * M_PI * synth->tremolo_rate) * synth->tremolo_depth;

    // the input pressure mixed with a little noise
    double input_pressure = synth->blowing_pressure;
    double normalized_noise = noise () * 2 - 1;
    input_pressure += normalized_noise * input_pressure * synth->noise;
    input_pressure *= tremolo;
    input_pressure = fmin (synth->max_input_pressure, input_pressure);

    // the delay line feedback and input from reed
    double feedback = synth->lips_reflection * synth->delay_output;
    double reed_input = synth->delay_output - feedback;
    double reed_output = input_pressure * synth->x * synth->x;
    double delay_input = feedback + reed_output;
    // TODO: reflection filter on the delay_input
    double filter = delay_input; // TODO this line
    synth_run_delay (synth, filter);
    double output = filter * synth->gain;

    // the reed mass spring system
    // coupled with delay line
    // and driven by input pressure
    double k = synth->stiffness * vibrato;
    double b = synth->damping;
    double nk = synth->stiffness_nonlinear_coefficient;
    double nkd = synth->stiffness_nonlinear_degree;
    double nb = synth->damping_nonlinear_coefficient;
    double nbd = synth->damping_nonlinear_degree;
    double a = -k * (synth->x + nk * pow (synth->x, 2 * nkd + 1))
               -b * (synth->v + nb * pow (synth->x, 2 * nbd)) * synth->fundamental;
    a += k * (input_pressure - reed_input * synth->lips_coupling);
    synth->v += a / synth->rate;
    synth->x += synth->v / synth->rate;
    synth->x = fmin (1, fmax (-1, synth->x));

    synth->time += 1 / synth->rate;

    // TODO: dc blocker...
    return output;
}

synth_t *create_synth (double rate) {

    synth_t *synth = (synth_t *) malloc (sizeof (synth_t));
    memset (synth, 0, sizeof (synth_t));

    synth->gain = 0.5;
    synth->noise = 0.005;
    synth->stiffness_nonlinear_coefficient = 2;
    synth->stiffness_nonlinear_degree = 10;
    synth->damping_nonlinear_coefficient = 5;
    synth->damping_nonlinear_degree = 1;
    synth->damping = 0.1;
    synth->lips_reflection = 0.5;
    synth->lips_coupling = 0.5;
    synth->max_input_pressure = 1.25;

    synth->lips_tension_scaling = 0;
    synth->blowing_pressure = 0.75;

    synth->rate = rate;
    synth_set_fundamental (synth, 174.61 / 2);

    synth->vibrato_rate = 5;
    synth->vibrato_depth = 0.05;
    synth->tremolo_rate = 2;
    synth->tremolo_depth = 0.05;

    return synth;
}

void destroy_synth (synth_t *synth) {

    free (synth);
}



// MAIN N STUFF

int main (int argc, char **argv) {

    srand (SRAND_SEED);

    const int rate = 48000;
    const double render_time = 4; // seconds
    const int n_samples = rate * render_time; // samples to render

    synth_t *synth = create_synth (rate);

    // render
    printf ("rendering %lf seconds @ %d hz...\n", render_time, rate);
    float buffer[n_samples];
    for (int i = 0; i < n_samples; i++)
        buffer[i] = (float) synth_process (synth);

    // write to file
    puts ("opening file...");
    SF_INFO sfinfo;
    sfinfo.samplerate = rate;
    sfinfo.channels = 1;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_32;
    SNDFILE *sf = sf_open ("out.wav", SFM_WRITE, &sfinfo);
    if (sf == NULL)
        fprintf (stderr, "Could not open output file: %s", sf_strerror (sf));

    puts ("writing file...");
    sf_write_float (sf, buffer, n_samples);

    puts ("done; cleanin up");
    destroy_synth (synth);

    return EXIT_SUCCESS;
}
