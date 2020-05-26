#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#define MAX_SAMPLE_DELAY 4096
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
    synth->noise = 0.01;
    synth->stiffness_nonlinear_coefficient = 10;
    synth->stiffness_nonlinear_degree = 5;
    synth->damping_nonlinear_coefficient = 5;
    synth->damping_nonlinear_degree = 1;
    synth->damping = 0.1;
    synth->lips_reflection = 0.5;
    synth->lips_coupling = 1;
    synth->max_input_pressure = 1.05;

    synth->lips_tension_scaling = 0;
    synth->blowing_pressure = 1;

    synth->rate = rate;
    synth_set_fundamental (synth, 164.81 / 2);

    synth->vibrato_rate = 5;
    synth->vibrato_depth = 0.01;
    synth->tremolo_rate = 2;
    synth->tremolo_depth = 0.01;

    return synth;
}

void destroy_synth (synth_t *synth) {

    free (synth);
}



// MAIN N AUDIO INTERFACE N STUFF

typedef struct jack_context_t {

    jack_client_t *client;
    jack_port_t *input_port;
    jack_port_t *output_port;
    synth_t *synth;

} jack_context_t;

void init_synth (jack_context_t *context, double rate) {

    if (context->synth != NULL) {

        puts ("destroying old synth");
        destroy_synth (context->synth);
    }

    printf ("initing synth with rate %lf\n", rate);
    context->synth = create_synth (rate);
}

int jack_process (jack_nframes_t n_frames, void *arg) {

    jack_context_t *context = (jack_context_t *) arg;

	jack_default_audio_sample_t *output =
        (jack_default_audio_sample_t *) jack_port_get_buffer (context->output_port, n_frames);
	void *midi_buffer = jack_port_get_buffer (input_port, nframes);
	jack_nframes_t n_events = jack_midi_get_event_count (midi_buffer);

    // process midi inputs
    // TODO: make them not batched into audio buffer?
    for (int i = 0; i < n_events; i++) {

    	jack_midi_event_t event;
        jack_midi_event_get (&event, midi_buffer, i);
        jack_midi_data_t *buffer = event.buffer;
        int command = buffer[0] & 0xf0;
        int channel = buffer[0] & 0x0f;

        printf ("got midi message 0x%x on channel 0x%x\n", command, channel);
    }

    // process audio out
    for (int i = 0; i < n_frames; i++)
        output[i] = synth_process (context->synth);

	return 0;
}

int jack_set_rate (jack_nframes_t rate, void *arg) {

    printf ("jack has update the rate to %d\n", rate);
    init_synth (arg, rate);
	return 0;
}

void jack_shutdown (void *arg) {

    fprintf (stderr, "jack SHUTDOWN on us!!\n");
	exit (EXIT_FAILURE);
}

int main (int argc, char **argv) {

    srand (SRAND_SEED);

    jack_context_t context;
    context.synth = NULL;

	if ((context.client = jack_client_open ("nancetto", JackNullOption, NULL)) == 0) {
		fprintf (stderr, "could not connect to jack server\n");
		return EXIT_FAILURE;
	}

    //init_synth (&context, jack_get_sample_rate (context.client));
	jack_set_process_callback (context.client, jack_process, &context);
	jack_set_sample_rate_callback (context.client, jack_set_rate, &context);
	jack_on_shutdown (context.client, jack_shutdown, &context);

	context.input_port = jack_port_register (context.client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
	context.output_port = jack_port_register (context.client, "audio_out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

	if (jack_activate (context.client)) {
		fprintf (stderr, "cannot activate jack client");
		return EXIT_FAILURE;
	}

	while (1)
		sleep(1);

    destroy_synth (context.synth);
	jack_client_close (context.client);
    return EXIT_SUCCESS;
}
