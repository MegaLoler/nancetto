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
    double x_clip;

    // live paramaters
    double target_blowing_pressure;
    double blowing_pressure;
    double vibrato_rate;
    double vibrato_depth;
    double tremolo_rate;
    double tremolo_depth;

    // flare filter parameters
    double filter_a;
    double filter_c;
    int filter_n;

    // private
    double time;
    double rate;
    double frequency; // target
    double fundamental; // current bore frequency
    double stiffness;
    double lips_tension_scaling; // scales actual lips tension on log scale

    int n_delay_samples;
    int i_delay;
    double fractional_delay_amount;
    double delay_line[MAX_SAMPLE_DELAY];
    double delay_output;

    double x;   // mass displacement
    double v;   // mass velocity

    double filter_last;
    double last_out;

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

double flare_impulse_response (double a, double c, int n) {

    return a * exp (c * n);
}

double flare_filter (synth_t *synth, double input) {

    // TODO: figure this out lolo
//    // convolve input with flare impulse response
//    double output = 0;
//    for (int i = 0; i < n; i++)
//        output += flare_impulse_response (a, c, i) * input;

    // TODO: temp 
    double output = synth->filter_last + (input - synth->filter_last) * 0.1;
    synth->filter_last = output;

    return output;
}

double synth_process (synth_t *synth, double external_feedback) {

    // TODO: optimize low frequency changes out into a "control rate" function
    // smooth transitions...
    synth->blowing_pressure +=
        0.0025 * (synth->target_blowing_pressure - synth->blowing_pressure);

    synth_set_fundamental (synth, 
        synth->fundamental + 0.005 * (synth->frequency - synth->fundamental));

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
    double delay_input = feedback + reed_output + external_feedback;
    double filter = flare_filter (synth, delay_input);
    synth_run_delay (synth, filter);
    double output = (delay_input - filter) * synth->gain;

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
    synth->x = fmin (synth->x_clip, fmax (-synth->x_clip, synth->x));

    synth->time += 1 / synth->rate;
    synth->last_out = output;

    // TODO: dc blocker...
    return output;
}

synth_t *create_synth (double rate) {

    synth_t *synth = (synth_t *) malloc (sizeof (synth_t));
    memset (synth, 0, sizeof (synth_t));

    synth->gain = 0.5;
    synth->noise = 0;//0.01;
    synth->stiffness_nonlinear_coefficient = 10;
    synth->stiffness_nonlinear_degree = 5;
    synth->damping_nonlinear_coefficient = 5;
    synth->damping_nonlinear_degree = 1;
    synth->damping = 0.1;
    synth->lips_reflection = 0.5;
    synth->lips_coupling = 0.944882; // 1;
    synth->max_input_pressure = 1.125;
    synth->x_clip = 1;

    synth->filter_a = 0.01;
    synth->filter_c = 0.1;
    synth->filter_n = 10;

    //synth->lips_tension_scaling = 2.102362;
    synth->lips_tension_scaling = 1.346457;
    synth->target_blowing_pressure = 0;
    synth->blowing_pressure = 0;

    synth->rate = rate;
    synth->frequency = 164.81 / 2;
    synth_set_fundamental (synth, synth->frequency);

    synth->vibrato_rate = 5;
    synth->vibrato_depth = 0;//0.01;
    synth->tremolo_rate = 2;
    synth->tremolo_depth = 0.007874;//0.01;

    return synth;
}

void destroy_synth (synth_t *synth) {

    free (synth);
}



// MAIN N AUDIO INTERFACE N STUFF

#define N_VOICES 4

typedef struct jack_context_t {

    jack_client_t *client;
    jack_port_t *input_port;
    jack_port_t *output_port;
    synth_t *synths[N_VOICES];
    int notes[N_VOICES];

} jack_context_t;

void init_synths (jack_context_t *context, double rate) {

    for (int i = 0; i < N_VOICES; i++) {

        if (context->synths[i] != NULL) {

            puts ("destroying old synth");
            destroy_synth (context->synths[i]);
        }

        printf ("initing synth #%d with rate %lf\n", i, rate);
        context->synths[i] = create_synth (rate);
    }
}

synth_t *allocate_voice (jack_context_t *context, int note) {

    printf ("allocating voice to note %d\n", note);

    for (int i = 0; i < N_VOICES; i++) {
        if (context->notes[i] == 0) {
            printf ("assigning note %d to voice %d\n", note, i);
            context->notes[i] = note;
            return context->synths[i];
        }
    }
    puts ("NO FREE VOICES");
    // not any free voices?
    return NULL;
}

