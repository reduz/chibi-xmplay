#include "xmplay.h"
#include <stdlib.h>	
/*#define _XM_DEBUG */

#ifdef _XM_DEBUG_CUSTOM_H
	#include _XM_DEBUG_CUSTOM_H
#else
	#ifdef _XM_DEBUG
		#include <stdio.h>

		#define _XM_DEBUG_PRINTF( ... )\
		do {\
			fprintf(stdout , __VA_ARGS__);\
		} while (0)

	#define _XM_ERROR_PRINTF( ... )\
	do {\
		fprintf(stdout , "** %s:%i ** XMPLAY ERROR:  ", __FILE__, __LINE__);\
		fprintf(stdout , __VA_ARGS__);\
		fprintf(stdout , "\n");\
	} while (0)

#else
/* Or Implement as you wish */
#define _XM_DEBUG_PRINTF(  ... ) \
do { } while(0)
#define _XM_ERROR_PRINTF(  ... ) \
do { } while(0)

#endif

#endif

#define _XM_AUDIO_LOCK \
do {\
if (_xm_audio_lock)\
	_xm_audio_lock->lock();\
} while(0);

#define _XM_AUDIO_UNLOCK \
do {\
if (_xm_audio_lock)\
	_xm_audio_lock->unlock();\
} while(0);

/* standard ANSI C doesn't know inline
   replace this macro if you want proper
   inlining */

#ifndef _XM_INLINE
#define _XM_INLINE
#endif

#define _XM_ABS(m_v)  (((m_v)<0)?(-(m_v)):(m_v))
static void _xm_zero_mem(void * p_mem, xm_u32 p_bytes) {

	xm_u8 *mem=(xm_u8*)p_mem;
	int i;
	for (i=0;i<p_bytes;i++)
		mem[i]=0;
}

/*******************************
************LOCKING*************
*******************************/

static XM_AudioLock *_xm_audio_lock=0;

void xm_set_audio_lock( XM_AudioLock * p_audio_lock ) {

	if (_xm_audio_lock) {

		_XM_ERROR_PRINTF("XM AUDIO LOCK ALREADY CONFIGURED");
	}

	_xm_audio_lock=p_audio_lock;
}

XM_AudioLock * xm_get_audio_lock() {

	return _xm_audio_lock;
}


/*******************************
************MEMORY**************
*******************************/


static XM_MemoryManager * _xm_memory=0;


/*******************************
************MIXER**************
*******************************/

static XM_Mixer *_xm_mixer=0;


XM_Mixer *xm_get_mixer() {

	return _xm_mixer;
}


/*******************************
*********SOFTWARE MIXER********
*******************************/

/* Software Mixer Data */

/* change this limit as you see fit */
#define _XM_SW_MAX_SAMPLES 256
#define _XM_SW_FRAC_SIZE 12
#define _XM_SW_VOL_FRAC_SIZE 8
#define DECLICKER_BUFFER_BITS 6
#define DECLICKER_BUFFER_SIZE (1<<DECLICKER_BUFFER_BITS)
#define DECLICKER_BUFFER_MASK (DECLICKER_BUFFER_SIZE-1)
#define DECLICKER_FADE_BITS 5
#define DECLICKER_FADE_SIZE (1<<DECLICKER_FADE_BITS)

static const xm_s16 _xm_ima_adpcm_step_table[89] = { 
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 
};

static const xm_s8 _xm_ima_adpcm_index_table[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8	
};

typedef struct {

	XM_SampleID sample;
	xm_s32 increment_fp;

	xm_s32 oldvol_r,oldvol_l; /* volume ramp history */
	xm_s64 offset;

	xm_bool start; /* mixing for the first time with new parameters */
	xm_bool active;
	xm_u8 volume; /* 0 .. 255 */
	xm_u8 pan;  /* 0 .. 255 */
	
	struct {
		
		xm_s16 step_index;
		xm_s32 predictor;
		/* values at loop point */
		xm_s16 loop_step_index;
		xm_s32 loop_predictor;
		xm_s32 last_nibble;
	} ima_adpcm;

} _XM_SoftwareMixerVoice;

typedef struct {

	XM_Mixer mixer;
	XM_SampleData sample_pool[_XM_SW_MAX_SAMPLES ];
	_XM_SoftwareMixerVoice *voices;

	xm_s32 * mixdown_buffer;
	xm_u32 sampling_rate;

	void (*process_callback)();
	xm_u32 callback_interval;
	xm_u32 callback_interval_countdown;

	xm_s32 declicker_rbuffer[DECLICKER_BUFFER_SIZE*2];
	xm_s32 declicker_fade[DECLICKER_FADE_SIZE*2];
	xm_u32 declicker_pos;

	xm_u8 max_voices;
	xm_u8 agc_shift; /* shifting for auto gain control */


} _XM_SoftwareMixer;

_XM_SoftwareMixer *_xm_software_mixer=0;

static void _xm_sw_voice_start(xm_u8 p_voice,XM_SampleID p_sample,xm_u32 p_offset) {

	_XM_SoftwareMixer *m=_xm_software_mixer;

	if (p_voice>=m->max_voices) {

		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return;
	}
	if (p_sample<0 || p_sample>=_XM_SW_MAX_SAMPLES || !m->sample_pool[p_sample].data) {

		_XM_ERROR_PRINTF("INVALID SAMPLE: %i\n",p_voice);
		return;
	}

	/* SEND CURRENT VOICE, IF ACTIVE TO DECLICKER */

	if (m->voices[p_voice].active)
		m->mixer.voice_stop(p_voice);

	/* Set and Validate new voice */

	if (p_offset>=m->sample_pool[p_sample].length) {

		m->voices[p_voice].active=xm_false; /* turn off voice, offset is too long */
		return;
	}

	m->voices[p_voice].sample=p_sample;
	m->voices[p_voice].offset=(m->sample_pool[p_sample].format!=XM_SAMPLE_FORMAT_IMA_ADPCM)?p_offset:0;
	m->voices[p_voice].offset<<=_XM_SW_FRAC_SIZE; /* convert to fixed point */
	m->voices[p_voice].start=xm_true;
	m->voices[p_voice].active=xm_true;
	xm_u32 base_sample_rate = m->sample_pool[p_sample].base_sample_rate;
	if (base_sample_rate>0) {
		m->voices[p_voice].increment_fp=((xm_s64)base_sample_rate<<_XM_SW_FRAC_SIZE)/m->sampling_rate;
		m->voices[p_voice].volume=255;		
		m->voices[p_voice].pan=255;		
	} else {
		m->voices[p_voice].increment_fp=0;
	}
	
	if (m->sample_pool[p_sample].format==XM_SAMPLE_FORMAT_IMA_ADPCM) {
		/* IMA ADPCM SETUP */
		m->voices[p_voice].ima_adpcm.step_index=0;
		m->voices[p_voice].ima_adpcm.predictor=0;
		m->voices[p_voice].ima_adpcm.loop_step_index=0;
		m->voices[p_voice].ima_adpcm.loop_predictor=0;
		m->voices[p_voice].ima_adpcm.last_nibble=-1;				
	}

}

void _xm_sw_software_mix_voice_to_buffer( xm_u8 p_voice, xm_s32 *p_buffer, xm_u32 p_frames);


static void _xm_sw_voice_stop(xm_u8 p_voice) {

	_XM_SoftwareMixer *m=_xm_software_mixer;

	if (p_voice>=m->max_voices) {

		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return;
	}

	if (!m->voices[p_voice].active)
		return;

	if (m->voices[p_voice].increment_fp!=0) {

		int i=0;

		for (i=0;i<(DECLICKER_FADE_SIZE*2);i++) {
			m->declicker_fade[i]=0;

		}
		_xm_sw_software_mix_voice_to_buffer( p_voice, &m->declicker_fade[0], DECLICKER_FADE_SIZE );

		for (i=0;i<DECLICKER_FADE_SIZE;i++) {

			int dpos=((m->declicker_pos+i) & DECLICKER_BUFFER_MASK) << 1;
			int inv=DECLICKER_FADE_SIZE-i;


			m->declicker_rbuffer[ dpos ] += (m->declicker_fade [ i<<1] >> (DECLICKER_FADE_BITS))*inv;
			m->declicker_rbuffer[ dpos +1] += (m->declicker_fade [ (i<<1) +1] >> DECLICKER_FADE_BITS)*inv;
		}
	}

	/* SEND CURRENT VOICE, IF ACTIVE TO DECLICKER */


	m->voices[p_voice].active=xm_false;
}

#include <stdio.h>
#include <stdlib.h>

static xm_u8 _xm_sw_voice_get_volume(xm_u8 p_voice)
{
	_XM_SoftwareMixer *m=_xm_software_mixer;
	
	if (p_voice>=m->max_voices) {
		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return 0;
	}
	
	if (!m->voices[p_voice].active)
		return 0;
	
	return m->voices[p_voice].volume;
}

static void _xm_sw_voice_set_volume(xm_u8 p_voice,xm_u8 p_vol) {

	_XM_SoftwareMixer *m=_xm_software_mixer;

	if (p_voice>=m->max_voices) {

		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return;
	}

	if (!m->voices[p_voice].active)
		return;

	m->voices[p_voice].volume=p_vol;
}
static void _xm_sw_voice_set_pan(xm_u8 p_voice,xm_u8 p_pan) {

	_XM_SoftwareMixer *m=_xm_software_mixer;


	if (p_voice>=m->max_voices) {

		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return;
	}

	if (!m->voices[p_voice].active)
		return;

	m->voices[p_voice].pan=p_pan;

}
static xm_bool _xm_sw_voice_is_active(xm_u8 p_voice) {

	_XM_SoftwareMixer *m=_xm_software_mixer;

	if (p_voice>=m->max_voices) {

		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return xm_false;
	}

	return m->voices[p_voice].active;

}

static void _xm_sw_voice_set_speed(xm_u8 p_voice,xm_u32 p_hz) {

	_XM_SoftwareMixer *m=_xm_software_mixer;
	xm_bool backwards;

	if (p_voice>=m->max_voices) {

		_XM_ERROR_PRINTF("INVALID VOICE: %i\n",p_voice);
		return;
	}

	if (!m->voices[p_voice].active)
		return;

	/* imcrementor in fixed point */
	backwards=(m->voices[p_voice].increment_fp<0)?xm_true:xm_false;
	m->voices[p_voice].increment_fp=((xm_s64)p_hz<<_XM_SW_FRAC_SIZE)/m->sampling_rate;
	if (backwards)
		m->voices[p_voice].increment_fp=-m->voices[p_voice].increment_fp;

}

static XM_SampleID _xm_sw_sample_register(XM_SampleData *p_sample_data)  {


	int i;
	_XM_SoftwareMixer *m = _xm_software_mixer;



	if (p_sample_data->data==0) {
		_XM_ERROR_PRINTF("SAMPLE DATA IS NULL");
		return XM_INVALID_SAMPLE_ID;
	}

	for (i=0;i<_XM_SW_MAX_SAMPLES;i++) {

		if (m->sample_pool[i].data==p_sample_data) {
			_XM_ERROR_PRINTF("SAMPLE ALREADY REGISTERED");
			return XM_INVALID_SAMPLE_ID;
		}
	}
	for (i=0;i<_XM_SW_MAX_SAMPLES;i++) {

		if (m->sample_pool[i].data!=0)
			continue;

		m->sample_pool[i]=*p_sample_data;
		return i;
	}

	_XM_ERROR_PRINTF("SAMPLE POOL FULL");
	return XM_INVALID_SAMPLE_ID;

}
static void _xm_sw_sample_unregister(XM_SampleID p_sample)  {

	_XM_SoftwareMixer *m = _xm_software_mixer;

	if (p_sample<0 || p_sample>=_XM_SW_MAX_SAMPLES) {

		_XM_ERROR_PRINTF("INVALID SAMPLE ID: %i",p_sample);
		return;
	}
	_XM_AUDIO_LOCK

	_xm_memory->free( m->sample_pool[p_sample].data, XM_MEMORY_ALLOC_SAMPLE );
	m->sample_pool[p_sample].data=0; /* set to unused */

	_XM_AUDIO_UNLOCK

}

static void _xm_sw_reset_voices()  {

	int i;
	_XM_SoftwareMixer *m = _xm_software_mixer;

	_XM_AUDIO_LOCK
	for (i=0;i<m->max_voices;i++) {

		_XM_SoftwareMixerVoice *v=&m->voices[i];

		v->sample=XM_INVALID_SAMPLE_ID;
		v->increment_fp=0;

		v->oldvol_r=0;
		v->oldvol_l=0;
		v->offset=0;

		v->start=xm_false;
		v->active=xm_false;
		v->volume=0;
		v->pan=0;
	}
	_XM_AUDIO_UNLOCK

}
static void _xm_sw_reset_samples()  {


}

static xm_u32 _xm_sw_get_features()  {

	_XM_SoftwareMixer *m = _xm_software_mixer;
	return (XM_MIXER_FEATURE_NEEDS_END_PADDING|(xm_u32)m->max_voices);
}


void _xm_sw_set_process_callback(void (*p_process_callback)()) {

	_XM_SoftwareMixer *m = _xm_software_mixer;
	m->process_callback=p_process_callback;

}
void _xm_sw_set_process_callback_interval(xm_u32 p_usec) {

	_XM_SoftwareMixer *m = _xm_software_mixer;
	/* convert to frames */
	xm_s64 interval=p_usec;
	interval*=m->sampling_rate;
	interval/=1000000L;

	m->callback_interval = (xm_u32)interval;

}

void xm_create_software_mixer(xm_u32 p_sampling_rate_hz, xm_u8 p_max_channels) {



	if (_xm_mixer) {

		_XM_ERROR_PRINTF("MIXER ALREADY CONFIGURED");
		return;
	}
	if (_xm_software_mixer) {

		_XM_ERROR_PRINTF("SOFTWARE MIXER ALREADY CREATED");
		return;
	}

	_xm_software_mixer = (_XM_SoftwareMixer *)_xm_memory->alloc( sizeof(_XM_SoftwareMixer), XM_MEMORY_ALLOC_SW_MIXER );

	if (!_xm_software_mixer) {

		_XM_ERROR_PRINTF("OUT OF MEMORY FOR SOFTWARE MIXER");
		return;

	}
	_xm_zero_mem( _xm_software_mixer, sizeof(_XM_SoftwareMixer) );

	/* Assign Functions */

	_xm_software_mixer->mixer.set_process_callback=_xm_sw_set_process_callback;
	_xm_software_mixer->mixer.set_process_callback_interval=_xm_sw_set_process_callback_interval;
	_xm_software_mixer->mixer.voice_start=_xm_sw_voice_start;
	_xm_software_mixer->mixer.voice_stop=_xm_sw_voice_stop;
	_xm_software_mixer->mixer.voice_set_volume=_xm_sw_voice_set_volume;
	_xm_software_mixer->mixer.voice_get_volume=_xm_sw_voice_get_volume;
	_xm_software_mixer->mixer.voice_set_pan=_xm_sw_voice_set_pan;
	_xm_software_mixer->mixer.voice_is_active=_xm_sw_voice_is_active;
	_xm_software_mixer->mixer.voice_set_speed=_xm_sw_voice_set_speed;
	_xm_software_mixer->mixer.sample_register=_xm_sw_sample_register;
	_xm_software_mixer->mixer.sample_unregister=_xm_sw_sample_unregister;
	_xm_software_mixer->mixer.reset_voices=_xm_sw_reset_voices;
	_xm_software_mixer->mixer.reset_samples=_xm_sw_reset_samples;
	_xm_software_mixer->mixer.get_features=_xm_sw_get_features;


	_xm_software_mixer->sampling_rate=p_sampling_rate_hz;
	_xm_software_mixer->voices=(_XM_SoftwareMixerVoice*)_xm_memory->alloc( sizeof(_XM_SoftwareMixerVoice)*p_max_channels, XM_MEMORY_ALLOC_SW_MIXER );
	_xm_software_mixer->max_voices=p_max_channels;
	_xm_software_mixer->mixdown_buffer=0;
	_xm_software_mixer->process_callback=0;
	_xm_software_mixer->callback_interval=0; /* every mixing frame */
	_xm_software_mixer->callback_interval_countdown=0; /* every mixing frame */


	/* process the auto gain control shift */
	_xm_software_mixer->agc_shift=7;
	/*
	for (i=7;i>=0;i--) {

		if (p_max_channels&((1<<i))) {

			_xm_software_mixer->agc_shift=(7-i)+1;
			break;
		}
	}
	*/
	_xm_sw_reset_voices();

	xm_set_mixer(&_xm_software_mixer->mixer);

}


