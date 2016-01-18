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


#include "xmplay.h"

#include <SDL.h>
#include <SDL_main.h>

/** XM_AudioLock Implementation */

void lock_audio() {
	
	SDL_LockAudio();
}

void unlock_audio() {
	
	SDL_UnlockAudio();
}

/** XM_MemoryManager Implementation */

void * alloc_mem(xm_u32 p_size,XM_MemoryAllocType p_alloc_type) {
	
	return malloc(p_size);
}

void free_mem(void *p_mem,XM_MemoryAllocType p_alloc_type) {
	
	free(p_mem);
}

/** XM_FileIO Implementation **/

FILE *f=NULL;
xm_bool f_be;

xm_bool fileio_in_use() {
	
	return f?xm_true:xm_false;		
}

XM_FileIOError fileio_open(const char *p_file,xm_bool p_big_endian_mode) {
	
	if (f) 
		return XM_FILE_ERROR_IN_USE;
	
	f=fopen(p_file,"rb");
	if (!f)
		return XM_FILE_ERROR_CANT_OPEN;
	
	f_be=p_big_endian_mode;
	return XM_FILE_OK;
}

xm_u8 fileio_get_u8() {
	
	
	xm_u8 b;
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return 0;	
	}
	
	fread(&b,1,1,f);
	
	return b;
}

xm_u16 fileio_get_u16() {
	
	xm_u8 a,b;
	xm_u16 c;
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return 0;	
	}
	
	if (!f_be) {
		a=fileio_get_u8();
		b=fileio_get_u8();
	} else {
		
		b=fileio_get_u8();
		a=fileio_get_u8();		
	}
	
	c=((xm_u16)b << 8 ) | a;
	
	return c;
}

xm_u32 fileio_get_u32() {
	
	xm_u16 a,b;
	xm_u32 c;
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return 0;	
	}
	
	if (!f_be) {
		a=fileio_get_u16();
		b=fileio_get_u16();
	} else {
		
		b=fileio_get_u16();
		a=fileio_get_u16();		
	}
	
	c=((xm_u32)b << 16 ) | a;	
	
	return c;
}

void fileio_get_byte_array(xm_u8 *p_dst,xm_u32 p_count) {
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return;	
	}
	
	fread(p_dst,p_count,1,f);
}

void fileio_seek_pos(xm_u32 p_offset) {
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return;	
	}
	
	fseek(f,p_offset,SEEK_SET);
}

xm_u32 fileio_get_pos() {
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return 0;	
	}
	
	return ftell(f);
}

xm_bool fileio_eof_reached() {
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return xm_true;	
	}
	
	return feof(f)?xm_true:xm_false;
}


/** XM_Mixer read callback **/
 
 
#define INTERNAL_BUFFER_SIZE 2048
 static xm_s32 mix_buff[INTERNAL_BUFFER_SIZE*2];
 
void audio_callback(void *userdata, Uint8 *stream, int len) {
	
	int todo=len/4; /* since dst is 16 bits, stereo | convert bytes -> frames */
	int i;
	Sint16 *dst_buff=(Sint16*)stream;
	
	while (todo) {
	
		int to_mix=todo;
		if (to_mix>INTERNAL_BUFFER_SIZE)
			to_mix=INTERNAL_BUFFER_SIZE;
			
		todo-=to_mix;
			
		for (i=0;i<to_mix*2;i++) {
		
			mix_buff[i]=0; /* clean up target buffer */
		}
		xm_software_mix_to_buffer(mix_buff,to_mix);
		
		for (i=0;i<to_mix*2;i++) {
			
			dst_buff[i]=mix_buff[i]>>16; /* conver to 16 bits */			
		}
	
	}
	
}

