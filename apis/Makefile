CC = gcc
CFLAGS = -Wall -O2 -std=gnu99 -fPIC
TARGET_LIB = libepaper.a
TARGET_SO = libepaper.so
SOURCES = send_epaper_data.c receive_epaper_data.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = send_epaper_data.h receive_epaper_data.h stb_image.h

all: $(TARGET_LIB) $(TARGET_SO)

$(TARGET_LIB): $(OBJECTS)
	ar rcs $@ $^

$(TARGET_SO): $(OBJECTS)
	$(CC) -shared -o $@ $^ -lm

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET_LIB) $(TARGET_SO)

install: $(TARGET_LIB) $(TARGET_SO)
	sudo cp $(TARGET_LIB) /usr/local/lib/
	sudo cp $(TARGET_SO) /usr/local/lib/
	sudo cp $(HEADERS) /usr/local/include/
	sudo ldconfig

.PHONY: all clean install
