CC ?= cc

CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

PKG_CONFIG ?= pkg-config
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client 2>/dev/null)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client 2>/dev/null)

TARGET := wayfetch
SRC := src/wayfetch.c src/detect.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC) src/detect.h
	@if ! $(PKG_CONFIG) --exists wayland-client; then \
		echo "error: wayland-client development files not found."; \
		echo "Install wayland-devel and pkg-config, then run make again."; \
		exit 1; \
	fi
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(WAYLAND_LIBS) -lm $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)