void _xm_sw_software_mix_voice_to_buffer( xm_u8 p_voice, xm_s32 *p_buffer, xm_u32 p_frames) {

	/* some pointers.. */
	_XM_SoftwareMixer *m = _xm_software_mixer;
	_XM_SoftwareMixerVoice *v=&m->voices[p_voice];
	XM_SampleData *s=&m->sample_pool[ v->sample ];


	xm_u32 vol=v->volume; /* 32 bits version of the volume */

	/* some 64-bit fixed point precaches */
	xm_s64 loop_begin_fp=((xm_s64)s->loop_begin << _XM_SW_FRAC_SIZE);
	xm_s64 loop_end_fp=((xm_s64)s->loop_end << _XM_SW_FRAC_SIZE);
	xm_s64 length_fp=((xm_s64)s->length << _XM_SW_FRAC_SIZE);
	xm_s64 begin_limit=(s->loop_type!=XM_LOOP_DISABLED)?loop_begin_fp:0;
	xm_s64 end_limit=(s->loop_type!=XM_LOOP_DISABLED)?loop_end_fp:length_fp;
	xm_s32 todo=p_frames;
	xm_s32 vol_l,vol_r;
	xm_s32 dst_vol_l,dst_vol_r;
	xm_s32 vol_l_inc,vol_r_inc;
	/* check that sample is valid */

	if (!s->data) {
		/* if sample dissapeared, disable voice */

		v->active=xm_false;
		return;
	}

	/* compute voice left and right volume multipliers */

	vol<<= m->agc_shift; /* apply autogain shift */
	dst_vol_l=(vol * (255-v->pan)) >> 8;
	dst_vol_r=(vol * (v->pan)) >> 8;


	if (v->start) { /* first time, reset ramp */
		v->oldvol_l=dst_vol_l;
		v->oldvol_r=dst_vol_r;
	}

	/* compute vlume ramps */

	vol_l=v->oldvol_l<<_XM_SW_VOL_FRAC_SIZE;
	vol_r=v->oldvol_r<<_XM_SW_VOL_FRAC_SIZE;
	vol_l_inc=((dst_vol_l-v->oldvol_l)<<_XM_SW_VOL_FRAC_SIZE)/p_frames;
	vol_r_inc=((dst_vol_r-v->oldvol_r)<<_XM_SW_VOL_FRAC_SIZE)/p_frames;

	XM_LoopType loop_type = s->loop_type;
	if (loop_type==XM_LOOP_PING_PONG && s->format==XM_SAMPLE_FORMAT_IMA_ADPCM) {
		
		loop_type=XM_LOOP_FORWARD;
	}
	/* @TODO validar loops al registrar , pedir un poco mas de memoria para interpolar */

	while (todo>0) {

		xm_s64 limit=0;
		xm_s32 target=0,aux=0;

		/** LOOP CHECKING **/

		if ( v->increment_fp < 0 ) {
			/* going backwards */

			if(  loop_type!=XM_LOOP_DISABLED && v->offset < loop_begin_fp ) {
				/* loopstart reached */

				if ( loop_type==XM_LOOP_PING_PONG ) {
					/* bounce ping pong */
					v->offset= loop_begin_fp + ( loop_begin_fp-v->offset );
					v->increment_fp=-v->increment_fp;
				} else {
					/* go to loop-end */
					v->offset=loop_end_fp-(loop_begin_fp-v->offset);
				}
			} else {
				/* check for sample not reaching begining */
				if(v->offset < 0) {

					v->active=xm_false;
					break;
				}
			}
		} else {
			/* going forward */
			if(  loop_type!=XM_LOOP_DISABLED && v->offset >= loop_end_fp ) {
				/* loopend reached */

				if ( loop_type==XM_LOOP_PING_PONG ) {
					/* bounce ping pong */
					v->offset=loop_end_fp-(v->offset-loop_end_fp);
					v->increment_fp=-v->increment_fp;
				} else {
					/* go to loop-begin */
					if (s->format==XM_SAMPLE_FORMAT_IMA_ADPCM) {
						v->ima_adpcm.step_index=v->ima_adpcm.loop_step_index;
						v->ima_adpcm.predictor=v->ima_adpcm.loop_predictor;
						v->ima_adpcm.last_nibble=loop_begin_fp>>_XM_SW_FRAC_SIZE;
						v->offset=loop_begin_fp;
					} else {
						
						v->offset=loop_begin_fp+(v->offset-loop_end_fp);
						
					}
				}
			} else {
				/* no loop, check for end of sample */
				if(v->offset >= length_fp) {

					v->active=xm_false;

					break;
				}
			}
		}

		/** MIXCOUNT COMPUTING **/

		/* next possible limit (looppoints or sample begin/end */
		limit=(v->increment_fp < 0) ?begin_limit:end_limit;

		/* compute what is shorter, the todo or the limit? */
		aux=(limit-v->offset)/v->increment_fp+1;
		target=(aux<todo)?aux:todo; /* mix target is the shorter buffer */

		/* check just in case */
		if ( target<=0 ) {

			v->active=xm_false;
			break;
		}

		todo-=target;

		switch ( s->format ) {

			case XM_SAMPLE_FORMAT_PCM8: { /* signed 8-bits */

				/* convert to local mixing chunk so
				    32 bits resampling can be used */
				xm_s8 *src_ptr =  &((xm_s8*)s->data)[v->offset >> _XM_SW_FRAC_SIZE ];
				xm_s32 offset=v->offset&( (1<<_XM_SW_FRAC_SIZE) -1); /* strip integer */

				while (target--) {

					xm_s32 val = src_ptr[offset>>_XM_SW_FRAC_SIZE];
					xm_s32 val_next = src_ptr[(offset>>_XM_SW_FRAC_SIZE) +1];
					val<<=8; /* convert to 16 */
					val_next<<=8; /* convert to 16 */

					val=val+((val_next-val)*((offset)&(((1<<_XM_SW_FRAC_SIZE))-1)) >> _XM_SW_FRAC_SIZE);

					*(p_buffer++) += val * (vol_l>>_XM_SW_VOL_FRAC_SIZE);
					*(p_buffer++) += val * (vol_r>>_XM_SW_VOL_FRAC_SIZE);

					vol_l+=vol_l_inc;
					vol_r+=vol_r_inc;
					offset+=v->increment_fp;

				}

				v->offset+=offset;

			} break;
			case XM_SAMPLE_FORMAT_PCM16: { /* signed 16-bits */

				/* convert to local mixing chunk so
				32 bits resampling can be used */
				xm_s16 *src_ptr =  &((xm_s16*)s->data)[v->offset >> _XM_SW_FRAC_SIZE ];
				xm_s32 offset=v->offset&( (1<<_XM_SW_FRAC_SIZE) -1); /* strip integer */

				while (target--) {

					xm_s32 val = src_ptr[offset>>_XM_SW_FRAC_SIZE];
					xm_s32 val_next = src_ptr[(offset>>_XM_SW_FRAC_SIZE) +1];

					val=val+((val_next-val)*((offset)&(((1<<_XM_SW_FRAC_SIZE))-1)) >> _XM_SW_FRAC_SIZE);

					*(p_buffer++) += val * (vol_l>>_XM_SW_VOL_FRAC_SIZE);
					*(p_buffer++) += val * (vol_r>>_XM_SW_VOL_FRAC_SIZE);

					vol_l+=vol_l_inc;
					vol_r+=vol_r_inc;

					offset+=v->increment_fp;

				}

				v->offset+=offset;

			} break;
			case XM_SAMPLE_FORMAT_IMA_ADPCM: { /* ima-adpcm */

				xm_u8 *src_ptr =  (xm_u8*)s->data;
				src_ptr+=4;

				while (target--) {

					xm_s32 integer = (v->offset>>_XM_SW_FRAC_SIZE);
					while(integer>v->ima_adpcm.last_nibble) {			
						xm_s16 nibble,diff,step;
						
						v->ima_adpcm.last_nibble++;
						
						nibble = (v->ima_adpcm.last_nibble&1)?
								(src_ptr[v->ima_adpcm.last_nibble>>1]>>4):(src_ptr[v->ima_adpcm.last_nibble>>1]&0xF);
						step=_xm_ima_adpcm_step_table[v->ima_adpcm.step_index];
						
						v->ima_adpcm.step_index += _xm_ima_adpcm_index_table[nibble];
						if (v->ima_adpcm.step_index<0) 
							v->ima_adpcm.step_index=0;
						if (v->ima_adpcm.step_index>88)
							v->ima_adpcm.step_index=88;

						/*
						signed_nibble = (nibble&7) * ((nibble&8)?-1:1);						
						diff = (2 * signed_nibble + 1) * step / 4; */
						
						diff = step >> 3 ;
						if (nibble & 1)
							diff += step >> 2 ;
						if (nibble & 2)
							diff += step >> 1 ;
						if (nibble & 4)
							diff += step ;
						if (nibble & 8)
							diff = -diff ;
						
						v->ima_adpcm.predictor+=diff;
						if (v->ima_adpcm.predictor<-0x8000)
							v->ima_adpcm.predictor=0x8000;
						else if (v->ima_adpcm.predictor>0x7FFF)
							v->ima_adpcm.predictor=0x7FFF;
							
						
						
						/*
						if (v->ima_adpcm.step_index<0)
							v->ima_adpcm.step_index=0;
						if (v->ima_adpcm.step_index>88)
							v->ima_adpcm.step_index=88;
						*/
									
						/*
						sign = nibble & 8;
						delta = nibble & 7;
						diff=0;
						if (delta & 4) 
							diff += step;
						step>>=1;
						if (delta & 2) 
							diff += step;
						step>>=1;
						if (delta & 1) 
							diff += step;
						step>>=1;
						diff += step;
						if (sign) {
							v->ima_adpcm.predictor -= diff;
							if (v->ima_adpcm.predictor<-0x8000) 
								v->ima_adpcm.predictor = -0x8000;
						} else {
							v->ima_adpcm.predictor += diff; 	
							if (v->ima_adpcm.predictor>0x7FFF) 
								v->ima_adpcm.predictor = 0x7FFF;
						}
						*/					
						/*printf("nibble %i, diff %i, predictor at %i\n",nibble,diff,v->ima_adpcm.predictor);*/
												
						/* store loop if there */
						if (s->loop_type==XM_LOOP_FORWARD && v->ima_adpcm.last_nibble==s->loop_begin) {
							
							v->ima_adpcm.loop_step_index = v->ima_adpcm.step_index;
							v->ima_adpcm.loop_predictor = v->ima_adpcm.predictor;
						}
														
					}
											
					xm_s32 val = v->ima_adpcm.predictor;

					*(p_buffer++) += val * (vol_l>>_XM_SW_VOL_FRAC_SIZE);
					*(p_buffer++) += val * (vol_r>>_XM_SW_VOL_FRAC_SIZE);

					vol_l+=vol_l_inc;
					vol_r+=vol_r_inc;
					v->offset+=v->increment_fp;

				}
			} break;
			case XM_SAMPLE_FORMAT_CUSTOM: { /* Custom format, XM_Mixer should support this */

				/* I can't play this! */
			} break;

		}

	}

	v->oldvol_l=dst_vol_l;
	v->oldvol_r=dst_vol_r;
}

void _xm_sw_software_mix_voices_to_buffer( xm_s32 *p_buffer, xm_u32 p_frames) {

	_XM_SoftwareMixer *m = _xm_software_mixer;
	int i;
	xm_s32 *target_buff=p_buffer;

	for (i=0;i<m->max_voices;i++) {

		if (!m->voices[i].active) /* ignore inactive voices */
			continue;
		_xm_sw_software_mix_voice_to_buffer( i, p_buffer, p_frames );
	}



	for (i=0;i<p_frames;i++) {


		int dpos=(m->declicker_pos & DECLICKER_BUFFER_MASK) << 1;

		target_buff[i<<1]+=m->declicker_rbuffer[ dpos ];
		target_buff[(i<<1)+1]+=m->declicker_rbuffer[ dpos+1 ];
		m->declicker_rbuffer[ dpos ]=0;
		m->declicker_rbuffer[ dpos+1 ]=0;
		m->declicker_pos++;

	}

}
void xm_software_mix_to_buffer( xm_s32 *p_buffer, xm_u32 p_frames) {

	_XM_SoftwareMixer *m = _xm_software_mixer;

	if (m->callback_interval == 0 || !m->process_callback)
	{
		if (m->process_callback)
			m->process_callback(); /* if callback is there, just call it */
		_xm_sw_software_mix_voices_to_buffer(p_buffer,p_frames);
		return;
	}

	while (p_frames) 
	{
		xm_u32 to_mix=0;

		if ( m->callback_interval_countdown == 0 ) { /* callback time! */
			m->process_callback(); /* pass the ball to the _XM_Player */
			m->callback_interval_countdown=m->callback_interval;
		}

		to_mix=(m->callback_interval_countdown < p_frames) ? m->callback_interval_countdown : p_frames;

		_xm_sw_software_mix_voices_to_buffer(p_buffer,to_mix);
		p_buffer+=to_mix*2; /* advance the pointer too  */

		m->callback_interval_countdown-=to_mix;
		p_frames-=to_mix;

	}

}


/*******************************
*********PLAYER****************
*******************************/


typedef enum {

	_XM_NOTE_OFF=97,
 	_XM_FIELD_EMPTY=0xFF,
	_XM_COMP_NOTE_BIT=1,
	_XM_COMP_INSTRUMENT_BIT=2,
	_XM_COMP_VOLUME_BIT=4,
	_XM_COMP_COMMAND_BIT=8,
	_XM_COMP_PARAMETER_BIT=16,
 	_XM_MAX_CHANNELS=32


} _XM_PlayerConstants;

typedef struct {

	xm_u8 note; /* 0 .. 96 , 97 is note off */
	xm_u8 instrument; /* 0 .. 127 */
	xm_u8 volume; /* xm volume < 0x10 means no volume */
	xm_u8 command; /* xm command, 255 means no command */
	xm_u8 parameter; /* xm parameter, 0x00 to 0xFF */

} _XM_Note;

typedef struct {

	xm_s16 tick; /* current tick */
	xm_u8 current_point; /* current node where ticks belong to (for speed) */
	xm_u8 value;
	xm_bool done;

} _XM_EnvelopeProcess;

typedef struct {

	_XM_EnvelopeProcess volume_envelope;
	_XM_EnvelopeProcess pan_envelope;

	XM_Instrument *instrument_ptr;
	XM_Sample *sample_ptr;

	xm_u32 note_start_offset;

	xm_s32 period; /* channel period  */
	xm_s32 old_period; /* period before note */
	xm_s32 tickrel_period; /* only for this tick, relative period added to output..  */
	xm_s32 porta_period; /* target period for portamento  */

	xm_u8 note; /* last note parsed */
	xm_u8 instrument; /* last instrument parsed */
	xm_u8 command;
	xm_u8 parameter;


	xm_bool active;
	xm_bool note_off;
	xm_u8 volume;
	xm_u8 pan;

	xm_u8 sample;
	xm_bool portamento_active;
	xm_bool row_has_note;
	xm_bool restart;

	xm_u32 restart_offset;

	xm_s16 tickrel_volume;
	xm_u16 fadeout;
	xm_s16 real_note; /* note + note adjustment in sample */
	xm_s8 finetune; /* finetune used */
	xm_u8 volume_command;
	xm_u8 note_delay;

	/* effect memories */
	xm_u8 fx_1_memory;
	xm_u8 fx_2_memory;
	xm_u8 fx_3_memory;
	xm_u8 fx_4_memory;
	xm_s8 fx_4_vibrato_phase;
	xm_u8 fx_4_vibrato_speed;
	xm_u8 fx_4_vibrato_depth;
	xm_u8 fx_4_vibrato_type;
	xm_u8 fx_5_memory;
	xm_u8 fx_6_memory;
	xm_s8 fx_7_tremolo_phase;
	xm_u8 fx_7_tremolo_speed;
	xm_u8 fx_7_tremolo_depth;
	xm_u8 fx_7_tremolo_type;
	xm_u8 fx_A_memory;
	xm_u8 fx_E1_memory;
	xm_u8 fx_E2_memory;
	xm_u8 fx_EA_memory;
	xm_u8 fx_EB_memory;
	xm_u8 fx_H_memory;
	xm_u8 fx_P_memory;
	xm_u8 fx_R_memory;
	xm_u8 fx_X1_memory;
	xm_u8 fx_X2_memory;

} _XM_Channel;

typedef struct {

	xm_u8 caches[_XM_MAX_CHANNELS][5];
	xm_u8 *pattern_ptr;
	xm_u8 *pos_ptr;
	xm_s16 row;
	xm_s8 channel;
	xm_s16 last_row; /* last requested row */
	xm_s8 last_channel; /* last requested row */

} _XM_PatternDecompressor;




