CC = gcc
CFLAGS = -Wall -O2
LIBS = $(shell pkg-config --cflags --libs libavformat libavcodec libavutil libswscale libswresample libass sdl2) -lm
SRC = src/vidp.c
TARGET = vidp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	rm -f $(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
