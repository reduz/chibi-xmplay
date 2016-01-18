all:
	gcc -ansi -g -Wall xmplay.c xmplay_sdl.c -o xmplay -D _XM_DEBUG `sdl-config --cflags --libs`