typedef struct {

	_XM_Channel channel[_XM_MAX_CHANNELS];

	XM_Song *song;

	xm_u8 tick_counter;
	xm_u8 current_speed;
	xm_u8 current_tempo;
	xm_u8 global_volume;

	/* Position */

	xm_u16 current_order;
	xm_u16 current_row;
	xm_u16 current_pattern;
	xm_u16 pattern_delay;

	/* flags */

	xm_bool force_next_order; /* force an order change, XM commands Dxx, Bxx */
	xm_u8 forced_next_order; /* order change */
	xm_u16 forced_next_order_row; /* order change */
	xm_bool active;

	_XM_PatternDecompressor decompressor;

} _XM_PlayerData;


_XM_PlayerData *_xm_player=0;

static void _xm_player_reset() {


	int i;
	_XM_PlayerData *p=_xm_player;

	for (i=0;i<_XM_MAX_CHANNELS;i++) {

		_xm_zero_mem( &p->channel[i], sizeof( _XM_Channel) );
		p->channel[i].pan=32;
	}

	if (p->song) {

		p->current_speed=p->song->speed;
		p->current_tempo=p->song->tempo;
	} else {

		p->current_speed=5;
		p->current_tempo=120;

	}

	p->tick_counter=p->current_speed; /* so it starts on a row */

	/* Position */

	p->current_order=0;
	p->current_row=0;
	p->pattern_delay=0;

	if (p->song) {
		p->current_pattern=p->song->order_list[0];
	}
	p->force_next_order=xm_false;
	p->forced_next_order=0;
	p->forced_next_order_row=0;
	p->active=xm_false;
	p->global_volume=64; /* 0x40 */
	_xm_zero_mem( &p->decompressor, sizeof( _XM_PatternDecompressor) );

}

static xm_u16 _xm_player_get_pattern_length(xm_u8 p_pattern) {

	_XM_PlayerData *p=_xm_player;

	if (p_pattern >= p->song->pattern_count || !p->song->pattern_data[p_pattern])
		return 64;
	else
		return (xm_u16)*p->song->pattern_data[p_pattern]+1;
}


static void _xm_player_reset_decompressor() {

	_XM_PlayerData *p=_xm_player;


	if (p->decompressor.pattern_ptr) {

		int i;
		/* reset caches */
		for (i=0;i<_XM_MAX_CHANNELS;i++) {

			p->decompressor.caches[i][0]=_XM_FIELD_EMPTY;
			p->decompressor.caches[i][1]=_XM_FIELD_EMPTY;
			p->decompressor.caches[i][2]=_XM_FIELD_EMPTY;
			p->decompressor.caches[i][3]=_XM_FIELD_EMPTY;
			p->decompressor.caches[i][4]=0; /* only values other than 0 are read for this as cache */
		}

		/* set stream pointer */

		p->decompressor.pos_ptr=&p->decompressor.pattern_ptr[1];
	} else {

		p->decompressor.pos_ptr=0;
	}

	/* set stream pos */

	p->decompressor.row=0;
	p->decompressor.channel=0;
	p->decompressor.last_row=0;
	p->decompressor.last_channel=0;

}

static const _XM_Note _xm_empty_note={ _XM_FIELD_EMPTY, _XM_FIELD_EMPTY, 0, _XM_FIELD_EMPTY, 0 };

typedef enum {

	_XM_DCMP_CMD_CHAN   =0,  /*000,   set channel */
	_XM_DCMP_CMD_CACHE  =1,  /*001,   use caches */
	_XM_DCMP_CMD_FIELD  =2,  /*010,   read fields */
	_XM_DCMP_CMD_EOP    =3,  /*011,   end of pattern */
	_XM_DCMP_CMD_CHANR  =4,  /*100,   read chan, advance row*/
	/* _XM_DCMP_CMD_CACHEC =5,  101,   advance one chan, use caches,  */
 	/* _XM_DCMP_CMD_FIELDC =6,  110,   advance one chan, read fields,  */
	_XM_DCMP_CMD_ADVROW =7   /*111,   advance rows  */

} _XM_PatternCompressionCommand;

static _XM_INLINE _XM_Note _xm_player_decompress_note(xm_u8 p_row, xm_u8 p_channel) {

	_XM_Note n=_xm_empty_note;
	xm_u8 * current_pattern_ptr=0;
	_XM_PlayerData *p=_xm_player;

	/* check current pattern validity */
	if (p->current_pattern < p->song->pattern_count && p->song->pattern_data[p->current_pattern]) {

		current_pattern_ptr=p->song->pattern_data[p->current_pattern];
	}

	/* if pattern changed, reset decompressor */
	if (current_pattern_ptr!=p->decompressor.pattern_ptr) {

		p->decompressor.pattern_ptr=current_pattern_ptr;
		_xm_player_reset_decompressor();
	}

	/* check for empty pattern, return empty note if pattern is empty */

	if (!current_pattern_ptr) {

		return _xm_empty_note;
	}

	/* check if the position is requested is behind the seekpos,
	   and not after. If so, reset decompressor and seek to begining */

	if (p->decompressor.last_row > p_row || (p->decompressor.last_row == p_row && p->decompressor.last_channel > p_channel)) {

		_xm_player_reset_decompressor();
	}

	while( p->decompressor.row < p_row || ( p->decompressor.row == p_row && p->decompressor.channel <= p_channel ) ) {

		xm_u8 cmd = *p->decompressor.pos_ptr;
		int i;

		/*printf("pdecomp cmd %i%i%i(%i) - data %i\n",(cmd>>7)&1,(cmd>>6)&1,(cmd>>5)&1,cmd>>5,cmd&0x1F);*/

		/* at end of pattern, just break */
		if ((cmd>>5)==_XM_DCMP_CMD_EOP)
			break;

		switch(cmd>>5) {

			case _XM_DCMP_CMD_CHANR:

				p->decompressor.row++;

			case _XM_DCMP_CMD_CHAN: {

				p->decompressor.channel=cmd&0x1F; /* channel in lower 5 bits */
			} break;
			case _XM_DCMP_CMD_FIELD:

				 /* read fields into the cache */
				for (i=0;i<5;i++) {

					if (cmd&(1<<i)) {

						p->decompressor.pos_ptr++;
						p->decompressor.caches[p->decompressor.channel][i]=*p->decompressor.pos_ptr;
					}
				}
				/* fallthrough because values must be read */
			case _XM_DCMP_CMD_CACHE: {


				/* if not the same position, break */
				if (p->decompressor.row!=p_row || p->decompressor.channel!=p_channel)
					break;

				/* otherwise assign the caches to the note */

				if (cmd&_XM_COMP_NOTE_BIT)
					n.note=p->decompressor.caches[p_channel][0];
				if (cmd&_XM_COMP_INSTRUMENT_BIT)
					n.instrument=p->decompressor.caches[p_channel][1];
				if (cmd&_XM_COMP_VOLUME_BIT)
					n.volume=p->decompressor.caches[p_channel][2]+0x10;
				if (cmd&_XM_COMP_COMMAND_BIT)
					n.command=p->decompressor.caches[p_channel][3];
				if (cmd&_XM_COMP_PARAMETER_BIT)
					n.parameter=p->decompressor.caches[p_channel][4];


			} break;
			case _XM_DCMP_CMD_EOP: {

				_XM_ERROR_PRINTF("Code should never reach here!");
			} break;
			case _XM_DCMP_CMD_ADVROW: {

				p->decompressor.row+=(cmd&0x1F)+1;
				p->decompressor.channel=0;
			} break;
			default:
				_XM_ERROR_PRINTF("INVALID COMMAND!");

		}

		/* if not at end of pattern, advance one byte */
		p->decompressor.pos_ptr++;

	}

	p->decompressor.last_row=p_row;
	p->decompressor.last_channel=p_channel;

	/*if (	n.note != _xm_empty_note.note ||
		n.instrument != _xm_empty_note.instrument ||
		n.volume != _xm_empty_note.volume ||
		n.command != _xm_empty_note.command ||
		n.parameter != _xm_empty_note.parameter )
		printf("row/col: %i,%i   - %i,%i,%i,%i,%i\n",p_row,p_channel,n.note,n.instrument,n.volume,n.command,n.parameter);*/
	return n;
}

static _XM_INLINE void _xm_player_envelope_reset(_XM_EnvelopeProcess *p_env_process ) {

	p_env_process->tick=0;
	p_env_process->current_point=0;
	p_env_process->value=0;
	p_env_process->done=xm_false;
}


static const xm_u16 _xm_amiga_period_table[12*8+1]={

	907,900,894,887,881,875,868,862,
	856,850,844,838,832,826,820,814,
	808,802,796,791,785,779,774,768,
	762,757,752,746,741,736,730,725,
	720,715,709,704,699,694,689,684,
	678,675,670,665,660,655,651,646,
	640,636,632,628,623,619,614,610,
	604,601,597,592,588,584,580,575,
	570,567,563,559,555,551,547,543,
	538,535,532,528,524,520,516,513,
	508,505,502,498,494,491,487,484,
	480,477,474,470,467,463,460,457,454 /* plus one for interpolation */
};

/* this python script can generate the table below:

import math;
for x in range(0,768):
	print 8363.0*math.pow(2.0,(6.0*12.0*16.0*4.0 - x)/ (12*16*4))

*/


 static const xm_u32 _xm_linear_frequency_table[768]={

	535232,534749,534266,533784,533303,532822,532341,531861,
	531381,530902,530423,529944,529466,528988,528511,528034,
	527558,527082,526607,526131,525657,525183,524709,524236,
	523763,523290,522818,522346,521875,521404,520934,520464,
	519994,519525,519057,518588,518121,517653,517186,516720,
	516253,515788,515322,514858,514393,513929,513465,513002,
	512539,512077,511615,511154,510692,510232,509771,509312,
	508852,508393,507934,507476,507018,506561,506104,505647,
	505191,504735,504280,503825,503371,502917,502463,502010,
	501557,501104,500652,500201,499749,499298,498848,498398,
	497948,497499,497050,496602,496154,495706,495259,494812,
	494366,493920,493474,493029,492585,492140,491696,491253,
	490809,490367,489924,489482,489041,488600,488159,487718,
	487278,486839,486400,485961,485522,485084,484647,484210,
	483773,483336,482900,482465,482029,481595,481160,480726,
	480292,479859,479426,478994,478562,478130,477699,477268,
	476837,476407,475977,475548,475119,474690,474262,473834,
	473407,472979,472553,472126,471701,471275,470850,470425,
	470001,469577,469153,468730,468307,467884,467462,467041,
	466619,466198,465778,465358,464938,464518,464099,463681,
	463262,462844,462427,462010,461593,461177,460760,460345,
	459930,459515,459100,458686,458272,457859,457446,457033,
	456621,456209,455797,455386,454975,454565,454155,453745,
	453336,452927,452518,452110,451702,451294,450887,450481,
	450074,449668,449262,448857,448452,448048,447644,447240,
	446836,446433,446030,445628,445226,444824,444423,444022,
	443622,443221,442821,442422,442023,441624,441226,440828,
	440430,440033,439636,439239,438843,438447,438051,437656,
	437261,436867,436473,436079,435686,435293,434900,434508,
	434116,433724,433333,432942,432551,432161,431771,431382,
	430992,430604,430215,429827,429439,429052,428665,428278,
	427892,427506,427120,426735,426350,425965,425581,425197,
	424813,424430,424047,423665,423283,422901,422519,422138,
	421757,421377,420997,420617,420237,419858,419479,419101,
	418723,418345,417968,417591,417214,416838,416462,416086,
	415711,415336,414961,414586,414212,413839,413465,413092,
	412720,412347,411975,411604,411232,410862,410491,410121,
	409751,409381,409012,408643,408274,407906,407538,407170,
	406803,406436,406069,405703,405337,404971,404606,404241,
	403876,403512,403148,402784,402421,402058,401695,401333,
	400970,400609,400247,399886,399525,399165,398805,398445,
	398086,397727,397368,397009,396651,396293,395936,395579,
	395222,394865,394509,394153,393798,393442,393087,392733,
	392378,392024,391671,391317,390964,390612,390259,389907,
	389556,389204,388853,388502,388152,387802,387452,387102,
	386753,386404,386056,385707,385359,385012,384664,384317,
	383971,383624,383278,382932,382587,382242,381897,381552,
	381208,380864,380521,380177,379834,379492,379149,378807,
	378466,378124,377783,377442,377102,376762,376422,376082,
	375743,375404,375065,374727,374389,374051,373714,373377,
	373040,372703,372367,372031,371695,371360,371025,370690,
	370356,370022,369688,369355,369021,368688,368356,368023,
	367691,367360,367028,366697,366366,366036,365706,365376,
	365046,364717,364388,364059,363731,363403,363075,362747,
	362420,362093,361766,361440,361114,360788,360463,360137,
	359813,359488,359164,358840,358516,358193,357869,357547,
	357224,356902,356580,356258,355937,355616,355295,354974,
	354654,354334,354014,353695,353376,353057,352739,352420,
	352103,351785,351468,351150,350834,350517,350201,349885,
	349569,349254,348939,348624,348310,347995,347682,347368,
	347055,346741,346429,346116,345804,345492,345180,344869,
	344558,344247,343936,343626,343316,343006,342697,342388,
	342079,341770,341462,341154,340846,340539,340231,339924,
	339618,339311,339005,338700,338394,338089,337784,337479,
	337175,336870,336566,336263,335959,335656,335354,335051,
	334749,334447,334145,333844,333542,333242,332941,332641,
	332341,332041,331741,331442,331143,330844,330546,330247,
	329950,329652,329355,329057,328761,328464,328168,327872,
	327576,327280,326985,326690,326395,326101,325807,325513,
	325219,324926,324633,324340,324047,323755,323463,323171,
	322879,322588,322297,322006,321716,321426,321136,320846,
	320557,320267,319978,319690,319401,319113,318825,318538,
	318250,317963,317676,317390,317103,316817,316532,316246,
	315961,315676,315391,315106,314822,314538,314254,313971,
	313688,313405,313122,312839,312557,312275,311994,311712,
	311431,311150,310869,310589,310309,310029,309749,309470,
	309190,308911,308633,308354,308076,307798,307521,307243,
	306966,306689,306412,306136,305860,305584,305308,305033,
	304758,304483,304208,303934,303659,303385,303112,302838,
	302565,302292,302019,301747,301475,301203,300931,300660,
	300388,300117,299847,299576,299306,299036,298766,298497,
	298227,297958,297689,297421,297153,296884,296617,296349,
	296082,295815,295548,295281,295015,294749,294483,294217,
	293952,293686,293421,293157,292892,292628,292364,292100,
	291837,291574,291311,291048,290785,290523,290261,289999,
	289737,289476,289215,288954,288693,288433,288173,287913,
	287653,287393,287134,286875,286616,286358,286099,285841,
	285583,285326,285068,284811,284554,284298,284041,283785,
	283529,283273,283017,282762,282507,282252,281998,281743,
	281489,281235,280981,280728,280475,280222,279969,279716,
	279464,279212,278960,278708,278457,278206,277955,277704,
	277453,277203,276953,276703,276453,276204,275955,275706,
	275457,275209,274960,274712,274465,274217,273970,273722,
	273476,273229,272982,272736,272490,272244,271999,271753,
	271508,271263,271018,270774,270530,270286,270042,269798,
	269555,269312,269069,268826,268583,268341,268099,267857
 };
static xm_u32 _xm_player_get_period(xm_s16 p_note, xm_s8 p_fine) {

	_XM_PlayerData *p=_xm_player;

	/* Get period, specified by fasttracker xm.txt */

	if (p->song->flags&XM_SONG_FLAGS_LINEAR_PERIODS) {

		xm_s32 period = 10*12*16*4 - (xm_s32)(p_note-1)*16*4 - (xm_s32)p_fine/2;

		if (period<0)
			period=0;

		return (xm_u32)period;

	} else { /* amiga periods */

		xm_s16 fine=p_fine;
		xm_s16 note=p_note;
		xm_u16 idx; /* index in amiga table */
		xm_s16 period;

		/* fix to fit table, note always positive and fine always 0 .. 127 */
		if (fine<0) {

			fine+=128;
			--note;
		}

		if (note<0)
			note=0;

		/* find index in table */
		idx = ((note%12)<<3) + (fine >> 4);

		period = _xm_amiga_period_table[idx];
		/* interpolate for further fine-ness :) */
		period+= ((fine&0xF)*(_xm_amiga_period_table[idx+1]-period)) >> 4;

		/* apply octave */
		period=((period<<4)/(1<<(note/12)))<<1;
		return (xm_u32)period;

	}
}

