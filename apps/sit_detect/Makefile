CFLAGS=-O3 -ffast-math -pipe -Wall --std=c99 -pedantic
#LDFLAGS=-lm
LIBS=-lm

all: fftdetect

fftdetect: fftdetect.o sit_detect.a ${LIBS}
fftdetect.o: fftdetect.c sit_detect.h
sit_detect.a: sit_detect.o fft.o
	ar -rc sit_detect.a sit_detect.o fft.o
sit_detect.o: sit_detect.c sit_detect.h
	$(CC) $(CFLAGS) -fPIC -c sit_detect.c
fft.o: fft.c
	$(CC) $(CFLAGS) -fPIC -c fft.c

install: 
dist-clean: clean
clean:
	rm -f *.o *.a fftdetect
