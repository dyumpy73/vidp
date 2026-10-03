CC = gcc
CFLAGS = -O2 -Wall
PKGS = libavformat libavcodec libavutil libswscale libswresample libass sdl2
LIBS = $(shell pkg-config --cflags --libs $(PKGS)) -lm

TARGET = vidp
SRC = src/vidp.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)
	strip $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install uninstall clean
