#include <math.h>
#include <rtl-sdr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const int DEFAULT_LEN_SECS = 30;
const int WAV_SAMPLE_RATE = 44100;
const int DECIMATION_FACTOR = 32;
const int SDR_SAMPLE_RATE = DECIMATION_FACTOR * WAV_SAMPLE_RATE;
const int FREQ_DEVIATION = 75000; // I think commercial FM radio is supposed to have max deviation of 75 kHz

rtlsdr_dev_t *DEVICE = NULL;

typedef struct {
    unsigned char *buf;
    int len;
    int i;
} IQ_DATA;

#pragma pack(1)
typedef struct {
    char riff[4]; // "RIFF"
    uint32_t overall_size; // File size - 8 bytes
    char wave[4]; // "WAVE"
    char fmt_chunk_marker[4]; // "fmt "
    uint32_t length_of_fmt; // Length of format data
    uint16_t format_type; // Format type (1 for PCM)
    uint16_t channels; // Number of channels
    uint32_t sample_rate; // Sample rate
    uint32_t byterate; // SampleRate * NumChannels * BitsPerSample/8
    uint16_t block_align; // NumChannels * BitsPerSample/8
    uint16_t bits_per_sample; // Bits per sample
    char data_chunk_header[4]; // "data"
    uint32_t data_size; // Number of bytes in the data
} WAV_HEADER;
#pragma pack()

static void sigint_handler(int sig) {
    fprintf(stderr, "Got SIGINT\n");
    if (DEVICE != NULL) {
        rtlsdr_cancel_async(DEVICE);
    }
}

static void rtlsdr_cb(unsigned char *iq_buf, uint32_t len, void *ctx) {
    IQ_DATA *iq = ctx;
    for (int i = 0; i < len; i++) {
        if (iq->i >= iq->len) {
            rtlsdr_cancel_async(DEVICE);
            return;
        }
        iq->buf[iq->i++] = iq_buf[i];
    }
}

int rtlsdr_listen(IQ_DATA *iq, int frequency) {
    int r = rtlsdr_open(&DEVICE, 0);
    if (r < 0) {
        fprintf(stderr, "RTLSDR failed to open device\n");
        return -1;
    }
    r = rtlsdr_set_center_freq(DEVICE, frequency) |
        rtlsdr_set_sample_rate(DEVICE, SDR_SAMPLE_RATE) |
        rtlsdr_set_tuner_gain_mode(DEVICE, 0) | rtlsdr_reset_buffer(DEVICE);
    if (r < 0) {
        fprintf(stderr, "RTLSDR failed to set parameters\n");
        return -1;
    }
    r = rtlsdr_read_async(DEVICE, rtlsdr_cb, iq, 15, 0x40000) |
        rtlsdr_close(DEVICE);
    if (r < 0) {
        fprintf(stderr, "RTLSDR failed to read samples\n");
        return -1;
    }
    return 0;
}

static void fm_demodulate(IQ_DATA *iq, float *samples) {
    float last_angle = 0;
    for (int i = 0; i < iq->len / (2 * DECIMATION_FACTOR); i += 1) {
        float freq_avg = 0;
        for (int j = 0; j < DECIMATION_FACTOR; j++) {
            float I = (float) iq->buf[i * 2 * DECIMATION_FACTOR + 2 * j] - 127.5;
            float Q = (float) iq->buf[i * 2 * DECIMATION_FACTOR + 2 * j + 1] - 127.5;
            float angle = atan2f(Q, I);
            if (angle - last_angle > M_PI) {
                last_angle += 2 * M_PI;
            } else if (angle - last_angle < -M_PI) {
                last_angle -= 2 * M_PI;
            }
            float freq = SDR_SAMPLE_RATE * (angle - last_angle) / (2 * M_PI);
            freq_avg += freq;
            last_angle = angle;
        }
        freq_avg /= DECIMATION_FACTOR;
        samples[i] = freq_avg / FREQ_DEVIATION;
    }
}