static _XM_INLINE xm_u32 _xm_player_get_frequency(xm_u32 p_period) {

	_XM_PlayerData *p=_xm_player;

	/* Get frequency, specified by fasttracker xm.txt */

	if (p->song->flags&XM_SONG_FLAGS_LINEAR_PERIODS) {

		xm_s32 shift=(((xm_s32)p_period/768)-0);

		if (shift>0) {

			return _xm_linear_frequency_table[p_period%768]>>shift;
		} else {

			return _xm_linear_frequency_table[p_period%768]<<(-shift);
		}

	} else { /* amiga */

		return 8363*1712/p_period;

	}
}

static _XM_INLINE void _xm_process_notes() {

	_XM_PlayerData *p=_xm_player;
	int i;
	int channels=(p->song->flags&XM_SONG_FLAGS_MASK_CHANNELS_USED)+1;

	for (i=0;i<channels;i++) {

		/* Decomrpess a Note */
		_XM_Channel *ch=&p->channel[i];
		_XM_Note note = _xm_player_decompress_note(p->current_row, i);
		xm_bool process_note=xm_false;

		/* Validate instrument and note fields */

		if ((note.instrument!=_XM_FIELD_EMPTY && (note.instrument>=p->song->instrument_count || p->song->instrument_data[note.instrument]==0)) || (note.note!=_XM_FIELD_EMPTY && note.note>_XM_NOTE_OFF))  {

			/* if any is invalid, both become invalid */
			note.instrument=_XM_FIELD_EMPTY;
			note.note=_XM_FIELD_EMPTY;
		}

		/* Determine wether note should be processed */

		if ( (note.note!=_XM_FIELD_EMPTY || note.instrument!=_XM_FIELD_EMPTY) && note.note!=_XM_NOTE_OFF) {


			if (note.note==_XM_FIELD_EMPTY) {
					/* if note field is empty, there is only one case where
				a note can be played.. either the channel must be inactive,
				or the instrment is different than the one previously used
				in the channel. If the conditions meet, the previos note played
				is used (as long as it exist)*/
				if ((!ch->active || ch->instrument!=note.instrument) && ch->note!=_XM_FIELD_EMPTY) {

					note.note=ch->note; /* use previous channel note */
					process_note=xm_true;
				}

			} else if (note.instrument==_XM_FIELD_EMPTY) {

				if (note.note==_XM_NOTE_OFF) {

					process_note=xm_true;

				} if (ch->instrument!=_XM_FIELD_EMPTY) {

					note.instrument=ch->instrument;
					process_note=xm_true;
				}
			} else {

				process_note=xm_true;
			}

			if (process_note) {
				/* check the sample/instrument combination for validity*/

				xm_u8 sample;
				/* this was validated before.. */
				XM_Instrument *ins=p->song->instrument_data[note.instrument];
				sample=ins->note_sample[note.note>>1];
				sample=(note.note&1)?sample>>4:sample&0xF;

				if (sample >= ins->sample_count || ins->samples[sample].sample_id==XM_INVALID_SAMPLE_ID)
					process_note=xm_false; /* invalid sample */

			}

		}

		if (process_note) {

			xm_u8 sample;
			xm_bool sample_changed;

			/* set some note-start flags */
			ch->note_start_offset=0; /* unless changed by effect */
			ch->portamento_active=xm_false; /* unless changed by effect */
			ch->row_has_note=xm_true;
			ch->note_off=xm_false;

			ch->note=note.note;

			/** all this was previously validated **/
			ch->instrument=note.instrument;
			ch->instrument_ptr=p->song->instrument_data[ch->instrument];

			/* extract sample nibble */
			sample=ch->instrument_ptr->note_sample[ch->note>>1];
			sample=(ch->note&1)?sample>>4:sample&0xF;

			sample_changed=ch->sample!=sample;
			ch->sample=sample;
			ch->sample_ptr=&ch->instrument_ptr->samples[sample];

			ch->real_note=(xm_s16)ch->note+ch->sample_ptr->base_note;
			ch->finetune=ch->sample_ptr->finetune;

			/* envelopes */
			_xm_player_envelope_reset(&ch->volume_envelope);
			_xm_player_envelope_reset(&ch->pan_envelope);

			/* get period from note */
			ch->old_period=ch->period;
			ch->period=_xm_player_get_period( ch->real_note , ch->sample_ptr->finetune );

			if (sample_changed || ch->old_period==0) {
				/* if sample changed, fix portamento period */
				ch->old_period=ch->period;
			}

			if (ch->period==0)
				ch->active=xm_false;

			/* volume/pan */

			ch->volume=ch->sample_ptr->volume; /* may be overriden by volume column anyway */
			ch->pan=ch->sample_ptr->pan; /* may be overriden by volume column anyway */

			ch->restart=xm_true;
			ch->restart_offset=0; /* unless changed by command */
			ch->active=xm_true; /* mey got unset later */
			ch->fadeout=0xFFFF;
		} else {

			ch->row_has_note=xm_false;
		}

		/* process note off */
		if (note.note==_XM_NOTE_OFF && ch->active) {
			/* if channels is not active, ignore */

			if (ch->instrument_ptr) {

				if (ch->instrument_ptr->volume_envelope.flags&XM_ENVELOPE_ENABLED) {
						/* if the envelope is enabled, noteoff is used, for both
					the envelope and the fadeout */
					ch->note_off=xm_true;

				} else {
					/* otherwise, just deactivate the channel */
					ch->active=xm_false;

				}
			}
		}

		/* Volume */

		ch->volume_command=0; /* By default, reset volume command */

		if (note.volume>=0x10) {
			/* something in volume column... */

			if (note.volume>=0x10 && note.volume<=0x50) {
				/* set volume */
				ch->volume=note.volume-0x10;

			} else if (note.volume>=0xC0 && note.volume<=0xCF) {
				/* set pan */
				ch->pan = (note.volume-0xC0) << 4;
			} else {

				ch->volume_command=note.volume;
			}
		}

		/* Command / Parameter */


		ch->command=note.command;
		ch->parameter=note.parameter;

		ch->note_delay=0; /* force note delay to zero */
	} /* end processing note */

}

static _XM_INLINE void _xm_player_process_envelope( _XM_Channel *ch, _XM_EnvelopeProcess *p_env_process, XM_Envelope *p_envelope) {

	xm_s16 env_len;

	if ( !(p_envelope->flags&XM_ENVELOPE_ENABLED))
		return;
	if ( p_env_process->done ) /* Envelope is finished */
		return;

	env_len=p_envelope->flags&XM_ENVELOPE_POINT_COUNT_MASK;


	/* compute envelope first */

	if (	p_envelope->flags&XM_ENVELOPE_SUSTAIN_ENABLED &&
	        !ch->note_off ) {
		/* if sustain looping */
			if (p_env_process->current_point >= p_envelope->sustain_index ) {

				p_env_process->current_point=p_envelope->sustain_index;
				p_env_process->tick=0;
			}
	} else if ( p_envelope->flags&XM_ENVELOPE_LOOP_ENABLED ) {
		/* else if loop enabled */

		if (p_env_process->current_point >= p_envelope->loop_end_index ) {

			p_env_process->current_point=p_envelope->loop_begin_index;
			p_env_process->tick=0;
		}
	}

	if ( p_env_process->current_point >= (env_len-1) && ( p_env_process->tick > 0) ) {
		/* envelope is terminated. note tick>0 instead of ==0, as a clever
		   trick to know for certain when the envelope ended */

		p_env_process->done=xm_true;;
		p_env_process->current_point=env_len-1;
		if (env_len==0)
			return; /* a bug, don't bother with it */
	}

	{ /* process the envelope */

		xm_s16 node_len=(p_env_process->current_point >= (env_len-1)) ? 0 : (XM_ENV_OFFSET(p_envelope->points[ p_env_process->current_point+1 ]) - XM_ENV_OFFSET(p_envelope->points[ p_env_process->current_point ]));

		if (node_len==0 || ( p_env_process->tick==0)) {
			/* don't interpolate */
			p_env_process->value=XM_ENV_VALUE(p_envelope->points[ p_env_process->current_point ]);
		} else {

			xm_s16 v1=XM_ENV_VALUE(p_envelope->points[ p_env_process->current_point ]);
			xm_s16 v2=XM_ENV_VALUE(p_envelope->points[ p_env_process->current_point+1 ]);
			xm_s16 r=v1 + p_env_process->tick * ( v2-v1 ) / node_len;
			p_env_process->value=r;
		}

		/* increment */
		if (node_len) {
			p_env_process->tick++;
			if (p_env_process->tick>=node_len) {

				p_env_process->tick=0;
				p_env_process->current_point++;
			}
		}

	}


}

/** EFFECT COMMAND LISTINGS **/

typedef enum {
	/* list must be contiguous, for most compilers to
	   create a jump table in the switch() */
	_XM_FX_0_ARPEGGIO,
	_XM_FX_1_PORTA_UP,
	_XM_FX_2_PORTA_DOWN,
	_XM_FX_3_TONE_PORTA,
	_XM_FX_4_VIBRATO,
	_XM_FX_5_TONE_PORTA_AND_VOL_SLIDE,
	_XM_FX_6_VIBRATO_AND_VOL_SLIDE,
	_XM_FX_7_TREMOLO,
	_XM_FX_8_SET_PANNING,
	_XM_FX_9_SAMPLE_OFFSET,
	_XM_FX_A_VOLUME_SLIDE,
	_XM_FX_B_POSITION_JUMP,
	_XM_FX_C_SET_VOLUME,
	_XM_FX_D_PATTERN_BREAK,
	_XM_FX_E_SPECIAL,
	_XM_FX_F_SET_SPEED_AND_TEMPO,
	_XM_FX_G_SET_GLOBAL_VOLUME,
	_XM_FX_H_GLOBAL_VOLUME_SLIDE,
	_XM_FX_I_UNUSED,
	_XM_FX_J_UNUSED,
	_XM_FX_K_KEY_OFF,
	_XM_FX_L_SET_ENVELOPE_POS,
	_XM_FX_M_UNUSED,
	_XM_FX_N_UNUSED,
	_XM_FX_O_UNUSED,
	_XM_FX_P_PAN_SLIDE,
	_XM_FX_R_RETRIG,
	_XM_FX_S_UNUSED,
	_XM_FX_T_TREMOR,
	_XM_FX_U_UNUSED,
	_XM_FX_V_UNUSED,
	_XM_FX_W_UNUSED,
	_XM_FX_X_EXTRA_FINE_PORTA,
	_XM_FX_Y_UNUSED,
	_XM_FX_Z_FILTER /* for mixers that can do filtering? */
} _XM_FX_List;


static const xm_u8 _xm_fx_4_and_7_table[32]={ /* vibrato sine table */
	0, 24, 49, 74, 97,120,141,161,180,197,212,224,235,244,250,253,
 255,253,250,244,235,224,212,197,180,161,141,120, 97, 74, 49, 24
};

typedef enum {
    _XM_FX_E0_AMIGA_FILTER,
	_XM_FX_E1_FINE_PORTA_UP,
	_XM_FX_E2_FINE_PORTA_DOWN,
	_XM_FX_E3_SET_GLISS_CONTROL,
	_XM_FX_E4_SET_VIBRATO_CONTROL,
	_XM_FX_E5_SET_FINETUNE,
	_XM_FX_E6_SET_LOOP_BEGIN,
	_XM_FX_E7_SET_TREMOLO_CONTROL,
	_XM_FX_E8_UNUSED,
	_XM_FX_E9_RETRIG_NOTE,
	_XM_FX_EA_FINE_VOL_SLIDE_UP,
	_XM_FX_EB_FINE_VOL_SLIDE_DOWN,
	_XM_FX_EC_NOTE_CUT,
	_XM_FX_ED_NOTE_DELAY,
	_XM_FX_EE_PATTERN_DELAY

} _XM_FX_E_List;

typedef enum {
	_XM_VX_6_VOL_SLIDE_DOWN = 6,
	_XM_VX_7_VOL_SLIDE_UP,
	_XM_VX_8_FINE_VOL_SLIDE_DOWN,
	_XM_VX_9_FINE_VOL_SLIDE_UP,
	_XM_VX_A_SET_VIBRATO_SPEED,
	_XM_VX_B_SET_VIBRATO_DEPTH,
	_XM_VX_C_SET_PANNING, /* this is unused; panning is processed elsewhere */
	_XM_VX_D_PAN_SLIDE_LEFT,
	_XM_VX_E_PAN_SLIDE_RIGHT,
	_XM_VX_F_TONE_PORTA
} _XM_VX_List;

static _XM_INLINE void _xm_player_do_vibrato(_XM_Channel *ch) {

	_XM_PlayerData *p=_xm_player;

	/* 0 .. 32 index, both pos and neg */
	xm_u8 idx=_XM_ABS(ch->fx_4_vibrato_phase>>2)&0x1F;
	xm_s16 modifier=0;
	switch(ch->fx_4_vibrato_type) {

		case 0: {
			/* Sine Wave */
			modifier=_xm_fx_4_and_7_table[idx];
		} break;
		case 1:{
			/* Square Wave */
			modifier=0xFF;
		} break;
		case 2:{
			/* Saw Wave */
			modifier=(xm_s16)idx<<3;
			if (ch->fx_4_vibrato_phase<0)
				modifier=0xFF-modifier;
		} break;
		case 3:{
			/* can't figure out this */
		} break;
	}

	modifier*=ch->fx_4_vibrato_depth;
	modifier>>=5;

	ch->tickrel_period+=(ch->fx_4_vibrato_phase<0)?-modifier:modifier;

	if (p->tick_counter>0)
		ch->fx_4_vibrato_phase+=ch->fx_4_vibrato_speed;


}
static _XM_INLINE void _xm_player_do_tone_porta(_XM_Channel *ch) {

	_XM_PlayerData *p=_xm_player;

	if (ch->porta_period!=0 && p->tick_counter) {


		/* porta period must be valid */

		xm_s32 dist=(xm_s32)ch->period-(xm_s32)ch->porta_period;

		if (dist==0)
			return; /* nothing to do, we're at same period */

		if (_XM_ABS(dist)<(ch->fx_3_memory<<2)) {
			/* make it reach */
			ch->period=ch->porta_period;
		} else if (dist<0) {
			/* make it raise */
			ch->period+=ch->fx_3_memory<<2;
		} else if (dist>0) {

			ch->period-=ch->fx_3_memory<<2;
		}
	}
}

static _XM_INLINE void _xm_player_do_volume_slide_down(_XM_Channel *ch, xm_u8 val) {
	xm_s8 new_vol = (xm_s8)ch->volume - (xm_s8)val;
	if(new_vol<0)
		new_vol=0;
	ch->volume=new_vol;
}

static _XM_INLINE void _xm_player_do_volume_slide_up(_XM_Channel *ch, xm_u8 val) {
	xm_s8 new_vol = (xm_s8)ch->volume + (xm_s8)val;
	if (new_vol>64)
		new_vol=64;
	ch->volume=new_vol;
}


static _XM_INLINE void _xm_player_do_volume_slide(_XM_Channel *ch) {

	_XM_PlayerData *p=_xm_player;

	xm_u8 param=(ch->parameter>0)?ch->parameter:ch->fx_A_memory;
	xm_u8 param_up=param>>4;
	xm_u8 param_down=param&0xF;

	if (p->tick_counter==0)
		return;

	if (ch->parameter)
		ch->fx_A_memory=ch->parameter;


	if (param_up) {
		_xm_player_do_volume_slide_up(ch, param_down);
	} else if (param_down) { /* up has priority over down */
		_xm_player_do_volume_slide_down(ch, param_down);
	}

}


