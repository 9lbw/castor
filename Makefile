CC = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra `pkg-config --cflags x11 xft`
LDFLAGS = `pkg-config --libs x11 xft`

TARGET = launcher
SRC = launcher.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c config.h
	$(CC) -c $(CFLAGS) $<

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	mkdir -p $(DESTDIR)/usr/local/bin
	cp -f $(TARGET) $(DESTDIR)/usr/local/bin

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