static void fm_demodulate_fast(IQ_DATA *iq, float *samples) {
    // when I**2 + Q**2 = 1 (this should always be the case for SDR since sin2(x) + cos2(x) = 1)
    // then freq = d_phase/dt = I * d_Q/dt - Q * d_I/dt (by some trig identities + calculus)
    // this doesn't seem to perform quite as well as the atan2 method, but it's faster
    // can probably get on par with some effort
    for (int i = 0; i < iq->len / (2 * DECIMATION_FACTOR); i += 1) {
        float freq_avg = 0;
        for (int j = 0; j < DECIMATION_FACTOR; j++) {
            int i_idx = i * 2 * DECIMATION_FACTOR + 2 * j;
            int q_idx = i * 2 * DECIMATION_FACTOR + 2 * j + 1;
            int I = (int) iq->buf[i_idx] - 127;
            int I_PREV = (int) iq->buf[i_idx - 2] - 127;
            int Q = (int) iq->buf[q_idx] - 127;
            int Q_PREV = (int) iq->buf[q_idx - 2] - 127;
            float freq = I * (Q - Q_PREV) - Q * (I - I_PREV);
            freq_avg += freq;
        }
        freq_avg /= DECIMATION_FACTOR;
        samples[i] = freq_avg / FREQ_DEVIATION;
    }
}

int wav_write(float *samples, int len) {
    char filename[64];
    snprintf(filename, sizeof(filename), "fm_%d.wav", (int)time(NULL));
    FILE *wav_file = fopen(filename, "wb");
    if (!wav_file) {
        fprintf(stderr, "Unable to open file for writing\n");
        return -1;
    }

    WAV_HEADER header;
    strncpy(header.riff, "RIFF", 4);
    strncpy(header.wave, "WAVE", 4);
    strncpy(header.fmt_chunk_marker, "fmt ", 4);
    strncpy(header.data_chunk_header, "data", 4);

    header.length_of_fmt = 16;
    header.format_type = 1;
    header.channels = 1;
    header.sample_rate = WAV_SAMPLE_RATE;
    header.bits_per_sample = 16;
    header.byterate =
            header.sample_rate * header.channels * header.bits_per_sample / 8;
    header.block_align = header.channels * header.bits_per_sample / 8;
    header.data_size = len * header.channels * header.bits_per_sample / 8;
    header.overall_size = 4 + (8 + header.length_of_fmt) + (8 + header.data_size);

    fwrite(&header, sizeof(WAV_HEADER), 1, wav_file);

    int16_t *pcm_data = malloc(len * sizeof(int16_t));
    for (int i = 0; i < len; i++) {
        pcm_data[i] = (int16_t) (samples[i] * 32767); // Convert float to 16-bit PCM
    }

    fwrite(pcm_data, sizeof(int16_t), len, wav_file);

    fclose(wav_file);
    free(pcm_data);
    fprintf(stderr, "Wrote %s\n", filename);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, sigint_handler);
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <frequency> [length]\n"
                "\n"
                "Parameters:\n"
                "  frequency    (MHz)  - Required. The frequency to tune into.\n"
                "  length       (sec)  - Optional. The length of the recording. Defaults to %d seconds.\n"
                "\n"
                "Description:\n"
                "  This utility uses an RTL-SDR device to listen on the given frequency,\n"
                "  FM demodulate the signal, and store a WAV file containing the recording.\n",
                argv[0], DEFAULT_LEN_SECS);
        return 1;
    }
    int freq_hz = atof(argv[1]) * 1000000;
    int len_secs = argc > 2 ? atoi(argv[2]) : DEFAULT_LEN_SECS;
    IQ_DATA iq = {.buf = NULL, .len = len_secs * SDR_SAMPLE_RATE * 2, .i = 0};
    iq.buf = malloc(iq.len * sizeof(unsigned char));
    float *samples = malloc(len_secs * WAV_SAMPLE_RATE * sizeof(float));
    if (iq.buf == NULL || samples == NULL) {
        fprintf(stderr, "Memory allocation error\n");
        return 1;
    }
    fprintf(stderr, "Listening...\n");
    int r = rtlsdr_listen(&iq, freq_hz);
    if (r < 0) {
        return 1;
    }
    fprintf(stderr, "Demodulating...\n");
    fm_demodulate(&iq, samples);
    fprintf(stderr, "Writing...\n");
    r = wav_write(samples, len_secs * WAV_SAMPLE_RATE);
    if (r < 0) {
        return 2;
    }
    free(iq.buf);
    free(samples);
    return 0;
}
