/* Chibi XM Play  - Copyright (c) 2007, Juan Linietsky */
/*

License:

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
    CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
    EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
    PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
    PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
    LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
    NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/



#ifndef CHIBI_XM_PLAY_H
#define CHIBI_XM_PLAY_H

#ifdef XM_CPU16


#else

typedef unsigned char xm_u8;
typedef unsigned short xm_u16;
typedef unsigned int xm_u32;

typedef signed char xm_s8;
typedef signed short xm_s16;
typedef signed int xm_s32;

/* override with wathever makes your compiler happy */
#ifndef xm_s64
typedef long long xm_s64;
#endif

#endif

typedef xm_u8 xm_bool;
#define xm_true 1
#define xm_false 0

/** MIXER **/

typedef enum {

	XM_CONSTANT_MAX_NOTES=96,
	XM_CONSTANT_MAX_PATTERNS=255,
	XM_CONSTANT_MAX_INSTRUMENTS=255,
	XM_CONSTANT_MAX_ENVELOPE_POINTS=24,
	XM_CONSTANT_MAX_SAMPLES_PER_INSTRUMENT=16,
	XM_CONSTANT_MAX_ORDERS=256,
	XM_INVALID_SAMPLE_ID=-1

} XM_Constants;

/** AUDIO LOCK **/


typedef struct {

	void (*lock)(); /** Lock the Mixer */
	void (*unlock)(); /** Lock the Mixer */

} XM_AudioLock;

void xm_set_audio_lock( XM_AudioLock * p_audio_lock );
XM_AudioLock * xm_get_audio_lock();

/** MIXER **/

typedef enum {

	XM_SAMPLE_FORMAT_PCM8, /* signed 8-bits */
	XM_SAMPLE_FORMAT_PCM16, /* signed 16-bits */
	XM_SAMPLE_FORMAT_IMA_ADPCM, /* ima-adpcm */
	XM_SAMPLE_FORMAT_CUSTOM /* Custom format, XM_Mixer should support this */

} XM_SampleFormat;


typedef enum {

	XM_LOOP_DISABLED,
	XM_LOOP_FORWARD,
	XM_LOOP_PING_PONG
} XM_LoopType;

/* heh, should be restrictions, not features... */
typedef enum {
	XM_MIXER_FEATURE_NO_PINGPONG_LOOPS=(1<<8), /** Mixer can't do pingpong loops, loader will alloc extra memory for them */
	XM_MIXER_FEATURE_NEEDS_END_PADDING=(1<<9), /** Mixer needs extra (zeroed) space at the end of the sample for interpolation to work */
	XM_MIXER_FEATURE_MAX_VOICES_MASK=0xFF /** Maximum amount of simultaneous voices this mixer can do */

} XM_MixerFeaturesMask;

typedef struct {

	void *data; /* unused if null */
	XM_SampleFormat format;
	XM_LoopType loop_type;
	xm_u32 loop_begin;  /* position in audio frames */
	xm_u32 loop_end;  /* position in audio frames */
	xm_u32 length; /* size in audio frames */
	xm_u32 base_sample_rate; /* base sample rate of SFX */
} XM_SampleData;

typedef xm_s32 XM_SampleID;

/** XM_Mixer:
  *
  * Chibi XM Play provides a default software mixer, but for portability issues, or taking advantage of
  * a certain architecture or mixing hardware, the mixer can be reimplemented for any other backend.
  *
  * Methods tagged with *LOCK* mean that they must not be called directly by the user,
  * unless locking/unlocking is performed before/after calling them, or calling from the same
  * thread as the process callback.
  */



typedef struct {

	void (*set_process_callback)(void (*p_process_callback)()); /** callback to use for every interval. This is called from the sound thread (used by player, don't call) */
	void (*set_process_callback_interval)(xm_u32 p_usec); /** set interval for process callback, in usecs (used by player, don't call) */

	void (*voice_start)(xm_u8 p_voice,XM_SampleID p_sample,xm_u32 p_offset); /** start offset in audio frames, *LOCK* */
	void (*voice_stop)(xm_u8 p_voice); /** stop voice, *LOCK* */
	void (*voice_set_volume)(xm_u8 p_voice,xm_u8 p_vol); /** volume from 0 to 255 *LOCK* */
	xm_u8 (*voice_get_volume)(xm_u8 p_voice); /** volume from 0 to 255 */
	void (*voice_set_pan)(xm_u8 p_voice,xm_u8 p_pan); /** pan from 0 to 255, 127 is center *LOCK* */
	void (*voice_set_speed)(xm_u8 p_voice,xm_u32 p_hz); /** speed, in audio frames per second *LOCK* */

	xm_bool (*voice_is_active)(xm_u8 p_voice); /** speed, in audio frames/second *LOCK* */

	XM_SampleID (*sample_register)(XM_SampleData *p_sample_data); /** Mixer takes ownership of the sample data */
	void (*sample_unregister)(XM_SampleID p_sample); /** Mixer releases ownership of the sample, and may free the sample data */
	void (*reset_voices)(); /** silence and reset all voices */
	void (*reset_samples)(); /** unregister all samples */

	xm_u32 (*get_features)(); /** Mixer feature bits, check MixerFeaturesFlags */


} XM_Mixer;

