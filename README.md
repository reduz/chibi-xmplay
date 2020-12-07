Chibi XMPlay by Juan Linietsky

Extremely small C library for full song XM playback, can be embedded or ported
to pretty much any platform in existence. It was used back in the time for
many Nintendo DS and Wii games.

As an additional feature, if the underlying platform has support for dedicated audio mixing and resampling hardware, chibi-xmplay can accept custom back-ends to take advantage of it (instead of doing software mixing), freing the CPU for other tasks. This was useful back in the day in console games, as most hardware supported this.

