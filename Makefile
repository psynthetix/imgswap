CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET ?= imgswap
SRC := imgswap.c

WARN_CFLAGS := -Wall -Wextra -O2
PKG_MODULES := libjpeg libpng libwebp libtiff-4 libheif

CFLAGS += $(WARN_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKG_MODULES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKG_MODULES)) -lgif

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
