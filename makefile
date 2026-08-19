CC      ?= gcc
PKGS     = gtk+-3.0 libconfig
CFLAGS  += -O2 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS  := $(shell pkg-config --libs $(PKGS))
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
LDLIBS += -lpthread
endif

TARGET = serial-terminal

all: $(TARGET)

$(TARGET): main.c window1.glade
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

clean:
	rm -f $(TARGET)