static _XM_INLINE void _xm_player_process_effects_and_envelopes()
{
	_XM_PlayerData *p=_xm_player;
	int i;
	int channels=(p->song->flags&XM_SONG_FLAGS_MASK_CHANNELS_USED)+1;

	for (i=0;i<channels;i++)
	{
		_XM_Channel *ch=&p->channel[i];

		/* reset pre-effect variables */
		ch->tickrel_period=0;
		ch->tickrel_volume=0;


		/* PROCESS VOLUME COLUMN FOR EFFECT COMMANDS */
		xm_u8 vcmd   = ch->volume_command  >> 4;
		xm_u8 vparam = ch->volume_command  & 0xF;
		switch(vcmd) {
			case _XM_VX_6_VOL_SLIDE_DOWN: {
				_xm_player_do_volume_slide_down(ch, vparam);
			} break;

			case _XM_VX_7_VOL_SLIDE_UP: {
				_xm_player_do_volume_slide_up(ch, vparam);
			} break;

			case _XM_VX_8_FINE_VOL_SLIDE_DOWN: {
				if (p->tick_counter!=0)
					break;
				_xm_player_do_volume_slide_down(ch, vparam);
			} break;

			case _XM_VX_9_FINE_VOL_SLIDE_UP: {
				if (p->tick_counter!=0)
					break;
				_xm_player_do_volume_slide_up(ch, vparam);
			} break;

			case _XM_VX_A_SET_VIBRATO_SPEED: {
				if (p->tick_counter==0) {
					if (vparam)
						ch->fx_4_vibrato_speed=vparam<<2;
				}
				_xm_player_do_vibrato(ch);
			} break;

			case _XM_VX_B_SET_VIBRATO_DEPTH: {
				if (p->tick_counter==0) {
					if (vparam)
						ch->fx_4_vibrato_depth=vparam;
				}
				_xm_player_do_vibrato(ch);
			} break;

			case _XM_VX_C_SET_PANNING: {
			} break;

			case _XM_VX_D_PAN_SLIDE_LEFT: {
				xm_s16 new_pan = (xm_u16)ch->pan - vparam;
				if (new_pan<0)
					new_pan=0;
				ch->pan=(xm_u8)new_pan;
			} break;

			case _XM_VX_E_PAN_SLIDE_RIGHT: {
				xm_s16 new_pan = (xm_u16)ch->pan + vparam;
				if (new_pan>255)
					new_pan=255;
				ch->pan=(xm_u8)new_pan;
			} break;

			case _XM_VX_F_TONE_PORTA: {
				if(vparam)
					ch->fx_3_memory=vparam<<4;

				if (p->tick_counter==0 && ch->row_has_note) {
					ch->porta_period=ch->period;
					ch->period=ch->old_period;
					ch->restart=xm_false;
				}

				_xm_player_do_tone_porta(ch);
			} break;
		}



		/* PROCESS  EFFECT COMMANDS */
		switch(ch->command) {

			case _XM_FX_0_ARPEGGIO: {

				xm_u32 base_period = _xm_player_get_period( ch->real_note , ch->finetune );
				xm_u8 ofs;

				switch(p->tick_counter%3) {
					case 0: ofs=0; break;
					case 1: ofs=ch->parameter>>4; break;
					case 2: ofs=ch->parameter&0xF; break;
				}

				ch->tickrel_period += _xm_player_get_period( ch->real_note+ofs , ch->finetune )-base_period;

			} break;
			case _XM_FX_1_PORTA_UP: {

				xm_u8 param;
				if (p->tick_counter==0)
					break;
				param=(ch->parameter>0)?ch->parameter:ch->fx_1_memory;
				if (ch->parameter)
					ch->fx_1_memory=param;
				ch->period-=param<<2;

			} break;
			case _XM_FX_2_PORTA_DOWN: {

				xm_u8 param;
				if (p->tick_counter==0)
					break;

				param=(ch->parameter>0)?ch->parameter:ch->fx_2_memory;
				if (ch->parameter)
					ch->fx_2_memory=ch->parameter;

				ch->period+=param<<2;

			} break;
			case _XM_FX_3_TONE_PORTA: {

				xm_u8 param=(ch->parameter>0)?ch->parameter:ch->fx_3_memory;
				if (ch->parameter)
					ch->fx_3_memory=param;

				if (p->tick_counter==0 && ch->row_has_note) {

					ch->porta_period=ch->period;
					ch->period=ch->old_period;
					ch->restart=xm_false;
				}

				_xm_player_do_tone_porta(ch);

			} break;
			case _XM_FX_4_VIBRATO: {
				/* reset phase on new note */
				if (p->tick_counter==0) {

					if (ch->row_has_note)
						ch->fx_4_vibrato_phase=0;
					if (ch->parameter&0xF)
						ch->fx_4_vibrato_depth=ch->parameter&0xF;
					if (ch->parameter&0xF0)
						ch->fx_4_vibrato_speed=(ch->parameter&0xF0)>>2;
				}
				_xm_player_do_vibrato(ch);
			} break;

			case _XM_FX_5_TONE_PORTA_AND_VOL_SLIDE: {
				_xm_player_do_volume_slide(ch);
				_xm_player_do_tone_porta(ch);
			} break;

			case _XM_FX_6_VIBRATO_AND_VOL_SLIDE: {

				_xm_player_do_volume_slide(ch);
				_xm_player_do_vibrato(ch);

			} break;
			case _XM_FX_7_TREMOLO: {

				_XM_PlayerData *p=_xm_player;

				/* 0 .. 32 index, both pos and neg */
				xm_u8 idx=_XM_ABS(ch->fx_7_tremolo_phase>>2)&0x1F;
				xm_s16 modifier=0;
				switch(ch->fx_7_tremolo_type) {

					case 0: {
						/* Sine Wave */
						modifier=_xm_fx_4_and_7_table[idx];
					} break;
					case 1:{
						/* Square Wave */
						modifier=0xFF;
					} break;
					case 2:{
						/* Saw Wave */
						modifier=(xm_s16)idx<<3;
						if (ch->fx_7_tremolo_phase<0)
							modifier=0xFF-modifier;
					} break;
					case 3:{
						/* can't figure out this */
					} break;
				}

				modifier*=ch->fx_7_tremolo_depth;
				modifier>>=7;

				ch->tickrel_volume+=(ch->fx_7_tremolo_phase<0)?-modifier:modifier;

				if (p->tick_counter>0)
					ch->fx_7_tremolo_phase+=ch->fx_7_tremolo_speed;

			} break;
			case _XM_FX_8_SET_PANNING: {

				if (p->tick_counter==0) {

					ch->pan=ch->parameter;
				}
			} break;
			case _XM_FX_9_SAMPLE_OFFSET: {

				ch->restart_offset=(xm_u32)ch->parameter<<8;
			} break;
			case _XM_FX_A_VOLUME_SLIDE: {

				_xm_player_do_volume_slide(ch);
			} break;
			case _XM_FX_B_POSITION_JUMP: {

				if (p->tick_counter!=0 || ch->parameter >= p->song->order_count)
					break; /* pointless */
				if (p->force_next_order) {
					/* already forced? just change order */
					p->forced_next_order=ch->parameter;
				} else {
					p->force_next_order=xm_true;
					p->forced_next_order=ch->parameter;
				}

			} break;
			case _XM_FX_C_SET_VOLUME: {

				if (p->tick_counter!=0 || ch->parameter >64)
					break;

				ch->volume=ch->parameter;
			} break;
			case _XM_FX_D_PATTERN_BREAK: {

				if (p->tick_counter!=0)
					break; /* pointless */
				if (p->force_next_order) {
					/* already forced? just change order */
					p->forced_next_order_row=ch->parameter;

				} else {
					p->force_next_order=xm_true;
					p->forced_next_order_row=ch->parameter;
					p->forced_next_order=p->current_order+1;
				}

			} break;
			case _XM_FX_E_SPECIAL: {

				xm_u8 ecmd=ch->parameter>>4;
				xm_u8 eparam=ch->parameter&0xF;
				switch(ecmd) {
					case _XM_FX_E0_AMIGA_FILTER:{

					}break;

					case _XM_FX_E1_FINE_PORTA_UP: {

						xm_u8 param;
						if (p->tick_counter!=0)
							break;
						param=(eparam>0)?eparam:ch->fx_E1_memory;
						if (eparam)
							ch->fx_E2_memory=param;
						ch->period-=param<<2;


					} break;
					case _XM_FX_E2_FINE_PORTA_DOWN: {

						xm_u8 param;
						if (p->tick_counter!=0)
							break;
						param=(eparam>0)?eparam:ch->fx_E2_memory;
						if (eparam)
							ch->fx_E2_memory=param;
						ch->period-=param<<2;

					} break;
					case _XM_FX_E3_SET_GLISS_CONTROL: {

						/* IGNORED, DEPRECATED */
					} break;
					case _XM_FX_E4_SET_VIBRATO_CONTROL: {
						if (p->tick_counter!=0)
							break;

						if (eparam<4)
							ch->fx_4_vibrato_type=eparam;

					} break;
					case _XM_FX_E5_SET_FINETUNE: {

						if (eparam<4)
							ch->finetune=((xm_s8)eparam-8)<<4;
					} break;
					case _XM_FX_E6_SET_LOOP_BEGIN: {

						/* IGNORED, difficult to
						   support in hardware*/
					} break;
					case _XM_FX_E7_SET_TREMOLO_CONTROL: {

						if (p->tick_counter!=0)
							break;

						if (eparam<4)
							ch->fx_7_tremolo_type=eparam;

					} break;
					case _XM_FX_E8_UNUSED: {


					} break;
					case _XM_FX_E9_RETRIG_NOTE: {

						/* this needs more testing... it gets a lot of validations already */

						if (eparam && (p->tick_counter%eparam)==0 && ch->old_period!=0 && ch->sample_ptr && ch->instrument_ptr) {
							ch->restart=xm_true;
							_xm_player_envelope_reset(&ch->volume_envelope);
							_xm_player_envelope_reset(&ch->pan_envelope);
							ch->active=xm_true;

						}
					} break;
					case _XM_FX_EA_FINE_VOL_SLIDE_UP: {
						xm_u8 param;
						if (p->tick_counter!=0)
							break;
						param=(eparam>0)?eparam:ch->fx_EA_memory;
						if (eparam)
							ch->fx_EA_memory=param;

						_xm_player_do_volume_slide_up(ch, param);
					} break;
					case _XM_FX_EB_FINE_VOL_SLIDE_DOWN: {
						xm_u8 param;
						if (p->tick_counter!=0)
							break;
						param=(eparam>0)?eparam:ch->fx_EB_memory;
						if (eparam)
							ch->fx_EB_memory=param;

						_xm_player_do_volume_slide_down(ch, param);
					} break;
					case _XM_FX_EC_NOTE_CUT: {

						if (p->tick_counter==eparam)
							ch->active=xm_false;

					} break;

					case _XM_FX_ED_NOTE_DELAY: {
						if (p->tick_counter !=0 || ch->note_delay >= p->current_speed) {
							break;
						}
						ch->note_delay=eparam;
					} break;

					case _XM_FX_EE_PATTERN_DELAY: {
						if (p->tick_counter!=0)
							break;
						p->pattern_delay=eparam;
					} break;
				}

			} break;
			case _XM_FX_F_SET_SPEED_AND_TEMPO: {

				if (p->tick_counter!=0)
					break; /* pointless */

				if (ch->parameter==0)
					break;
				else if (ch->parameter<0x1F)
					p->current_speed=ch->parameter;
				else
					p->current_tempo=ch->parameter;

			} break;
			case _XM_FX_G_SET_GLOBAL_VOLUME: {

				if (p->tick_counter!=0 || ch->parameter>64)
					break; /* pointless */
				p->global_volume=ch->parameter;
			} break;
			case _XM_FX_H_GLOBAL_VOLUME_SLIDE: {

				_XM_PlayerData *p=_xm_player;
				xm_u8 param,param_up,param_down;


				if (p->tick_counter==0)
					return;

				param=(ch->parameter>0)?ch->parameter:ch->fx_H_memory;
				param_up=param>>4;
				param_down=param&0xF;
				if (ch->parameter)
					ch->fx_H_memory=ch->parameter;


				if (param_up) {

					xm_s8 new_vol = (xm_s8)p->global_volume + (xm_s8)param_up;
					if (new_vol>64)
						new_vol=64;
					p->global_volume=new_vol;

				} else if (param_down) { /* up has priority over down */

					xm_s8 new_vol = (xm_s8)p->global_volume - (xm_s8)param_down;
					if (new_vol<0)
						new_vol=0;
					p->global_volume=new_vol;
				}

			} break;
			case _XM_FX_I_UNUSED: {


			} break;
			case _XM_FX_J_UNUSED: {


			} break;
			case _XM_FX_K_KEY_OFF: {
				ch->note_off=xm_true;
			} break;
			case _XM_FX_L_SET_ENVELOPE_POS: {

				/* this is weird, should i support it? Impulse Tracker doesn't.. */
			} break;
			case _XM_FX_M_UNUSED: {


			} break;
			case _XM_FX_N_UNUSED: {


			} break;
			case _XM_FX_O_UNUSED: {


			} break;
			case _XM_FX_P_PAN_SLIDE: {

				_XM_PlayerData *p=_xm_player;
				xm_u8 param,param_up,param_down;

				if (p->tick_counter==0)
					return;

				param=(ch->parameter>0)?ch->parameter:ch->fx_P_memory;
				param_up=param>>4;
				param_down=param&0xF;
				if (ch->parameter)
					ch->fx_P_memory=ch->parameter;


				if (param_up) {

					xm_s16 new_pan = (xm_s16)ch->pan + (xm_s16)param_up;
					if (new_pan>255)
						new_pan=255;
					ch->pan=new_pan;

				} else if (param_down) { /* up has priority over down */

					xm_s16 new_pan = (xm_s16)ch->pan - (xm_s16)param_down;
					if (new_pan<0)
						new_pan=0;
					ch->pan=new_pan;
				}

			} break;
			case _XM_FX_R_RETRIG: {


			} break;
			case _XM_FX_S_UNUSED: {


			} break;
			case _XM_FX_T_TREMOR: {


			} break;
			case _XM_FX_U_UNUSED: {


			} break;
			case _XM_FX_V_UNUSED: {


			} break;
			case _XM_FX_W_UNUSED: {


			} break;
			case _XM_FX_X_EXTRA_FINE_PORTA: {


			} break;
			case _XM_FX_Y_UNUSED: {


			} break;
			case _XM_FX_Z_FILTER : {


			} break;
		}

		/* avoid zero or negative period  */

		if (ch->period<=0)
			ch->active=xm_false;

		/* process note delay */

		if (ch->note_delay) {
			continue;
		}

		if (ch->active) {
			_xm_player_process_envelope( ch, &ch->volume_envelope, &ch->instrument_ptr->volume_envelope);
			_xm_player_process_envelope( ch, &ch->pan_envelope, &ch->instrument_ptr->pan_envelope);
		}


	}
}

static _XM_INLINE void _xm_player_update_mixer() {

	_XM_PlayerData *p=_xm_player;
	int i;
	int channels=(p->song->flags&XM_SONG_FLAGS_MASK_CHANNELS_USED)+1;
	int mixer_max_channels=_xm_mixer->get_features()&XM_MIXER_FEATURE_MAX_VOICES_MASK;

	for (i=0;i<channels;i++) {

		_XM_Channel *ch=&p->channel[i];

		if (!ch->active) {

			if (_xm_mixer->voice_is_active(i))
				_xm_mixer->voice_stop(i);

			continue;
		}

		/* reset pre-effect variables */
		if (i>=mixer_max_channels)
			continue; /* channel unsupported */

		if (ch->note_delay) { /* if requested delay, don't do a thing */
			ch->note_delay--;
			continue;
		}

		/* start voice if needed */
		if (ch->restart) {

			_xm_mixer->voice_start(i, ch->sample_ptr->sample_id, ch->restart_offset);
			ch->restart=xm_false;
/*			printf("TOMIXER starting channel %i, with sample %i, offset %i\n",i,ch->sample,ch->restart_offset); */
		}

		/* check channel activity */

		if (ch->active && !_xm_mixer->voice_is_active(i)) {

			ch->active=xm_false;
/*			printf("TOMIXER TERMINATED channel %i FROM mixer\n",i); */
			continue;

		}

		if (!ch->active && _xm_mixer->voice_is_active(i)) {

			_xm_mixer->voice_stop(i);
/*			printf("TOMIXER TERMINATED channel %i TO mixer\n",i); */
			continue;
		}



		{ /* VOLUME PROCESS */
			xm_s16 final_volume;
			final_volume=((ch->volume+ch->tickrel_volume) * 255) >> 6;

			if (ch->instrument_ptr->volume_envelope.flags&XM_ENVELOPE_ENABLED) {

				final_volume = (final_volume * ch->volume_envelope.value) >> 6;

				if (ch->note_off) {

					xm_u16 fade_down=ch->instrument_ptr->fadeout;

					if (fade_down>0xFFF || fade_down>=ch->fadeout)
						ch->fadeout=0;
					else
						ch->fadeout-=fade_down;

					if (ch->fadeout==0) {

						ch->active=xm_false;
					}

				}
			}

			final_volume =  (final_volume * p->global_volume ) >> 6;


			if (final_volume>255)
				final_volume=255;

			if (final_volume<0)
				final_volume=0;

			_xm_mixer->voice_set_volume( i, final_volume );
		}

		{ /* PAN PROCESS */

			xm_s16 final_pan=ch->pan;

			if (ch->instrument_ptr->pan_envelope.flags&XM_ENVELOPE_ENABLED) {
				final_pan=final_pan+((xm_s16)ch->pan_envelope.value-32)*(128-_XM_ABS(final_pan-128))/32;
			}

			if (final_pan<0)
				final_pan=0;
			if (final_pan>255)
				final_pan=255;

			_xm_mixer->voice_set_pan( i, final_pan );
		}

		_xm_mixer->voice_set_speed( i, _xm_player_get_frequency( ch->period + ch->tickrel_period ) );

	}

	_xm_mixer->set_process_callback_interval( 2500000/p->current_tempo ); /* re convert tempo to usecs */
}
static void _xm_player_process_tick() {


	_XM_PlayerData *p=_xm_player;


	if (!p) {
		_XM_ERROR_PRINTF("Player Unconfigured (missing player)!");
		return;
	}
	if (!p->song) {

		return; /* if song set is null, don't do a thing */
	}
	if (!p->active) {

		return;
	}

	/* Check Ticks */

	if (p->tick_counter >= (p->current_speed+p->pattern_delay)) {

		/* Tick Reaches Zero, process row */

		p->tick_counter=0;
		p->pattern_delay=0;

		/* change order, as requested by some command */
		if (p->force_next_order) {

			if (p->forced_next_order<p->song->order_count) {
				p->current_order=p->forced_next_order;
				p->current_row=p->forced_next_order_row;
				p->current_pattern=p->song->order_list[p->current_order];
			}
			p->force_next_order=xm_false;
			p->forced_next_order_row=0;

		}

		/** process a row of nnotes **/
		_xm_process_notes();

		/* increment row and check pattern/order changes */
		p->current_row++;

		if (p->current_row>=_xm_player_get_pattern_length( p->current_pattern )) {

			p->current_row=0;
			p->current_order++;
			if (p->current_order>=p->song->order_count)
				p->current_order=p->song->restart_pos;


			p->current_pattern=p->song->order_list[p->current_order];

		}



	}

	/** PROCESS EFFECTS AND ENVELOPES**/

	_xm_player_process_effects_and_envelopes();

	/** UPDATE MIXER **/

	_xm_player_update_mixer();

	/** DECREMENT TICK */
	p->tick_counter++;

/*	_XM_DEBUG_PRINTF("playing row %i, order %i\n",p->current_row,p->current_order); */
}


