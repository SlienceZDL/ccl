CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -pedantic
LDFLAGS ?=

TARGET := ib_util_monitor_us
SRC := ib_util_monitor_us.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
