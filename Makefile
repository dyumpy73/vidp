CC = gcc
CFLAGS ?= -O2 -Wall -Wextra

PKGS = libavformat libavcodec libavutil libswscale libswresample libass sdl2
CFLAGS_PKG = $(shell pkg-config --cflags $(PKGS))
LIBS = $(shell pkg-config --libs $(PKGS)) -lm

TARGET = vidp
SRC = src/main.c \
      src/player.c \
      src/video.c \
      src/audio.c \
      src/subtitle.c \
      src/playlist.c \
      src/utils.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LIBS)

%.o: %.c src/player.h
	$(CC) $(CFLAGS) $(CFLAGS_PKG) -c $< -o $@

debug: CFLAGS = -g -O0 -Wall -Wextra
debug: clean $(TARGET)

release: CFLAGS = -O2 -Wall -Wextra
release: clean $(TARGET)
	strip $(TARGET)

install: release
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all debug release install uninstall clean