void xm_set_mixer(XM_Mixer *p_mixer); /** Set a Custom Mixer **/
XM_Mixer *xm_get_mixer(); /** Get the current mixer in use, or none if no mixer is set */

/** SOFTWARE MIXER **/

void xm_create_software_mixer(xm_u32 p_sampling_rate_hz, xm_u8 p_max_channels); /** Create the default
mixer. The default software mixer is High Quality, and while it's pretty optimized, you may want to create your own for specific/less performant platforms */
void xm_software_mix_to_buffer( xm_s32 *p_buffer, xm_u32 p_frames); /** Software mix to buffer, in stereo interleaved, 32 bits per sample. Mix the amount of frames requested. THIS ONLY WORKS FOR AN EXISTING AND ACTIVE SOFTWARE MIXER */



/** INSTRUMENT **/

typedef struct {

	XM_SampleID sample_id;
	xm_s8 base_note;
	xm_s8 finetune;
	xm_u8 volume;
	xm_u8 pan;

} XM_Sample;

typedef enum {

	XM_ENVELOPE_POINT_COUNT_MASK=(1<<5)-1, /** First 5 bits indicade point count */
	XM_ENVELOPE_ENABLED=(1<<7), /** If Loop is enabled, Enable Sustain */
	XM_ENVELOPE_LOOP_ENABLED=(1<<6), /** Loop is enabled **/
	XM_ENVELOPE_SUSTAIN_ENABLED=(1<<5), /** If Loop is enabled, Enable Sustain */

} XM_EnvelopeFlags;


/* macros to inrerpret the envelope points */
#define XM_ENV_OFFSET( m_point ) ((m_point)&0x1FF)
#define XM_ENV_VALUE( m_point ) ((m_point)>>9)

typedef struct {

	xm_u16 points[XM_CONSTANT_MAX_ENVELOPE_POINTS]; /* Envelope points */
	xm_u8 flags; /* Envelope Flags & Point count ( XM_EnvelopeFlags ) */

	xm_u8 sustain_index; /* Sustain Point Index */
	xm_u8 loop_begin_index; /* Loop Begin Point Index */
	xm_u8 loop_end_index; /* Loop End  Point Index */



} XM_Envelope;

typedef enum {

	XM_VIBRATO_SINE,
	XM_VIBRATO_SQUARE,
	XM_VIBRATO_SAW_UP,
	XM_VIBRATO_SAW_DOWN

} XM_VibratoType;

typedef struct {


	/* Envelopes */

	XM_Envelope volume_envelope;
	XM_Envelope pan_envelope;

	/* Instrument Vibratto */
	XM_VibratoType vibrato_type;

	xm_u8 vibrato_sweep;
	xm_u8 vibrato_depth;
	xm_u8 vibrato_rate;
	xm_u16 fadeout;

	/* Note/Sample table */

	xm_u8 note_sample[XM_CONSTANT_MAX_NOTES/2]; /* sample for each note, in nibbles */

	/* Sample Data  */

	XM_Sample * samples;
	xm_u8 sample_count;




} XM_Instrument;


/** SONG **/

typedef enum {
	XM_SONG_FLAGS_LINEAR_PERIODS=1<<7,
	XM_SONG_FLAGS_MASK_CHANNELS_USED=(1<<5)-1 /** flags&XM_SONG_FLAGS_MASK_CHANNELS_USED + 1 to obtain**/

} XM_SongFlags;

/* SONG HEADER, size in mem: 272 bytes */

typedef struct {

	char name[21];
	xm_u8 restart_pos;
	xm_u8 order_count;
	xm_u8 flags; /* flags, defined in SongFlags (including channels used) */

	xm_u8 pattern_count;
	xm_u8 instrument_count;
	xm_u8 tempo;
	xm_u8 speed;

	xm_u8 ** pattern_data; /* array of pointers to patern data,using xm packing, NULL means empty pattern */
	XM_Instrument ** instrument_data;

	xm_u8 order_list[256];


} XM_Song;

