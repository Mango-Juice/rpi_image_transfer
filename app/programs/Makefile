CC = gcc
CFLAGS = -Wall -O2 -std=gnu99
LDFLAGS = 
LIBS = -lepaper -lm
TARGET = epaper_send
TARGET_RX = epaper_receive
SOURCES = epaper_send.c
SOURCES_RX = epaper_receive.c
HEADERS = 

all: $(TARGET) $(TARGET_RX)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LDFLAGS) $(LIBS)

$(TARGET_RX): $(SOURCES_RX)
	$(CC) $(CFLAGS) -o $(TARGET_RX) $(SOURCES_RX) $(LDFLAGS) $(LIBS)

../apis/libepaper.a:
	$(MAKE) -C ../apis

clean:
	rm -f $(TARGET) $(TARGET_RX)

install: $(TARGET) $(TARGET_RX)
	sudo cp $(TARGET) $(TARGET_RX) /usr/local/bin/

.PHONY: all clean install
