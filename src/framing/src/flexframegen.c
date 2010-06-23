/*
 * Copyright (c) 2007, 2009 Joseph Gaeddert
 * Copyright (c) 2007, 2009 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// flexframegen.c
//
// flexible frame generator
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <complex.h>

#include "liquid.internal.h"

#define DEBUG_FLEXFRAMEGEN          1
#define DEBUG_FLEXFRAMEGEN_PRINT    0

// default flexframegen properties
static flexframegenprops_s flexframegenprops_default = {
    16,         // rampup_len
    16,         // phasing_len
    0,          // payload_len
    MOD_BPSK,   // mod_scheme
    1,          // mod_bps
    16          // rampdn_len
};

struct flexframegen_s {
    // buffers: preamble (BPSK)
    float complex * ramp_up;            // ramp up sequence
    float complex * phasing;            // phasing pattern sequence
    float complex   pn_sequence[64];    // p/n sequence
    float complex * ramp_dn;            // ramp down sequence

    // header (QPSK)
    // TODO : use packetizer object for this
    modem mod_header;                   // header QPSK modulator
    fec fec_header;
    interleaver intlv_header;
    unsigned char header[15];
    unsigned char header_enc[32];
    unsigned char header_sym[256];
    float complex header_samples[256];

    // payload
    modem mod_payload;
    unsigned char * payload;            // payload data (bytes)
    unsigned char * payload_sym;        // payload symbols (modem input)
    float complex * payload_samples;    // payload samples (modem output)
    unsigned int payload_numalloc;
    unsigned int payload_sym_numalloc;
    unsigned int payload_samples_numalloc;

    // properties
    flexframegenprops_s props;

    unsigned int pnsequence_len;        // p/n sequence length
    unsigned int num_payload_symbols;   // number of paylod symbols
    unsigned int frame_len;             // number of frame symbols
};

flexframegen flexframegen_create(flexframegenprops_s * _props)
{
    flexframegen fg = (flexframegen) malloc(sizeof(struct flexframegen_s));

    unsigned int i;

    // generate pn sequence
    fg->pnsequence_len = 64;
    msequence ms = msequence_create(6);
    for (i=0; i<64; i++)
        fg->pn_sequence[i] = (msequence_advance(ms)) ? 1.0f : -1.0f;
    msequence_destroy(ms);

    // create header objects
    fg->mod_header = modem_create(MOD_BPSK, 1);
    fg->fec_header = fec_create(FEC_HAMMING74, NULL);
    fg->intlv_header = interleaver_create(32, LIQUID_INTERLEAVER_BLOCK);

    // initial memory allocation for payload
    fg->payload = (unsigned char*) malloc(1*sizeof(unsigned char));
    fg->payload_numalloc = 1;
    fg->payload_sym = (unsigned char*) malloc(1*sizeof(unsigned char));
    fg->payload_sym_numalloc = 1;
    fg->payload_samples = (float complex*) malloc(1*sizeof(float complex));
    fg->payload_samples_numalloc = 1;

    // create payload modem (initially bpsk, overridden by properties)
    fg->mod_payload = modem_create(MOD_BPSK, 1);

    // initialize properties
    if (_props != NULL)
        flexframegen_setprops(fg, _props);
    else
        flexframegen_setprops(fg, &flexframegenprops_default);

    flexframegen_configure_payload_buffers(fg);

    return fg;
}

void flexframegen_destroy(flexframegen _fg)
{
    // destroy header objects
    fec_destroy(_fg->fec_header);
    interleaver_destroy(_fg->intlv_header);
    modem_destroy(_fg->mod_header);

    // free internal payload buffers
    free(_fg->payload);
    free(_fg->payload_sym);
    free(_fg->payload_samples);

    // free payload objects
    free(_fg->mod_payload);

    // destroy frame generator
    free(_fg);
}

// get flexframegen properties
//  _fg     :   frame generator object
//  _props  :   frame generator properties structure pointer
void flexframegen_getprops(flexframegen _fg,
                           flexframegenprops_s * _props)
{
    // copy properties structure to output pointer
    memmove(_props, &_fg->props, sizeof(flexframegenprops_s));
}

// set flexframegen properties
//  _fg     :   frame generator object
//  _props  :   frame generator properties structure pointer
void flexframegen_setprops(flexframegen _fg,
                           flexframegenprops_s * _props)
{
    // TODO : flexframegen_setprops() validate input
    if (_props->mod_bps == 0) {
        printf("error: flexframegen_setprops(), modulation depth must be greater than 0\n");
        exit(1);
    }

    // copy properties to internal structure
    memmove(&_fg->props, _props, sizeof(flexframegenprops_s));

    // re-create modem
    modem_destroy(_fg->mod_payload);
    _fg->mod_payload = modem_create(_fg->props.mod_scheme, _fg->props.mod_bps);

    // re-compute payload and frame lengths
    flexframegen_compute_payload_len(_fg);
    flexframegen_compute_frame_len(_fg);

    // reconfigure payload buffers (reallocate as necessary)
    flexframegen_configure_payload_buffers(_fg);
}

// print flexframegen object internals
void flexframegen_print(flexframegen _fg)
{
    printf("flexframegen [%u samples]:\n", _fg->frame_len);
    printf("    ramp up len         :   %u\n", _fg->props.rampup_len);
    printf("    phasing len         :   %u\n", _fg->props.phasing_len);
    printf("    p/n sequence len    :   %u\n", _fg->pnsequence_len);
    printf("    payload len         :   %u bytes\n", _fg->props.payload_len);
    printf("    modulation scheme   :   %u-%s\n",
        1<<_fg->props.mod_bps,
        modulation_scheme_str[_fg->props.mod_scheme]);
    printf("    num payload symbols :   %u\n", _fg->num_payload_symbols);
    printf("    ramp dn len         :   %u\n", _fg->props.rampdn_len);
}

// get frame length (number of samples)
unsigned int flexframegen_getframelen(flexframegen _fg)
{
    return _fg->frame_len;
}

// exectue frame generator (create the frame)
//  _fg         :   frame generator object
//  _header     :   8-byte header
//  _payload    :   variable payload buffer (configured by setprops method)
//  _y          :   output frame symbols [size: frame_len x 1]
void flexframegen_execute(flexframegen _fg,
                          unsigned char * _header,
                          unsigned char * _payload,
                          float complex * _y)
{
    // write frame
    // TODO: write frame in pieces so as not to require excessively large output buffer
    unsigned int i, n=0;

    // ramp up
    for (i=0; i<_fg->props.rampup_len; i++) {
        _y[n] = ((n%2) ? 1.0f : -1.0f) * 0.5f * (1.0f - cos(M_PI*(float)(i)/(float)(_fg->props.rampup_len)));
        //_y[n++] = ((i%2) ? 1.0f : -1.0f) * kaiser(i, 2*_fg->props.rampup_len, 10.0f, 0.0f);
        //_y[n++] = ((i%2) ? 1.0f : -1.0f) * ((float)(i) / (float)(_fg->props.rampup_len));
        n++;
    }

    // phasing pattern
    for (i=0; i<_fg->props.phasing_len; i++) {
        _y[n] = (n%2) ? 1.0f : -1.0f;
        n++;
    }

    // p/n sequence
    for (i=0; i<64; i++)
        _y[n++] = _fg->pn_sequence[i];

    // copy and encode header
    memmove(_fg->header, _header, 8*sizeof(unsigned char));
    flexframegen_encode_header(_fg);
    flexframegen_modulate_header(_fg);
    memmove(&_y[n], _fg->header_samples, 256*sizeof(float complex));
    n += 256;

    // copy and encode payload
    memmove(_fg->payload, _payload, _fg->props.payload_len);
    flexframegen_modulate_payload(_fg);
    memmove(&_y[n], _fg->payload_samples, (_fg->num_payload_symbols)*sizeof(float complex));
    n += _fg->num_payload_symbols;

    // ramp down
    for (i=0; i<_fg->props.rampdn_len; i++)
        _y[n++] = ((i%2) ? 1.0f : -1.0f) * 0.5f * (1.0f + cos(M_PI*(float)(i)/(float)(_fg->props.rampup_len)));
        //_y[n++] = ((i%2) ? 1.0f : -1.0f) * (1.0f - kaiser(i, 2*_fg->props.rampdn_len, 10.0f, 0.0f));
        //_y[n++] = ((i%2) ? 1.0f : -1.0f) * ((float)(_fg->props.rampdn_len-i) / (float)(_fg->props.rampdn_len));

    assert(n == _fg->frame_len);
}

//
// internal
//

// compute length of payload (number of symbols)
void flexframegen_compute_payload_len(flexframegen _fg)
{
    // num_payload_symbols = ceil( payload_len / mod_bps )

    // compute integer division, keeping track of remainder
    div_t d = div(8*_fg->props.payload_len, _fg->props.mod_bps);

    // extend number of payload symbols if remainder is present
    _fg->num_payload_symbols = d.quot + (d.rem ? 1 : 0);
}

// compute length of frame (number of symbols)
void flexframegen_compute_frame_len(flexframegen _fg)
{
    // compute payload length
    flexframegen_compute_payload_len(_fg);

    _fg->frame_len = 0;

    _fg->frame_len += _fg->props.rampup_len;    // ramp up length
    _fg->frame_len += _fg->props.phasing_len;   // phasing length
    _fg->frame_len += _fg->pnsequence_len;      // p/n sequence length
    _fg->frame_len += 256;                      // header length
    _fg->frame_len += _fg->num_payload_symbols; // payload length
    _fg->frame_len += _fg->props.rampdn_len;    // ramp down length
}

// configures payload buffers, reallocating memory if necessary
void flexframegen_configure_payload_buffers(flexframegen _fg)
{
    // compute frame length, including payload length
    flexframegen_compute_frame_len(_fg);

    // payload data (bytes)
    if (_fg->payload_numalloc != _fg->props.payload_len) {
        _fg->payload = (unsigned char*) realloc(_fg->payload, _fg->props.payload_len);
        _fg->payload_numalloc = _fg->props.payload_len;
        //printf("reallocating payload (payload data) : %u\n", _fg->payload_numalloc);
    }

    // payload symbols (modem input)
    if (_fg->payload_sym_numalloc != _fg->num_payload_symbols) {
        _fg->payload_sym = (unsigned char*) realloc(_fg->payload_sym, _fg->num_payload_symbols);
        _fg->payload_sym_numalloc = _fg->num_payload_symbols;
        //printf("reallocating payload_sym (payload symbols) : %u\n", _fg->payload_sym_numalloc);
    }

    // payload symbols (modem output)
    if (_fg->payload_samples_numalloc != _fg->num_payload_symbols) {
        _fg->payload_samples = (float complex*) realloc(_fg->payload_samples, _fg->num_payload_symbols*sizeof(float complex));
        _fg->payload_samples_numalloc = _fg->num_payload_symbols;
        //printf("reallocating payload_samples (modulated payload symbols) : %u\n",
        //        _fg->payload_samples_numalloc);
    }

}

// encode header of flexframe
void flexframegen_encode_header(flexframegen _fg)
{
    // first 8 bytes of header are user-defined

    // add payload length
    _fg->header[8] = (_fg->props.payload_len >> 8) & 0xff;
    _fg->header[9] = (_fg->props.payload_len     ) & 0xff;

    // add modulation scheme/depth (pack into single byte)
    _fg->header[10]  = (_fg->props.mod_scheme << 4) & 0xf0;
    _fg->header[10] |= (_fg->props.mod_bps) & 0x0f;

    // compute crc
    unsigned int header_key = crc32_generate_key(_fg->header, 11);
    _fg->header[11] = (header_key >> 24) & 0xff;
    _fg->header[12] = (header_key >> 16) & 0xff;
    _fg->header[13] = (header_key >>  8) & 0xff;
    _fg->header[14] = (header_key      ) & 0xff;

    // scramble header
    scramble_data(_fg->header, 15);

    // run encoder
    fec_encode(_fg->fec_header, 15, _fg->header, _fg->header_enc);
#if !defined HAVE_FEC_H || HAVE_FEC_H==0 || LIQUID_FLEXFRAME_FORCE_H74==1
    // append 2 bytes of random data to end of header for Hamming(7,4) code
    _fg->header_enc[30] = 0xa7;
    _fg->header_enc[31] = 0x9e;
#endif

    // interleave header bits
    interleaver_encode(_fg->intlv_header, _fg->header_enc, _fg->header_enc);

#if DEBUG_FLEXFRAMEGEN_PRINT
    // print results
    printf("flexframegen_encode_header():\n");
    printf("    mod scheme  : %u\n", _fg->props.mod_scheme);
    printf("    mod depth   : %u\n", _fg->props.mod_bps);
    printf("    payload len : %u\n", _fg->props.payload_len);
    printf("    header key  : 0x%.8x\n", header_key);

    printf("    user data   :");
    for (i=0; i<8; i++)
        printf(" %.2x", _user_header[i]);
    printf("\n");
#endif
}

// modulate header into QPSK symbols
void flexframegen_modulate_header(flexframegen _fg)
{
    unsigned int i;

    // unpack header symbols
    for (i=0; i<32; i++) {
        _fg->header_sym[8*i+0] = (_fg->header_enc[i] >> 7) & 0x01;
        _fg->header_sym[8*i+1] = (_fg->header_enc[i] >> 6) & 0x01;
        _fg->header_sym[8*i+2] = (_fg->header_enc[i] >> 5) & 0x01;
        _fg->header_sym[8*i+3] = (_fg->header_enc[i] >> 4) & 0x01;
        _fg->header_sym[8*i+4] = (_fg->header_enc[i] >> 3) & 0x01;
        _fg->header_sym[8*i+5] = (_fg->header_enc[i] >> 2) & 0x01;
        _fg->header_sym[8*i+6] = (_fg->header_enc[i] >> 1) & 0x01;
        _fg->header_sym[8*i+7] = (_fg->header_enc[i]     ) & 0x01;
    }

    // modulate symbols
    for (i=0; i<256; i++)
        modem_modulate(_fg->mod_header, _fg->header_sym[i], &_fg->header_samples[i]);
}

// modulate payload into symbols using user-defined modem
void flexframegen_modulate_payload(flexframegen _fg)
{
    // clear payload
    memset(_fg->payload_sym, 0x00, _fg->props.payload_len);

    // repack 8-bit payload bytes into 'mod_bps'-bit payload symbols
    unsigned int num_written;
    repack_bytes(_fg->payload,     8, _fg->props.payload_len,
                 _fg->payload_sym,  _fg->props.mod_bps,   _fg->num_payload_symbols,
                 &num_written);

    // modulate symbols
    unsigned int i;
    for (i=0; i<_fg->num_payload_symbols; i++)
        modem_modulate(_fg->mod_payload, _fg->payload_sym[i], &_fg->payload_samples[i]);
}