XM_Song *xm_song_alloc();
void xm_song_free(XM_Song *p_song);

/** PLAYER **/

void xm_player_set_song(XM_Song *p_song);

void xm_player_play();
void xm_player_stop();


/** FILEIO **/

typedef enum {

	XM_FILE_OK,
	XM_FILE_ERROR_CANT_OPEN,
	XM_FILE_ERROR_IN_USE
} XM_FileIOError;

typedef struct {
	xm_bool (*eof_reached)(); /** Returns true if EOF */
	xm_bool (*in_use)(); /** FileIO is working with a file */
	XM_FileIOError (*open)(const char *p_file,xm_bool p_big_endian_mode); /** Open a File **/
	xm_u8  (*get_u8)(); /** Get byte from file */
	xm_u16 (*get_u16)(); /** Get 16 bits word from file **/
	xm_u32 (*get_u32)(); /** Get 32 bits dword from file **/
	void   (*get_byte_array)(xm_u8 *p_dst,xm_u32 p_count); /** Get a byte array from the file */
	void   (*seek_pos)(xm_u32 p_offset); /** Seek to a file position **/
	xm_u32 (*get_pos)(); /** Get the current file position **/
	void(*close)(); /** Close the file **/
} XM_FileIO;

/** MEM-IO **/

/* Each type of allocation is detailed by the loader, this way,
   the host implementation can manage the optimum memory allocation scheme
   for music if necesary */

typedef enum {
	XM_MEMORY_ALLOC_SONG_HEADER,
	XM_MEMORY_ALLOC_INSTRUMENT,
	XM_MEMORY_ALLOC_SAMPLE,
	XM_MEMORY_ALLOC_PATTERN,
 	XM_MEMORY_ALLOC_SW_MIXER, /** Only if SW Mixer is used, allocated once **/
	XM_MEMORY_ALLOC_PLAYER, /** Memory needed for XM_Player, allocated once **/
	XM_MEMORY_ALLOC_WAV
} XM_MemoryAllocType;

typedef struct {

	void* (*alloc)(xm_u32 p_size,XM_MemoryAllocType p_alloc_type); /* alloc mem */
	void (*free)(void *p_mem,XM_MemoryAllocType p_free_type); /* free mem */

} XM_MemoryManager;

void xm_set_memory_manager(XM_MemoryManager *p_memory_manager);
XM_MemoryManager *xm_get_memory_manager();

/** LOADER **/

typedef enum {

	XM_LOADER_OK,
	XM_LOADER_UNCONFIGURED, /** FileIO/Mixer/MemoryIO was not set **/
	XM_LOADER_ERROR_FILEIO_IN_USE,
	XM_LOADER_ERROR_FILE_CANT_OPEN,
	XM_LOADER_ERROR_FILE_UNRECOGNIZED,
	XM_LOADER_ERROR_OUT_OF_MEMORY,
	XM_LOADER_ERROR_FILE_CORRUPT
} XM_LoaderError;

void xm_loader_set_fileio( XM_FileIO *p_fileio );

XM_LoaderError xm_loader_open_song( const char *p_filename,XM_Song *p_song );
XM_LoaderError xm_loader_open_song_music( const char *p_filename, XM_Song *p_song ); /* Load only header/patterns */
XM_LoaderError xm_loader_open_instruments( const char *p_filename, XM_Song *p_song ); /* Load only instruments/samples */

void xm_loader_free_song( XM_Song *p_song ); /** Free all song, p_song is NOT freed, use xm_song_free */
void xm_loader_free_music( XM_Song *p_song ); /** Free patterns, p_song is NOT freed, use xm_song_free */
void xm_loader_free_instruments( XM_Song *p_song ); /** free instruments/samples */

XM_SampleID xm_load_wav(const char *p_file);
int xm_sfx_start(XM_SampleID sample); /* start automatic, return voice */
void xm_sfx_start_voice(XM_SampleID sample,int voice);
void xm_sfx_set_vol(int voice, xm_u8 vol);
void xm_sfx_set_pan(int voice, xm_u8 pan);
void xm_sfx_set_pitch(int voice, xm_u32 pitch_hz);
void xm_sfx_stop(int voice);

void xm_loader_set_recompress_all_samples(xm_bool p_enable); /** recompress ALL samples to ima-adpcm while loading */
#endif
