all: printf.so bp.so string.so fake_file.so page_malloc.so dumb_string.so dumb_file.so crc32.so libx11.so sdl.so

clean:
	rm -f *.so *.o

CFLAGS=-O2 -fPIC -I../include/ -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -shared

%.so: %.cc
	g++ $(CFLAGS) $< -o $@

%.so: %.c
	gcc $(CFLAGS) $< -o $@

#printf.so: printf.o
#	ld --shared -o $@ $<

#bp.so: bp.o
#	ld --shared -o $@ $<