void xm_player_set_song(XM_Song *p_song) {

	if (!_xm_player) {
		_XM_ERROR_PRINTF("NO PLAYER CONFIGURED");
		return;
	}

	if (_xm_player->active) {
		xm_player_stop();
	}

	_XM_AUDIO_LOCK;
	_xm_player->song=p_song;
	_xm_player_reset();
	_XM_AUDIO_UNLOCK;
}

void xm_player_play() {

	if (!_xm_player->song) {
		_XM_ERROR_PRINTF("NO SONG SET IN PLAYER");
		return;
	}

	xm_player_stop(); /* stop if playing */

	_XM_AUDIO_LOCK
	_xm_player_reset();
	_xm_player->active=xm_true;
	_XM_AUDIO_UNLOCK


}
void xm_player_stop() 
{
	_XM_AUDIO_LOCK

	_xm_player_reset();
	_xm_player->active=xm_false;

	_XM_AUDIO_UNLOCK

	if (_xm_mixer) {
		_xm_mixer->reset_voices();
	}
}

/*******************************
************SOME SETTINGS******
*******************************/

void xm_set_mixer(XM_Mixer *p_mixer) {

	if (_xm_memory==0) {
		_XM_ERROR_PRINTF("MEMORY MANAGER NOT CONFIGURED");
		return; /* Mixer Alrady Configured */
	}

	if (_xm_mixer!=0) {
		_XM_ERROR_PRINTF("XM MIXER ALREADY CONFIGURED");
		return; /* Mixer Alrady Configured */
	}
	_xm_mixer=p_mixer;
	_xm_mixer->set_process_callback( _xm_player_process_tick );

}

void xm_set_memory_manager(XM_MemoryManager *p_memory_manager) {
	if (_xm_memory) {

		_XM_ERROR_PRINTF("XM MEMORY MANAGER ALREADY CONFIGURED\n");
		return;
	}

	_xm_memory=p_memory_manager;


	/* create player */
	_XM_DEBUG_PRINTF("allocmem %i bytes\n", (int)sizeof(_XM_PlayerData));
	_xm_player = (_XM_PlayerData*)_xm_memory->alloc( sizeof(_XM_PlayerData) , XM_MEMORY_ALLOC_PLAYER );
	_xm_player->song=0;

	_XM_DEBUG_PRINTF("resetplayer\n");
	_xm_player_reset();
}

XM_MemoryManager *xm_get_memory_manager() {
        return _xm_memory;
}
/*******************************
************SONG***************
*******************************/

XM_Song *xm_song_alloc() {

	XM_Song *song=0;

	if (!_xm_memory) {
		_XM_ERROR_PRINTF("XM MEMORY MANAGER NOT CONFIGURED");
		return 0;
	}
	
	song= (XM_Song*)_xm_memory->alloc( sizeof(XM_Song), XM_MEMORY_ALLOC_SONG_HEADER );

	_xm_zero_mem( song, sizeof(XM_Song) );

	song->tempo=125;
	song->speed=6;

	return song;
}
void xm_song_free(XM_Song *p_song) {

	if (!_xm_memory) {
		_XM_ERROR_PRINTF("XM MEMORY MANAGER NOT CONFIGURED");
		return;
	}

	xm_loader_free_song(p_song);

	_xm_memory->free(p_song,XM_MEMORY_ALLOC_SONG_HEADER);
}

/*******************************
************LOADER**************
*******************************/

static XM_FileIO *_xm_fileio=0;
static xm_bool _xm_loader_recompress_samples=xm_false;

void xm_loader_set_recompress_all_samples(xm_bool p_enable) {
	
	_xm_loader_recompress_samples=xm_true;
}
void xm_loader_set_fileio( XM_FileIO *p_fileio ) {

	_xm_fileio=p_fileio;
}


static void _xm_clear_song( XM_Song *p_song, xm_s32 p_pattern_count, xm_s32 p_instrument_count ) {

	int i;

	/**
	  * Erasing must be done in the opposite way as creating.
	  * This way, allocating memory and deallocating memory from a song can work like a stack,
	  * to simplify the memory allocator in some implementations.
	  */

	_XM_AUDIO_LOCK

	/* Instruments first */

	for (i=(p_instrument_count-1);i>=0;i--) {

		if (i>=p_song->instrument_count) {

			_XM_ERROR_PRINTF("Invalid clear instrument amount specified.");
			continue;
		}

		if ( p_song->instrument_data[i] ) {

			XM_Instrument *ins = p_song->instrument_data[i];
			int j;

			for (j=(ins->sample_count-1);j>=0;j--) {

				if (ins->samples[j].sample_id!=XM_INVALID_SAMPLE_ID) {

					_xm_mixer->sample_unregister( ins->samples[j].sample_id );
				}
			}

			if (ins->samples) {
				_xm_memory->free( ins->samples, XM_MEMORY_ALLOC_INSTRUMENT );
			}
			_xm_memory->free( p_song->instrument_data[i], XM_MEMORY_ALLOC_INSTRUMENT );
		}
	}

	if ( p_song->instrument_data && p_instrument_count>=0) {

		_xm_memory->free( p_song->instrument_data, XM_MEMORY_ALLOC_INSTRUMENT );
		p_song->instrument_data=0;
	}



	/* Patterns Last */
	for (i=(p_pattern_count-1);i>=0;i--) {

		if (i>=p_song->pattern_count) {

			_XM_ERROR_PRINTF("Invalid clear pattern amount specified.");
			continue;
		}

		if ( p_song->pattern_data[i] ) {
			_xm_memory->free( p_song->pattern_data[i], XM_MEMORY_ALLOC_PATTERN );
		}
	}

	if ( p_song->pattern_data && p_pattern_count) {

		_xm_memory->free( p_song->pattern_data, XM_MEMORY_ALLOC_PATTERN );
		p_song->pattern_data=0;
	}

	_XM_AUDIO_UNLOCK


}

/**
 * Recompress Pattern from XM-Compression to Custom (smaller/faster to read) compression
 * Read pattern.txt included with this file for the format.
 * @param p_rows Rows for source pattern
 * @param channels Channels for source pattern
 * @param p_dst_data recompress target pointer, if null, will just compute size
 * @return size of recompressed buffer
 */



static xm_u32 _xm_recompress_pattern(xm_u16 p_rows,xm_u8 p_channels,void * p_dst_data) {

#define _XM_COMP_ADD_BYTE(m_b) do {\
	if (dst_data)\
		dst_data[data_size]=m_b;\
	++data_size;\
	} while(0)\

	XM_FileIO *f=_xm_fileio;

	xm_u8 *dst_data=(xm_u8*)p_dst_data;
	xm_u32 data_size=0;
	int i,j,k;
	xm_u8 caches[32][5];
	xm_s8 last_channel=-1;
	xm_u16 last_row=0;



	for (i=0;i<32;i++) {

		caches[i][0]=_XM_FIELD_EMPTY;
		caches[i][1]=_XM_FIELD_EMPTY;
		caches[i][2]=_XM_FIELD_EMPTY;
		caches[i][3]=_XM_FIELD_EMPTY;
		caches[i][4]=0; /* only values other than 0 are read for this as cache */
	}
	for(j=0;j<p_rows;j++)
		for(k=0;k<p_channels;k++) {

		xm_u8 note=_XM_FIELD_EMPTY;
		xm_u8 instrument=0; /* Empty XM Instrument */
		xm_u8 volume=0; /* Empty XM Volume */
		xm_u8 command=_XM_FIELD_EMPTY;
		xm_u8 parameter=0;
		xm_u8 arb=0; /* advance one row bit */

		xm_u8 aux_byte=0;

		xm_u8 cache_bits=0;
		xm_u8 new_field_bits=0;

		aux_byte=f->get_u8();
		if (!(aux_byte&0x80)) {

			note=aux_byte;
			aux_byte=0xFE; /* if bit 7 not set, read all of them except the note */
		}

		if (aux_byte&1) note=f->get_u8();
		if (aux_byte&2) instrument=f->get_u8();
		if (aux_byte&4) volume=f->get_u8();
		if (aux_byte&8) command=f->get_u8();
		if (aux_byte&16) parameter=f->get_u8();



		if (note>97)
			note=_XM_FIELD_EMPTY;

		if (instrument==0)
			instrument=_XM_FIELD_EMPTY;
		else
			instrument--;

		if (volume<0x10)
			volume=_XM_FIELD_EMPTY;
		else volume-=0x10;

		if (command==0 && parameter==0) {
			/* this equals to nothing */
			command=_XM_FIELD_EMPTY;
		}


		/** COMPRESS!!! **/

		/* Check differences with cache and place them into bits */
		cache_bits|=(note!=_XM_FIELD_EMPTY && note==caches[k][0])?_XM_COMP_NOTE_BIT:0;
		cache_bits|=(instrument!=_XM_FIELD_EMPTY && instrument==caches[k][1])?_XM_COMP_INSTRUMENT_BIT:0;
		cache_bits|=(volume!=_XM_FIELD_EMPTY && volume==caches[k][2])?_XM_COMP_VOLUME_BIT:0;
		cache_bits|=(command!=_XM_FIELD_EMPTY && command==caches[k][3])?_XM_COMP_COMMAND_BIT:0;
		cache_bits|=(parameter!=0 && parameter==caches[k][4])?_XM_COMP_PARAMETER_BIT:0;

		/* Check new field values and place them into bits and cache*/

		if (note!=_XM_FIELD_EMPTY && !(cache_bits&_XM_COMP_NOTE_BIT)) {

			new_field_bits|=_XM_COMP_NOTE_BIT;
			caches[k][0]=note;

		}


		if (instrument!=_XM_FIELD_EMPTY && !(cache_bits&_XM_COMP_INSTRUMENT_BIT)) {

			new_field_bits|=_XM_COMP_INSTRUMENT_BIT;
			caches[k][1]=instrument;

		}

		if (volume!=_XM_FIELD_EMPTY && !(cache_bits&_XM_COMP_VOLUME_BIT)) {

			new_field_bits|=_XM_COMP_VOLUME_BIT;
			caches[k][2]=volume;

		}

		if (command!=_XM_FIELD_EMPTY && !(cache_bits&_XM_COMP_COMMAND_BIT)) {

			new_field_bits|=_XM_COMP_COMMAND_BIT;
			caches[k][3]=command;

		}

		if (parameter!=0 && !(cache_bits&_XM_COMP_PARAMETER_BIT)) {

			new_field_bits|=_XM_COMP_PARAMETER_BIT;
			caches[k][4]=parameter;

		}



		if (!new_field_bits && !cache_bits) {
			continue; /* nothing to store, empty field */
		}



		/* Seek to Row */

		if (j>0 && last_row==(j-1) && last_channel!=k) {
			arb=0x80;
			last_row=j;

		} else while (last_row<j) {



			xm_u16 diff=j-last_row;

			if (diff>0x20) {
				/* The maximum value of advance_rows command is 32 (0xFF)
				   so, if the rows that are needed to advance are greater than that,
				   advance 32, then issue more advance_rows commands */
				_XM_COMP_ADD_BYTE( 0xFF ); /* Advance 32 rows */
				last_row+=0x20;
			} else {


				_XM_COMP_ADD_BYTE( 0xE0+(diff-1) ); /* Advance needed rows */
				last_row+=diff;
			}

			last_channel=0; /* advancing rows sets the last channel to zero */
		}
		/* Seek to Channel */



		if (last_channel!=k) {

			_XM_COMP_ADD_BYTE( arb|k );
		}

		if (cache_bits) {

			_XM_COMP_ADD_BYTE( cache_bits|(1<<5) );
		}

		if (new_field_bits) {

			_XM_COMP_ADD_BYTE( new_field_bits|(2<<5) );
			for (i=0;i<5;i++) {

				if (new_field_bits&(1<<i)) {

					_XM_COMP_ADD_BYTE( caches[k][i] );
				}
			}
		}

	}

	_XM_COMP_ADD_BYTE( 0x60 ); /* end of pattern */

#undef _XM_COMP_ADD_BYTE

	return data_size;
}

static void _xm_loader_recompress_sample(XM_SampleData *p_sample_data) {
	
	int i,step_idx=0,prev=0;
	XM_FileIO *f=_xm_fileio;
	xm_u8 *out = (xm_u8*)p_sample_data->data;
	int len=p_sample_data->length;
	xm_s16 xm_prev=0;
	
	if (len&1)
		len++;
	
	/* initial value is zero */
	*(out++) =0;
	*(out++) =0;
	/* Table index initial value */	
	*(out++) =0;
	/* unused */	
	*(out++) =0;
	
	
#define _XM_GET_SAMPLE16 \
((p_sample_data->format==XM_SAMPLE_FORMAT_PCM16)?(xm_s16)f->get_u16():(((xm_s16)((xm_s8)f->get_u8()))<<8))
	
	/*p_sample_data->data = (void*)malloc(len);
	xm_s8 *dataptr=(xm_s8*)p_sample_data->data;*/
	
	
	
	for (i=0;i<len;i++) {
		int step,diff,vpdiff,mask;
		xm_u8 nibble;
		xm_s16 xm_sample = ((i==p_sample_data->length)?0:(_XM_GET_SAMPLE16));
		xm_sample=xm_sample+xm_prev;
		xm_prev=xm_sample;
	
		diff = (int)xm_sample - prev ;

		nibble=0 ;
		step =  _xm_ima_adpcm_step_table[ step_idx ];
		vpdiff = step >> 3 ;
		if (diff < 0) {	
			nibble=8;
			diff=-diff ;
		}
		mask = 4 ;
		while (mask) {	
		
			if (diff >= step) {
				
				nibble |= mask;
				diff -= step;
				vpdiff += step;
			}
		
			step >>= 1 ;
			mask >>= 1 ;
		};

		if (nibble&8)
			prev-=vpdiff ;
		else
			prev+=vpdiff ;

		if (prev > 32767) {
			prev=32767;
		} else if (prev < -32768) {
			prev = -32768 ;
		}

		step_idx += _xm_ima_adpcm_index_table[nibble];
		if (step_idx< 0)
			step_idx= 0 ;
		else if (step_idx> 88)
			step_idx= 88 ;

		
		if (i&1) {
			*out|=nibble<<4;
			out++;
		} else {
			*out=nibble;
		}
		/*dataptr[i]=prev>>8;*/
		
	
	}
	
	p_sample_data->format=XM_SAMPLE_FORMAT_IMA_ADPCM;	
	return;
/*	
	for (i=0;i<len;i++) {
		int step,diff,signed_nibble,p;
		xm_u8 nibble;
		int xm_sample = ((i==p_sample_data->length)?0:(_XM_GET_SAMPLE16)) + xm_prev;
		xm_prev=xm_sample;
				
		step = _xm_ima_adpcm_step_table[ step_idx ];
		diff =  xm_sample - prev; 
		nibble = (_XM_ABS(diff)<<2)/step;
		signed_nibble=(diff<0)?-nibble:nibble;
		
		if (nibble>7) 
			nibble=7;
		step_idx+=_xm_ima_adpcm_index_table[nibble];
		
		if (step_idx<0) 
			step_idx=0;
		if (step_idx>88)
			step_idx=88;
				
		if (diff<0)
			nibble|=8;
		
		if (i&1) {
			*out|=nibble<<4;
			out++;
		} else {
			*out=nibble;
		}
		
		p = (2 * signed_nibble + 1) * step / 4;
		prev += p;
		
		if (prev<-0x8000) {
			printf("clip %i up\n",i);
			printf("clip original %i, adpcmd  %i\n",xm_sample,prev);
			prev=-0x8000;
		}
		if (prev>0x7fff) {
			prev=0x7fff;
			printf("clip %i down\n",i);
			printf("clip original %i, adpcmd  %i\n",xm_sample,prev);
			
		}
	}
*/	
	
}

