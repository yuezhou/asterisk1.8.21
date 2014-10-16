CFLAGS=-O3 -ffast-math -pipe -Wall -ansi -pedantic
#LDFLAGS=-lm
LIBS=-lm

all: fftdetect
fftdetect: fftdetect.o tone_detect.a ${LIBS}
fftdetect.o: fftdetect.c tone_detect.h
tone_detect.a: tone_detect.o fft.o
	ar -rc tone_detect.a tone_detect.o fft.o
tone_detect.o: tone_detect.c tone_detect.h
	$(CC) $(CFLAGS) -fPIC -c tone_detect.c
fft.o: fft.c
	$(CC) $(CFLAGS) -fPIC -c fft.c

install: 
dist-clean: clean
clean:
	rm -f *.o *.a fftdetect
