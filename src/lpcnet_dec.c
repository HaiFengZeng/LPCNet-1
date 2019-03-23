/*
   lpcnet_dec.c
   Feb 2019

   LPCNet to bit stream decoder, converts fully quantised bit stream
   on stdin (in 1 bit per char format) to 16 kHz signed 16 bit speech
   samples on stdout.
*/

/* Copyright (c) 2018 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <math.h>
#include <stdio.h>
#include <getopt.h>

#include "arch.h"
#include "freq.h"
#include "lpcnet_quant.h"
// NB_FEATURES has a different value in lpcnet.h, need to reconcile some time
#undef NB_FEATURES 
#include "lpcnet.h"

// Two sorts of VQs available
extern int   pred_num_stages;
extern float pred_vq[MAX_STAGES*NB_BANDS*MAX_ENTRIES];
extern int   pred_m[MAX_STAGES];
extern int   direct_split_num_stages;
extern float direct_split_vq[MAX_STAGES*NB_BANDS*MAX_ENTRIES];
extern int   direct_split_m[MAX_STAGES];

int main(int argc, char **argv) {
    FILE *fin, *fout;

    /* quantiser defaults */

    int   dec = 3;
    float pred = 0.9;    
    int   mbest_survivors = 5;
    float weight = 1.0/sqrt(NB_BANDS);    
    int   pitch_bits = 6;
    float ber = 0.0;
    int   num_stages = pred_num_stages;
    int   *m = pred_m;
    float *vq = pred_vq;
    int   logmag = 0;
    
    /* quantiser options */
    
    static struct option long_options[] = {
        {"ber",         required_argument, 0, 'b'},
        {"decimate",    required_argument, 0, 'd'},
        {"numstages",   required_argument, 0, 'n'},
        {"pitchquant",  required_argument, 0, 'o'},
        {"pred",        required_argument, 0, 'p'},
        {"directsplit", no_argument,       0, 's'},
        {"verbose",     no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int   c;
    int opt_index = 0;

    while ((c = getopt_long (argc, argv, "b:d:n:o:p:sv", long_options, &opt_index)) != -1) {
        switch (c) {
        case 'b':
            ber = atof(optarg);
            fprintf(stderr, "BER = %f\n", ber);
            break;
        case 'd':
            dec = atoi(optarg);
            fprintf(stderr, "dec = %d\n", dec);
            break;
        case 'n':
            num_stages = atoi(optarg);
            fprintf(stderr, "%d VQ stages\n",  num_stages);
            break;
        case 'o':
            pitch_bits = atoi(optarg);
            fprintf(stderr, "pitch quantised to %d bits\n",  pitch_bits);
            break;
        case 'p':
            pred = atof(optarg);
            fprintf(stderr, "pred = %f\n", pred);
            break;
        case 's':
            m = direct_split_m; vq = direct_split_vq; pred = 0.0; logmag = 1; weight = 1.0;
            fprintf(stderr, "split VQ\n");
            break;
        case 'v':
            lpcnet_verbose = 1;
            break;
        default:
            fprintf(stderr,"usage: %s [Options]:\n", argv[0]);
            fprintf(stderr,"  [-b --ber BER]\n");
            fprintf(stderr,"  [-d --decimation 1/2/3...]\n");
            fprintf(stderr,"  [-n --numstages]\n  [-o --pitchbits nBits]\n");
            fprintf(stderr,"  [-p --pred predCoff]\n");
            fprintf(stderr,"  [-s --split]\n");
            fprintf(stderr,"  [-v --verbose]\n");
            exit(1);
        }
    }

    LPCNET_QUANT *q = lpcnet_quant_create(num_stages, m, vq);
    q->weight = weight; q->pred = pred; q->mbest = mbest_survivors;
    q->pitch_bits = pitch_bits; q->dec = dec;
    lpcnet_quant_compute_bits_per_frame(q);
    
    fprintf(stderr, "dec: %d pred: %3.2f num_stages: %d mbest: %d bits_per_frame: %d frame: %2d ms bit_rate: %5.2f bits/s",
            q->dec, q->pred, q->num_stages, q->mbest, q->bits_per_frame, dec*10, (float)q->bits_per_frame/(dec*0.01));
    fprintf(stderr, "\n");

    char frame[q->bits_per_frame];
    int bits_read = 0;

    LPCNetState *net = lpcnet_create();

    fin = stdin;
    fout = stdout;
    int nbits = 0, nerrs = 0, i;

    do {
        float in_features[NB_TOTAL_FEATURES];
        float features[NB_TOTAL_FEATURES];
        short pcm[FRAME_SIZE];
        if ((q->f % q->dec) == 0) {
            bits_read = fread(frame, sizeof(char), q->bits_per_frame, fin);
            nbits += bits_read;
            if (ber != 0.0) {
                int i;
                for(i=0; i<q->bits_per_frame; i++) {
                    float r = (float)rand()/RAND_MAX;
                    if (r < ber) {
                        frame[i] = (frame[i] ^ 1) & 0x1;
                        nerrs++;
                    }
                }
            }
            
        }
        lpcnet_frame_to_features(q, frame, in_features);
        /* optionally log magnitudes convert back to cepstrals */
        if (logmag) {
            float tmp[NB_BANDS];
            dct(tmp, in_features);
            for(i=0; i<NB_BANDS; i++) in_features[i] = tmp[i];
       }
       
        RNN_COPY(features, in_features, NB_TOTAL_FEATURES);
        RNN_CLEAR(&features[18], 18);
        lpcnet_synthesize(net, pcm, features, FRAME_SIZE);
        fwrite(pcm, sizeof(pcm[0]), FRAME_SIZE, fout);
        if (fout == stdout) fflush(stdout);
    } while(bits_read != 0);
    
    fclose(fin);
    fclose(fout);
    lpcnet_destroy(net); lpcnet_quant_destroy(q);

    if (ber != 0.0)
        fprintf(stderr, "nbits: %d nerr: %d BER: %4.3f\n", nbits, nerrs, (float)nerrs/nbits);
    return 0;
}