synth_t *unallocate_voice (jack_context_t *context, int note) {

    printf ("unallocating voice from note %d\n", note);

    for (int i = 0; i < N_VOICES; i++) {
        if (context->notes[i] == note) {
            printf ("UNassigning note %d from voice %d\n", note, i);
            context->notes[i] = 0;
            return context->synths[i];
        }
    }
    puts ("no voices had that note");
    // not any free voices?
    return NULL;
}

void note_on (synth_t *synth, int note, int velocity) {

    if (synth == NULL)
        return;

    printf ("NOTE ON  %d, velocity=%d\n", note, velocity);

    //double normalized_velocity = velocity / 127.0;

    synth->frequency = 440 * pow (2.0, ((note - 69) / 12.0));
    //synth->target_blowing_pressure = normalized_velocity;
    synth->target_blowing_pressure = 1;
}

void note_off (synth_t *synth, int note, int velocity) {

    if (synth == NULL)
        return;

    printf ("NOTE OFF %d, velocity=%d\n", note, velocity);
    synth->target_blowing_pressure = 0;
}

void cc (synth_t *synth, int controller, int value) {

    printf ("CC       %d, value=%d\n", controller, value);

    double normalized_value = value / 127.0;

    switch (controller) {

        case 21: // breath control TODO: actual midi breath controller cc
            synth->target_blowing_pressure =
            synth->blowing_pressure = normalized_value *
            synth->max_input_pressure;
            break;

        case 22: // lips tension
            synth_set_lips_tension_scaling (synth, normalized_value * 3);
            printf ("tension scaling: %lf\n", synth->lips_tension_scaling);
            break;

        case 23:
            synth->stiffness_nonlinear_coefficient = normalized_value * 100;
            printf ("stiffness nonlinear coefficient: %lf\n",
            synth->stiffness_nonlinear_coefficient);
            break;

        case 24:
            synth->stiffness_nonlinear_degree = value;
            printf ("stiffness nonlinear degree: %lf\n", synth->stiffness_nonlinear_degree);
            break;

        case 25:
            synth->lips_coupling = normalized_value * 2;
            printf ("coupling: %lf\n", synth->lips_coupling);
            break;

        case 26:
            synth->vibrato_depth = normalized_value / 2;
            printf ("vibrato: %lf\n", synth->vibrato_depth);
            break;

        case 27:
            synth->tremolo_depth = normalized_value;
            printf ("tremolo: %lf\n", synth->tremolo_depth);
            break;
    }
}

int jack_process (jack_nframes_t n_frames, void *arg) {

    jack_context_t *context = (jack_context_t *) arg;

	jack_default_audio_sample_t *output =
        (jack_default_audio_sample_t *) jack_port_get_buffer (context->output_port, n_frames);
	void *midi_buffer = jack_port_get_buffer (context->input_port, n_frames);
	jack_nframes_t n_events = jack_midi_get_event_count (midi_buffer);

    // process midi inputs
    // TODO: make them not batched into audio buffer?
    for (int i = 0; i < n_events; i++) {

    	jack_midi_event_t event;
        jack_midi_event_get (&event, midi_buffer, i);
        jack_midi_data_t *buffer = event.buffer;
        int command = buffer[0] & 0xf0;
        //int channel = buffer[0] & 0x0f;

        //printf ("got midi message 0x%x on channel 0x%x\n", command, channel);

        switch (command) {

            case 0x90: // note on
                note_on (allocate_voice (context, buffer[1]), buffer[1], buffer[2]);
                break;

            case 0x80: // note off
                note_off (unallocate_voice (context, buffer[1]), buffer[1], buffer[2]);
                break;

            case 0xb0: // cc
                for (int j = 0; j < N_VOICES; j++)
                    cc (context->synths[j], buffer[1], buffer[2]);
                break;
        }
    }

    // process audio out
    for (int i = 0; i < n_frames; i++) {
        output[i] = 0;

        // TODO: have separate jack outputs for the different voices
        for (int j = 0; j < N_VOICES; j++)
            output[i] += synth_process (context->synths[j], 0);

        // clipping....
        output[i] = fmin (1, fmax (-1, output[i]));
    }

    return 0;
}

int jack_set_rate (jack_nframes_t rate, void *arg) {

    printf ("jack has update the rate to %d\n", rate);
    init_synths (arg, rate);
	return 0;
}

void jack_shutdown (void *arg) {

    fprintf (stderr, "jack SHUTDOWN on us!!\n");
	exit (EXIT_FAILURE);
}

int main (int argc, char **argv) {

    srand (SRAND_SEED);

    jack_context_t context;
    memset (&context, 0, sizeof (jack_context_t));

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

    for (int i = 0; i < N_VOICES; i++)
        destroy_synth (context.synths[i]);

    jack_client_close (context.client);
    return EXIT_SUCCESS;
}