static XM_LoaderError _xm_loader_open_song_custom( const char *p_filename, XM_Song *p_song, xm_bool p_load_music, xm_bool p_load_instruments )
{
	int i,j,k;
	XM_FileIO *f=_xm_fileio;

	_XM_DEBUG_PRINTF("\n*** LOADING XM: %s ***\n\n",p_filename);

	if (!p_load_music && !p_load_instruments)
		return XM_LOADER_OK; /* nothing to do... */

	if (!f || !_xm_mixer || !_xm_memory ) {
		/* Check wether we have everything needed to go */
		if (!f)
			_XM_ERROR_PRINTF("MISSING FILEIO");
		if (!_xm_mixer)
			_XM_ERROR_PRINTF("MISSING MIXER");
		if (!_xm_memory)
			_XM_ERROR_PRINTF("MISSING MEMORY");

		return XM_LOADER_UNCONFIGURED;
	}

	if (f->in_use()) {

		return XM_LOADER_ERROR_FILEIO_IN_USE;
	}

	if (f->open(p_filename, xm_false)==XM_FILE_ERROR_CANT_OPEN) {

		return XM_LOADER_ERROR_FILE_CANT_OPEN;
	};

	_xm_clear_song( p_song, p_load_music?p_song->pattern_count:-1, p_load_instruments?p_song->instrument_count:-1 );

    /**************************************
	LOAD Identifier
	***************************************/
	{


		xm_u8 idtext[18];
		f->get_byte_array(idtext,17);
		idtext[17]=0;

		/** TODO validate identifier **/


	}

	/**************************************
	LOAD Header
	***************************************/
	{

		xm_u8 hex1a;
		xm_u16 version;
		xm_u32 headersize;
		f->get_byte_array((xm_u8*)p_song->name,20);

		p_song->name[20]=0;

		_XM_DEBUG_PRINTF("Song Name: %s\n",p_song->name);

		hex1a=f->get_u8();
		if (hex1a!=0x1A) { /* XM "magic" byte.. */

			f->close();
			return XM_LOADER_ERROR_FILE_UNRECOGNIZED;
		}

		for (i=0;i<20;i++) /* skip trackername */
			f->get_u8();


		version=f->get_u16();

		headersize=f->get_u32();

		if (p_load_music) {
			xm_u8 chans;

			p_song->order_count=f->get_u16();

			p_song->restart_pos=f->get_u16();

			if (p_song->restart_pos>=p_song->order_count)
				p_song->restart_pos=0;

			chans=f->get_u16();
			if (chans>32) {

				_XM_ERROR_PRINTF("Invalid Number of Channels: %i > 32",chans);
				return XM_LOADER_ERROR_FILE_CORRUPT;
			}
			p_song->flags=(chans-1)&0x1F; /* use 5 bits, +1 */

			p_song->pattern_count=f->get_u16();

			if (p_load_instruments)
				p_song->instrument_count=f->get_u16();
			else
				f->get_u16(); /* ignore */

			if ( f->get_u16() ) /* flags. only linear periods */
				p_song->flags|=XM_SONG_FLAGS_LINEAR_PERIODS;


			p_song->speed=f->get_u16();
			p_song->tempo=f->get_u16();

			f->get_byte_array(p_song->order_list,256);

#ifdef _XM_DEBUG
			_XM_DEBUG_PRINTF("Song Header:\n");
			_XM_DEBUG_PRINTF("\tChannels: %i\n",chans);
			_XM_DEBUG_PRINTF("\tOrders: %i\n",p_song->order_count);
			_XM_DEBUG_PRINTF("\tPatterns: %i\n",p_song->pattern_count);
			_XM_DEBUG_PRINTF("\tInstruments: %i\n",p_song->instrument_count);
			_XM_DEBUG_PRINTF("\tRestart At: %i\n",p_song->restart_pos);
			_XM_DEBUG_PRINTF("\tTempo: %i\n",p_song->tempo);
			_XM_DEBUG_PRINTF("\tSpeed: %i\n",p_song->speed);
			_XM_DEBUG_PRINTF("\tOrders: ");

			for (i=0;i<p_song->order_count;i++) {
				if (i>0)
					_XM_DEBUG_PRINTF(", ");
				_XM_DEBUG_PRINTF("%i",p_song->order_list[i]);

			}

			_XM_DEBUG_PRINTF("\n");

#endif
		} else {

			f->get_u16(); /* skip order count */
			f->get_u16(); /* skip restart pos */
			f->get_u16(); /* skip flags */
			f->get_u16(); /* skip pattern count */
			p_song->instrument_count=f->get_u16();
			/* skip to end of header */
			_XM_DEBUG_PRINTF("Skipping Header.. \n");
			_XM_DEBUG_PRINTF("\tInstruments: %i \n",p_song->instrument_count);
		}

		while (f->get_pos() < (headersize+60))
			f->get_u8(); /* finish reading header */

	}

        /**************************************
	LOAD Patterns
	***************************************/

	if (p_load_music && p_song->pattern_count) {

		p_song->pattern_data = (xm_u8**)_xm_memory->alloc( sizeof(xm_u8*)*p_song->pattern_count , XM_MEMORY_ALLOC_PATTERN );

		if (!p_song->pattern_data) { /* Handle OUT OF MEMORY */
			/* _xm_clear_song( p_song, -1, -1 ); pointless */
			f->close();
			return XM_LOADER_ERROR_OUT_OF_MEMORY;
		}

		_xm_zero_mem(p_song->pattern_data,sizeof(xm_u8*)*p_song->pattern_count);

	}

	_XM_DEBUG_PRINTF("\n\n");

	for (i=0;i<p_song->pattern_count;i++) {

		xm_u32 _ofs=f->get_pos(); /* current file position */
		xm_u32 header_len=f->get_u32(); /* pattern header len */
		xm_u8 packing=f->get_u8(); /* if packing = 1, this pattern is pre-packed */
		xm_u16 rows=f->get_u16(); /* rows */
		xm_u16 packed_data_size=f->get_u16(); /* pattern header len */


		_XM_DEBUG_PRINTF("Pattern: %i\n",i);
		_XM_DEBUG_PRINTF("\tHeader Len: %i\n",header_len);
		_XM_DEBUG_PRINTF("\tRows: %i\n",rows);
		_XM_DEBUG_PRINTF("\tPacked Size: %i\n",packed_data_size);

		while (f->get_pos() < (_ofs+header_len))
			f->get_u8(); /* finish reading header */

		if (p_load_music) {

			if (packed_data_size==0) {
				p_song->pattern_data[i]=0;

			} else if (packing==1) { /* pre packed pattern */

				xm_u8 *pdata=0;
				pdata = (xm_u8 *)_xm_memory->alloc( packed_data_size , XM_MEMORY_ALLOC_PATTERN );

				if (!pdata) { /* Handle OUT OF MEMORY */

					_xm_clear_song( p_song, i, -1 );
					f->close();
					return XM_LOADER_ERROR_OUT_OF_MEMORY;
				}

				f->get_byte_array(pdata,packed_data_size);
			} else { /* pack on the fly while reading */

				xm_u32 _pack_begin_pos=f->get_pos();

				xm_u32 repacked_size=_xm_recompress_pattern( rows, (p_song->flags&0x1f)+1 , 0 ); /* just calculate size */
				xm_u8 *pdata=0;

				_XM_DEBUG_PRINTF("\tRePacked Size: %i\n",repacked_size);

				f->seek_pos(_pack_begin_pos);

				pdata = (xm_u8 *)_xm_memory->alloc( 1 + repacked_size , XM_MEMORY_ALLOC_PATTERN );


				if (!pdata) { /* Handle OUT OF MEMORY */

					_xm_clear_song( p_song, i, -1 );
					f->close();
					return XM_LOADER_ERROR_OUT_OF_MEMORY;
				}

				pdata[0]=rows-1; /* first byte is rows */

				/* on the fly recompress */
				_xm_recompress_pattern( rows, (p_song->flags&0x1f)+1 , &pdata[1] );

				p_song->pattern_data[i]=pdata;
			}

		} /* Just skip to end othersize */

		while (f->get_pos() < (_ofs+header_len+packed_data_size))
			f->get_u8(); /* finish reading header */
	}

       /**************************************
	LOAD INSTRUMENTS!
       ***************************************/

	if (p_load_instruments && p_song->instrument_count) {

		p_song->instrument_data = (XM_Instrument**)_xm_memory->alloc( sizeof(XM_Instrument*)*p_song->instrument_count , XM_MEMORY_ALLOC_INSTRUMENT );


		if (!p_song->instrument_data) { /* Handle OUT OF MEMORY */

			_xm_clear_song( p_song, p_load_music?p_song->pattern_count:-1, 0 );
			f->close();
			return XM_LOADER_ERROR_OUT_OF_MEMORY;
		}

		_xm_zero_mem(p_song->instrument_data,sizeof(XM_Instrument*)*p_song->instrument_count);

	} else {

		/* Don't load instruments */
		f->close();
		return XM_LOADER_OK;
	}



	for (i=0;i<p_song->instrument_count;i++) {

		xm_u32 	_ofs=f->get_pos();
		xm_u32 header_len=f->get_u32(); /* instrument size */
		xm_u16 samples;
		XM_Instrument *_ins=0;
		xm_u32 sample_header_size;
		for (j=0;j<22;j++)
			f->get_u8(); /* skip name */
		f->get_u8(); /* ignore type */
		samples=f->get_u16();

		_XM_DEBUG_PRINTF("Instrument: %i\n",i);
		_XM_DEBUG_PRINTF("\tHeader Len: %i\n",header_len);

		if (samples == 0 ) { /* empty instrument */
			p_song->instrument_data[i]=0; /* no instrument by default */
			_XM_DEBUG_PRINTF("\tSkipped!\n");

			while((f->get_pos()-_ofs)<header_len)
				f->get_u8(); /* go to end of header */
			continue;
			/** @TODO goto header len **/
		} else if (samples>XM_CONSTANT_MAX_SAMPLES_PER_INSTRUMENT) {

			_XM_ERROR_PRINTF("\tHas invalid sample count: %i\n",samples);

			_xm_clear_song( p_song, p_load_music?p_song->pattern_count:-1, i );
			f->close();
			return XM_LOADER_ERROR_FILE_CORRUPT;

		} else {

			p_song->instrument_data[i]=(XM_Instrument*)_xm_memory->alloc( sizeof(XM_Instrument) , XM_MEMORY_ALLOC_INSTRUMENT );


			if (!p_song->instrument_data[i]) { /* Out of Memory */

				_xm_clear_song( p_song, p_load_music?p_song->pattern_count:-1, i );
				f->close();
				return XM_LOADER_ERROR_OUT_OF_MEMORY;
			}

			_xm_zero_mem(p_song->instrument_data[i],sizeof(XM_Instrument));

		}

		_ins = p_song->instrument_data[i];
		_ins->sample_count=samples;

		/* reset the samples */
		_ins->samples=0;

		sample_header_size=f->get_u32(); /* "sample header size" is redundant, so i ignore it */
		_XM_DEBUG_PRINTF("\tSample Header Size: %i\n",sample_header_size);

		for (j=0;j<48;j++) {
			/* convert to nibbles */
			xm_u8 nb=f->get_u8()&0xF;
			nb|=f->get_u8()<<4;
			_ins->note_sample[j]=nb;
		}

		for (j=0;j<12;j++) {

			xm_u16 ofs=f->get_u16();
			xm_u16 val=f->get_u16();
			_ins->volume_envelope.points[j]=(val<<9)|ofs; /* encode into 16 bits */
		}
		for (j=0;j<12;j++) {

			xm_u16 ofs=f->get_u16();
			xm_u16 val=f->get_u16();
			_ins->pan_envelope.points[j]=(val<<9)|ofs; /* encode into 16 bits */
		}

		_ins->volume_envelope.flags=f->get_u8();
		_ins->pan_envelope.flags=f->get_u8();

		_ins->volume_envelope.sustain_index=f->get_u8();
		_ins->volume_envelope.loop_begin_index=f->get_u8();
		_ins->volume_envelope.loop_end_index=f->get_u8();

		_ins->pan_envelope.sustain_index=f->get_u8();
		_ins->pan_envelope.loop_begin_index=f->get_u8();
		_ins->pan_envelope.loop_end_index=f->get_u8();


		{ /* Volume Envelope Flags */
			xm_u8 env_flags=f->get_u8();
			if ( env_flags&1 )
				_ins->volume_envelope.flags|=XM_ENVELOPE_ENABLED;
			if ( env_flags&2 )
				_ins->volume_envelope.flags|=XM_ENVELOPE_SUSTAIN_ENABLED;
			if ( env_flags&4 )
				_ins->volume_envelope.flags|=XM_ENVELOPE_LOOP_ENABLED;
		}
		{ /* Pan Envelope Flags */
			xm_u8 env_flags=f->get_u8();
			if ( env_flags&1 )
				_ins->pan_envelope.flags|=XM_ENVELOPE_ENABLED;
			if ( env_flags&2 )
				_ins->pan_envelope.flags|=XM_ENVELOPE_SUSTAIN_ENABLED;
			if ( env_flags&4 )
				_ins->pan_envelope.flags|=XM_ENVELOPE_LOOP_ENABLED;
		}

		_ins->vibrato_type = (XM_VibratoType)f->get_u8();
		_ins->vibrato_sweep = f->get_u8();
		_ins->vibrato_depth = f->get_u8();
		_ins->vibrato_rate = f->get_u8();
		_ins->fadeout = f->get_u16();

		f->get_u16(); /* reserved */

#ifdef _XM_DEBUG

		_XM_DEBUG_PRINTF("\tVolume Envelope:\n");
		_XM_DEBUG_PRINTF("\t\tPoints: %i\n", _ins->volume_envelope.flags&XM_ENVELOPE_POINT_COUNT_MASK);
		_XM_DEBUG_PRINTF("\t\tEnabled: %s\n", (_ins->volume_envelope.flags&XM_ENVELOPE_ENABLED)?"Yes":"No");
		_XM_DEBUG_PRINTF("\t\tSustain: %s\n", (_ins->volume_envelope.flags&XM_ENVELOPE_SUSTAIN_ENABLED)?"Yes":"No");
		_XM_DEBUG_PRINTF("\t\tLoop Enabled: %s\n", (_ins->volume_envelope.flags&XM_ENVELOPE_LOOP_ENABLED)?"Yes":"No");

		_XM_DEBUG_PRINTF("\tPan Envelope:\n");
		_XM_DEBUG_PRINTF("\t\tPoints: %i\n", _ins->pan_envelope.flags&XM_ENVELOPE_POINT_COUNT_MASK);
		_XM_DEBUG_PRINTF("\t\tEnabled: %s\n", (_ins->pan_envelope.flags&XM_ENVELOPE_ENABLED)?"Yes":"No");
		_XM_DEBUG_PRINTF("\t\tSustain: %s\n", (_ins->pan_envelope.flags&XM_ENVELOPE_SUSTAIN_ENABLED)?"Yes":"No");
		_XM_DEBUG_PRINTF("\t\tLoop Enabled: %s\n", (_ins->pan_envelope.flags&XM_ENVELOPE_LOOP_ENABLED)?"Yes":"No");

#endif

		while((f->get_pos()-_ofs)<header_len)
			f->get_u8(); /* Skip rest of header */

		/**** SAMPLES *****/

		/* allocate array */
		_ins->samples = (XM_Sample*)_xm_memory->alloc( sizeof(XM_Sample)*samples , XM_MEMORY_ALLOC_SAMPLE );

		if (!_ins->samples) { /* Out of Memory */

			_xm_clear_song( p_song, p_load_music?p_song->pattern_count:-1, i );
			f->close();
			return XM_LOADER_ERROR_OUT_OF_MEMORY;
		}

		/* allocate samples */

		_xm_zero_mem(_ins->samples,sizeof(XM_Sample)*samples);


		{
			XM_SampleData sample_data[XM_CONSTANT_MAX_SAMPLES_PER_INSTRUMENT];
			xm_bool recompress_sample[16]; /* samples to recompress */

			_XM_DEBUG_PRINTF("\tSample_Names:\n");
			/** First pass, Sample Headers **/
			for (j=0;j<samples;j++) {

				XM_Sample *_smp=&_ins->samples[j];

				xm_u32 length = f->get_u32(); /* in bytes */
				xm_u32 loop_start = f->get_u32();
				xm_u32 loop_end = f->get_u32();
				xm_u8 flags;
				_smp->volume = f->get_u8();
				_smp->finetune = (xm_s8)f->get_u8();
				flags = f->get_u8();
				_smp->pan = f->get_u8();
				_smp->base_note = (xm_s8)f->get_u8();
				f->get_u8(); /* reserved */
				recompress_sample[j]=_xm_loader_recompress_samples;
#ifdef _XM_DEBUG
				_XM_DEBUG_PRINTF("\t\t%i- ",j);
				for (k=0;k<22;k++) {
					char n=f->get_u8();
					if (k==0 && n=='@')
						recompress_sample[j]=xm_true;
					_XM_DEBUG_PRINTF("%c",n);
				}
				_XM_DEBUG_PRINTF("\n");
#else
				for (k=0;k<22;k++) {
					char n=f->get_u8();
					if (k==0 && n=='@')
						recompress_sample[j]=xm_true;
				}
#endif

				_XM_DEBUG_PRINTF("\tSample %i:, length %i\n",j,length);

				if (length>0) {
					/* SAMPLE DATA */

					xm_bool pad_sample_mem=(_xm_mixer->get_features()&XM_MIXER_FEATURE_NEEDS_END_PADDING)?xm_true:xm_false;
					
					switch ((flags>>3) & 0x3) {
						/* bit 3 of XM sample flags specify extended (propertary) format */
						case 0: sample_data[j].format=XM_SAMPLE_FORMAT_PCM8; break;
						case 2: sample_data[j].format=XM_SAMPLE_FORMAT_PCM16; break;
						case 1: sample_data[j].format=XM_SAMPLE_FORMAT_IMA_ADPCM; break;
						case 3: sample_data[j].format=XM_SAMPLE_FORMAT_CUSTOM; break;
					}
					switch (flags & 0x3) {

						case 3:
						case 0: sample_data[j].loop_type=XM_LOOP_DISABLED; break;
						case 1: sample_data[j].loop_type=XM_LOOP_FORWARD; break;
						case 2: sample_data[j].loop_type=XM_LOOP_PING_PONG; break;
					}

					sample_data[j].loop_begin = loop_start;
					sample_data[j].loop_end = loop_end;

					sample_data[j].length = length;
					sample_data[j].base_sample_rate=0;

					if (sample_data[j].format==XM_SAMPLE_FORMAT_PCM16) {
						sample_data[j].length/=2; /* cut in half,since length is bytes */
						sample_data[j].loop_begin/=2; /* cut in half,since length is bytes */
						sample_data[j].loop_end/=2; /* cut in half,since length is bytes */
					} else if (sample_data[j].format==XM_SAMPLE_FORMAT_IMA_ADPCM) { 
						
						
						sample_data[j].length-=4; /* remove header size */
						sample_data[j].loop_begin-=4; /* remove header size */
						
						sample_data[j].length*=2; /* make twice,since length
						 is bytes */
						sample_data[j].loop_begin*=2; /* make twice,since length is bytes */
						sample_data[j].loop_end*=2; /* make twice,since length is in bytes */ 			
						
								
					} 			
							
					
					sample_data[j].loop_end+=sample_data[j].loop_begin;
					
					if (recompress_sample[j]) {
						/* ima adpcm uses 2 samples per byte, plus 4 for header */
						length=sample_data[j].length/2+4;
					}
					
					sample_data[j].data = _xm_memory->alloc( length + (pad_sample_mem?4:0), XM_MEMORY_ALLOC_SAMPLE );

					if (!sample_data[j].data) { /* Out of Memory */

						_xm_clear_song( p_song, p_load_music?p_song->pattern_count:-1, i );
						f->close();
						return XM_LOADER_ERROR_OUT_OF_MEMORY;
					}
				} else {

					_XM_DEBUG_PRINTF("\t\tSkipped!\n");
					_smp->sample_id=XM_INVALID_SAMPLE_ID;
					sample_data[j].data=0;
				}

			}

			/** Second pass, Sample Data **/
			for (j=0;j<samples;j++) {

				XM_Sample *_smp=&_ins->samples[j];
				xm_bool pad_sample_mem=(_xm_mixer->get_features()&XM_MIXER_FEATURE_NEEDS_END_PADDING)?xm_true:xm_false;

				if (!sample_data[j].data)
					continue; /* no data in sample, skip it */

				_XM_DEBUG_PRINTF("\tformat  %i\n",sample_data[j].format);
				
				if (recompress_sample[j]) {
							
					_XM_DEBUG_PRINTF("\trecompressing  %i\n",j);
					_xm_loader_recompress_sample(&sample_data[j]);
				} else {
				
					switch (sample_data[j].format) {
	
						case XM_SAMPLE_FORMAT_PCM16: {
							
							xm_s16 *data=(xm_s16 *)sample_data[j].data;
							xm_s16 old=0;
							for (k=0;k<sample_data[j].length;k++) {
								xm_s16 d=(xm_s16)f->get_u16();
								data[k]=d+old;
								old=data[k];
							}
							if (pad_sample_mem) {
								/* interpolation helper */
								/* these make looping smoother */
								switch( sample_data[j].loop_type ) {
	
									case XM_LOOP_DISABLED: data[sample_data[j].length]=0; break;
									case XM_LOOP_FORWARD: data[sample_data[j].length]=data[sample_data[j].loop_begin]; break;
									case XM_LOOP_PING_PONG: data[sample_data[j].length]=data[sample_data[j].loop_end]; break;
	
								}
	
								data[sample_data[j].length+1]=0;
							}
						} break;
						case XM_SAMPLE_FORMAT_PCM8: {
							xm_s8 *data=(xm_s8 *)sample_data[j].data;
							xm_s8 old=0;
							for (k=0;k<sample_data[j].length;k++) {
								xm_s8 d=(xm_s8)f->get_u8();
								data[k]=d+old;
								old=data[k];
							}
							if (pad_sample_mem) {
								/* interpolation helper */
								/* these make looping smoother */
								switch( sample_data[j].loop_type ) {
	
									case XM_LOOP_DISABLED: data[sample_data[j].length]=0; break;
									case XM_LOOP_FORWARD: data[sample_data[j].length]=data[sample_data[j].loop_begin]; break;
									case XM_LOOP_PING_PONG: data[sample_data[j].length]=data[sample_data[j].loop_end]; break;
	
								}
	
								data[sample_data[j].length+1]=0;
							}
						} break;
						default: { /* just read it into memory */
	
							f->get_byte_array((xm_u8*)sample_data[j].data,sample_data[j].length);
						} break;
					}
				}

				_smp->sample_id=_xm_mixer->sample_register( &sample_data[j] );

#ifdef _XM_DEBUG

				_XM_DEBUG_PRINTF("\t\tLength: %i\n",sample_data[j].length);
				switch (sample_data[j].loop_type) {

					case XM_LOOP_DISABLED:  _XM_DEBUG_PRINTF("\t\tLoop: Disabled\n"); break;
					case XM_LOOP_FORWARD:   _XM_DEBUG_PRINTF("\t\tLoop: Forward\n"); break;
					case XM_LOOP_PING_PONG: _XM_DEBUG_PRINTF("\t\tLoop: PingPong\n"); break;
				}

				_XM_DEBUG_PRINTF("\t\tLoop Begin: %i\n",sample_data[j].loop_begin);
				_XM_DEBUG_PRINTF("\t\tLoop End: %i\n",sample_data[j].loop_end);
				_XM_DEBUG_PRINTF("\t\tVolume: %i\n",_smp->volume);
				_XM_DEBUG_PRINTF("\t\tPan: %i\n",_smp->pan);
				_XM_DEBUG_PRINTF("\t\tBase Note: %i\n",_smp->base_note);
				_XM_DEBUG_PRINTF("\t\tFineTune: %i\n",_smp->finetune);
				switch (sample_data[j].format) {
					/* bit 3 of XM sample flags specify extended (propertary) format */
					case XM_SAMPLE_FORMAT_PCM8: _XM_DEBUG_PRINTF("\t\tFormat: PCM8\n"); break;
					case XM_SAMPLE_FORMAT_PCM16: _XM_DEBUG_PRINTF("\t\tFormat: PCM16\n"); break;
					case XM_SAMPLE_FORMAT_IMA_ADPCM: _XM_DEBUG_PRINTF("\t\tFormat: IMA_ADPCM\n"); break;
					case XM_SAMPLE_FORMAT_CUSTOM: _XM_DEBUG_PRINTF("\t\tFormat: CUSTOM\n"); break;
				}

#endif
			}



		}


	}

	f->close();
	return XM_LOADER_OK;

}
XM_LoaderError xm_loader_open_song( const char *p_filename, XM_Song *p_song ) {

	return _xm_loader_open_song_custom( p_filename, p_song, xm_true, xm_true );
}

