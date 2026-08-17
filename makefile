# Serial Terminal — GTK3 + Glade
#
# Requirements: gcc, pkg-config, libgtk-3-dev
#   Debian/Ubuntu:  sudo apt install gcc pkg-config libgtk-3-dev
#   Fedora:         sudo dnf install gcc pkgconfig gtk3-devel
#   Arch:           sudo pacman -S gcc pkgconf gtk3
#
# Usage:
#   make            build ./serial-terminal (uses window1.glade in cwd / exe dir)
#   make install    install under $(PREFIX) (default /usr/local)
#   make deb        build a .deb package (needs dpkg-deb; see make-deb.sh)
#   make run        build and run
#   make check-ui   validate window1.glade syntax

CC      ?= cc
PKG     := gtk+-3.0
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags $(PKG))
# -rdynamic: export the glade signal handlers (on_*) so that
# gtk_builder_connect_signals() can find them via dlsym()
LDFLAGS += -rdynamic
LDLIBS  := $(shell pkg-config --libs $(PKG))

TARGET  := serial-terminal

# install layout — override on the command line, e.g.  make install PREFIX=/usr
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/serial-terminal
ICONDIR ?= $(PREFIX)/share/icons/hicolor/scalable/apps

# let main.c find window1.glade in the install location
CPPFLAGS += -DDATA_DIR=\"$(DATADIR)\"

all: $(TARGET)

$(TARGET): main.c window1.glade
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ main.c $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR) $(DESTDIR)$(ICONDIR)
	install -m755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m644 window1.glade $(DESTDIR)$(DATADIR)/window1.glade
	install -m644 debian/serial-terminal.svg $(DESTDIR)$(ICONDIR)/serial-terminal.svg

deb:
	./make-deb.sh

# validate the glade file at build time (needs python3 + libxml2 module)
check-ui:
	python3 -c "import xml.dom.minidom,sys; xml.dom.minidom.parse('window1.glade'); print('window1.glade: XML OK')"

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install deb check-ui run clean