void fileio_close() {
	
	if (!f) {
		
		fprintf(stderr,"File Not Open!");
		return;	
	}
	
	fclose(f);
	f=NULL;
}

	xm_bool (*in_use)(); /** FileIO is working with a file */
	XM_FileIOError (*open)(const char *p_file,xm_bool p_big_endian_mode); /** Open a File **/
	xm_u8 (*get_u8)(); /** Get byte from file */
	xm_u16 (*get_u16)(); /** Get 16 bits word from file **/
	xm_u32 (*get_u32)(); /** Get 32 bits dword from file **/
	void (*get_byte_array)(xm_u8 *p_dst,xm_u32 p_count); /** Get a byte array from the file */
	
	void (*seek_pos)(xm_u32 p_offset); /** Seek to a file position **/
	xm_u32 (*get_pos)(); /** Get the current file position **/
	
	void(*close)(); /** Close the file **/


int main(int argc, char *argv[]) {
	
	
	XM_AudioLock audio_lock;
	XM_MemoryManager memory_manager;
	XM_FileIO file_io;
	SDL_AudioSpec desired_audio;
	SDL_AudioSpec obtained_audio;
	XM_Song *song;
	XM_LoaderError song_error;
	
	if (argc<=1) {
		printf("usage: xmplay_sdl file.xm\n");
		return 255;
	}	
		   
	SDL_Init(SDL_INIT_AUDIO);
		  
	/** AUDIO LOCK **/
	

	
	audio_lock.lock=lock_audio;
	audio_lock.unlock=unlock_audio;
	
	xm_set_audio_lock( & audio_lock );
	
	/** MEMORY MANAGER **/
	
	
	memory_manager.alloc=alloc_mem;
	memory_manager.free=free_mem;
	
	xm_set_memory_manager( &memory_manager );
	
	/** FILE IO **/
	
	
	file_io.in_use=fileio_in_use;
	file_io.open=fileio_open;
	file_io.get_u8=fileio_get_u8;
	file_io.get_u16=fileio_get_u16;
	file_io.get_u32=fileio_get_u32;
	file_io.get_byte_array=fileio_get_byte_array;
	file_io.seek_pos=fileio_seek_pos;
	file_io.get_pos=fileio_get_pos;
	file_io.eof_reached=fileio_eof_reached;
	file_io.close=fileio_close;
	
	xm_loader_set_fileio( &file_io );
	
	/** MIXER **/
	
	
	desired_audio.freq=44100;
	desired_audio.format=AUDIO_S16SYS;	
	desired_audio.channels=2;
	desired_audio.samples=2048;
	desired_audio.callback=audio_callback;
	desired_audio.userdata=0;
	
	SDL_OpenAudio(&desired_audio,&obtained_audio);

	if (obtained_audio.channels!=2) {
		
		printf("Stereo not supported");
		return 255;	
	}
	
	xm_create_software_mixer(obtained_audio.freq, 32); 

	/** SONG **/
	
	song = xm_song_alloc();
	
/*	xm_loader_set_recompress_all_samples(xm_true); */
	song_error=xm_loader_open_song( argv[1], song );

	switch (song_error) {
		
		case XM_LOADER_OK: printf("Loaded Song OK\n"); break;
		case XM_LOADER_UNCONFIGURED: printf("Loader Error: Unconfigured\n"); break;
		case XM_LOADER_ERROR_FILEIO_IN_USE: printf("Loader Error: File in Use\n"); break;
		case XM_LOADER_ERROR_FILE_CANT_OPEN: printf("Loader Error: Can't Open\n"); break;
		case XM_LOADER_ERROR_FILE_UNRECOGNIZED: printf("Loader Error: File Unrecognized\n"); break;
		case XM_LOADER_ERROR_OUT_OF_MEMORY: printf("Loader Error: Out of Memory\n"); break;
		case XM_LOADER_ERROR_FILE_CORRUPT: printf("Loader Error: File is Corrupted\n"); break;		
	}
	
	if (song_error)
		return 255;
	
	xm_player_set_song(song);
	xm_player_play();
	
	SDL_PauseAudio(0);

	while(1) { SDL_Delay(50); }
	
	
	return 0;
}
