CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr
DESTDIR ?=
STRIP   ?= strip

PKGS       = libavformat libavcodec libavutil libswscale libswresample libass sdl3 sdl3-ttf
CFLAGS_PKG = $(shell pkg-config --cflags $(PKGS))
LIBS       = $(shell pkg-config --libs $(PKGS)) -lm -lpthread

TARGET = vidp
SRC = src/main.c \
      src/player.c \
      src/video.c \
      src/audio.c \
      src/subtitle.c \
      src/playlist.c \
      src/utils.c \
      src/idle.c
OBJ = $(SRC:.c=.o)

.PHONY: all debug release install uninstall clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LIBS)

%.o: %.c src/player.h
	$(CC) $(CFLAGS) $(CFLAGS_PKG) -c $< -o $@

debug: CFLAGS = -g -O0 -Wall -Wextra
debug: clean $(TARGET)

release: CFLAGS = -O2 -Wall -Wextra
release: clean $(TARGET)
	$(STRIP) $(TARGET)

install: release
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 assets/vidp.desktop $(DESTDIR)$(PREFIX)/share/applications/vidp.desktop
	install -Dm644 assets/vidp.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/vidp.svg

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/vidp.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/vidp.svg

clean:
	rm -f $(TARGET) $(OBJ)