XM_LoaderError xm_loader_open_song_music( const char *p_filename, XM_Song *p_song ) {

	return _xm_loader_open_song_custom( p_filename, p_song, xm_true, xm_false );
}
XM_LoaderError xm_loader_open_instruments( const char *p_filename, XM_Song *p_song )  {

	return _xm_loader_open_song_custom( p_filename, p_song, xm_false, xm_true );
}


void xm_loader_free_song( XM_Song *p_song ) {
	_xm_clear_song(p_song, p_song->pattern_count, p_song->instrument_count );
}

void xm_loader_free_music( XM_Song *p_song ) {
	_xm_clear_song( p_song, p_song->pattern_count, -1 );
}

void xm_loader_free_instruments( XM_Song *p_song ) {
	_xm_clear_song( p_song, -1, p_song->instrument_count );

}




XM_SampleID xm_load_wav(const char *p_file)
{
	XM_FileIO *f=_xm_fileio;
	XM_SampleData p_sample;

	if(f->open( p_file, xm_false) != XM_FILE_OK) {
		_XM_DEBUG_PRINTF("[xm_load_wav] Failed to open file.");
		return XM_INVALID_SAMPLE_ID;
	}

	/* CHECK RIFF */
	char riff[5];
	riff[4]=0;
	f->get_byte_array((xm_u8*)&riff,4);

	if (riff[0]!='R' || riff[1]!='I' || riff[2]!='F' || riff[3]!='F') {
		f->close();
		_XM_DEBUG_PRINTF("[xm_load_wav] Invalid file format.");
		return XM_INVALID_SAMPLE_ID;
	}


	/* GET FILESIZE xm_u32 filesize=f->get_u32(); */
	f->get_u32();

	/* CHECK WAVE */
	char wave[4];

	f->get_byte_array((xm_u8*)&wave,4);

	if (wave[0]!='W' || wave[1]!='A' || wave[2]!='V' || wave[3]!='E')
	{
		f->close();
		_XM_DEBUG_PRINTF("[xm_load_wav] Invalid file format.");
		return XM_INVALID_SAMPLE_ID;
	}

	xm_bool format_found = xm_false;
	int format_bits=0;
	int format_channels=0;
	int format_freq=0;
	
	int id=XM_INVALID_SAMPLE_ID;
	
	while (!f->eof_reached())
	{
		/* chunk */
		char chunkID[4];
		f->get_byte_array((xm_u8*)&chunkID,4);

		/* chunk size */
		xm_u32 chunksize = f->get_u32();
		xm_u32 file_pos  = f->get_pos();

		if (f->eof_reached()) {
			_XM_DEBUG_PRINTF("EOF AFTER read chunk header and chunk size reached. breaking.\n");
			break;
		}

		_XM_DEBUG_PRINTF("READ CHUNK: '%c%c%c%c' x %d\n", chunkID[0], chunkID[1], chunkID[2], chunkID[3], chunksize);

		/* FORMAT CHUNK */
		if (chunkID[0]=='f' && chunkID[1]=='m' && chunkID[2]=='t' && chunkID[3]==' ' && !format_found)
		{
			xm_u16 compression_code=f->get_u16();

			if (compression_code!=1) {
				_XM_DEBUG_PRINTF("Format not supported for WAVE file (not PCM)\n");
				break;
			}

			format_channels=f->get_u16();
			if (format_channels!=1 && format_channels !=2)
			{
				_XM_DEBUG_PRINTF("Format not supported for WAVE file (not stereo or mono)\n");
				break;
			}

			/* sampling rate */
			format_freq=f->get_u32();

			/* average bits/second (unused) */
			f->get_u32();

			/* block align (unused) */
			f->get_u16();

			/* bits per sample */
			format_bits=f->get_u16();

			if (format_bits%8) {
				_XM_DEBUG_PRINTF("Strange number of bits in sample (not 8,16,24,32)\n");
				break;
			}

			/* Dont need anything else, continue */
			format_found=xm_true;
		}

		/* DATA CHUNK */
		if (chunkID[0]=='d' && chunkID[1]=='a' && chunkID[2]=='t' && chunkID[3]=='a')
		{
			if(!format_found) {
				_XM_DEBUG_PRINTF("'data' chunk before 'format' chunk found.\n");
				break;
			}

			int frames=chunksize;
			frames/=format_channels;
			frames/=(format_bits>>3);

			p_sample.data             = (void*)_xm_memory->alloc(chunksize, XM_MEMORY_ALLOC_WAV);
			p_sample.format           = format_bits == 8 ? XM_SAMPLE_FORMAT_PCM8 : XM_SAMPLE_FORMAT_PCM16;
			p_sample.loop_type        = XM_LOOP_DISABLED;
			p_sample.loop_begin       = 0;
			p_sample.loop_end         = 0;
			p_sample.length           = frames;
			p_sample.base_sample_rate = format_freq;
			_XM_DEBUG_PRINTF("Base Sample Rate %i\n",format_freq);

			if(!p_sample.data) {
				_XM_DEBUG_PRINTF("ERROR\n");
				break;
			}


			void * data_ptr=p_sample.data;

			int i, c, b;
			for (i=0;i<frames;i++)
			{
				for (c=0;c<format_channels;c++)
				{
					/* 8 bit samples are UNSIGNED */
					if (format_bits==8)
					{
						xm_u8 s = f->get_u8();
						s-=128;
						xm_s8 *sp=(xm_s8*)&s;

						xm_s8 *data_ptr8=&((xm_s8*)data_ptr)[i*format_channels+c];

						*data_ptr8=*sp;

					/* 16+ bits samples are SIGNED */
					} else {
						xm_u16 s = f->get_u16();
						xm_s16 *sp=(xm_s16*)&s;

						/* if sample is > 16 bits, just read extra bytes */
						for (b=0;b<((format_bits>>3)-2);b++) {
							f->get_u8();
						}

						xm_s16 *data_ptr16=&((xm_s16*)data_ptr)[i*format_channels+c];

						*data_ptr16=*sp;
					}
				}
			}


			if (f->eof_reached()) {
				_XM_DEBUG_PRINTF("File corrupted\n");
				break;
			} else {
			
				/** Mixer takes ownership of the sample data */
				_XM_AUDIO_LOCK;
				id=_xm_mixer->sample_register(&p_sample);
				_XM_AUDIO_UNLOCK;
				_XM_DEBUG_PRINTF("WAV load OK!\n");
				break;				
			}
		}

		f->seek_pos( file_pos+chunksize );
	}

	f->close();

	return id;
}

int xm_sfx_start(XM_SampleID sample) {
	/*
	int song_chans       = _(xm_song->flags&XM_SONG_FLAGS_MASK_CHANNELS_USED) + 1;
	int mixer_max_voices = _xm_mixer->get_features()&XM_MIXER_FEATURE_MAX_VOICES_MASK;
	*/
	return 0;
}

void xm_sfx_start_voice(XM_SampleID sample,int voice) {
	_XM_AUDIO_LOCK;
	_xm_mixer->voice_start(voice, sample, 0);
	_XM_AUDIO_UNLOCK;
}

void xm_sfx_set_vol(int voice, xm_u8 vol) {
	_XM_AUDIO_LOCK;
	_xm_mixer->voice_set_volume(voice, vol);
	_XM_AUDIO_UNLOCK;
}

void xm_sfx_set_pan(int p_voice, xm_u8 pan) {
	_XM_AUDIO_LOCK;
	_xm_mixer->voice_set_pan(p_voice, pan);
	_XM_AUDIO_UNLOCK;
}

void xm_sfx_set_pitch(int p_voice, xm_u32 pitch_hz) {

	_XM_AUDIO_LOCK;
	_xm_mixer->voice_set_speed(p_voice, pitch_hz);
	_XM_AUDIO_UNLOCK;
}

void xm_sfx_stop(int voice) {

	_XM_AUDIO_LOCK;
	_xm_mixer->voice_stop(voice);
	_XM_AUDIO_UNLOCK;
}

