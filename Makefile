CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PKG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample libass sdl2)
PKG_LIBS   := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale libswresample libass sdl2) -lm

CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(PKG_CFLAGS)
LDFLAGS += $(PKG_LIBS)

TARGET   = vidp
SRC      = src/vidp.c
OBJ      = $(SRC:.c=.o)

.PHONY: all debug clean install uninstall

all: $(TARGET)


$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS := -g -O0 -Wall -Wextra -DDEBUG $(PKG_CFLAGS)
debug: clean $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
