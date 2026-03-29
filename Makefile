CC ?= cc
PKG_CONFIG ?= pkg-config

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET ?= imgswap
SRC := imgswap.c

WARN_CFLAGS := -Wall -Wextra -O2

TIFF_PKG := $(shell $(PKG_CONFIG) --exists libtiff-4 && echo libtiff-4 || echo libtiff)
JPEG_PKG := $(shell $(PKG_CONFIG) --exists libjpeg && echo libjpeg)
PNG_PKG := libpng
WEBP_PKG := libwebp
HEIF_PKG := libheif

PKG_MODULES := $(PNG_PKG) $(WEBP_PKG) $(HEIF_PKG) $(TIFF_PKG)

ifeq ($(JPEG_PKG),)
    JPEG_LIBS := -ljpeg
    JPEG_CFLAGS :=
else
    PKG_MODULES += $(JPEG_PKG)
    JPEG_LIBS :=
    JPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(JPEG_PKG))
endif

CFLAGS += $(WARN_CFLAGS) $(JPEG_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKG_MODULES))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKG_MODULES)) $(JPEG_LIBS) -lgif

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
