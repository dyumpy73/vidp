CC = gcc
PKGS = libavformat libavcodec libavutil libswscale libswresample libass sdl2
LIBS = $(shell pkg-config --cflags --libs $(PKGS)) -lm

TARGET = vidp
SRC = src/vidp.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)

debug: $(SRC)
	$(CC) -g -O0 -Wall -Wextra $(SRC) -o $(TARGET) $(LIBS)

release: $(SRC)
	$(CC) -O2 -Wall -Wextra $(SRC) -o $(TARGET) $(LIBS)
	strip $(TARGET)

install: release
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

uninstall:
	rm -f /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all debug release install uninstall clean
