CC       := gcc
TARGET   := audio_gate_router
SRC      := audio_gate_router.c
ICON_PNG := app_icon.png
ICON_HDR := app_icon_data.h
APP_ID   := org.fedora.audiogaterouter

PREFIX   ?= /usr
BINDIR   := $(DESTDIR)$(PREFIX)/bin
APPDIR   := $(DESTDIR)$(PREFIX)/share/applications
ICONDIR  := $(DESTDIR)$(PREFIX)/share/icons/hicolor/512x512/apps

CFLAGS   := -O2 -Wall $(shell pkg-config --cflags gtk4 libpulse libpulse-simple)
LIBS     := $(shell pkg-config --libs gtk4 libpulse libpulse-simple) -lm -lpthread

.PHONY: all build clean install uninstall

all: build

build: $(TARGET)

# Convert app_icon.png into a C byte array header (app_icon_data.h)
$(ICON_HDR): $(ICON_PNG)
	xxd -i $(ICON_PNG) > $(ICON_HDR)

# Compile the binary after generating the baked-in icon header
$(TARGET): $(SRC) app_icon.h $(ICON_HDR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

install: build
	# Remove old audio-gate-router leftovers if present
	rm -f /usr/local/share/applications/audio-gate-router.desktop
	rm -f /usr/local/share/icons/hicolor/512x512/apps/audio-gate-router.png
	rm -f "$(HOME)/.local/share/icons/hicolor/512x512/apps/audio-gate-router.png"
	install -d "$(BINDIR)"
	install -m 755 $(TARGET) "$(BINDIR)/$(TARGET)"
	install -d "$(ICONDIR)"
	install -m 644 $(ICON_PNG) "$(ICONDIR)/$(APP_ID).png"
	install -d "$(APPDIR)"
	@echo "[Desktop Entry]" > "$(APPDIR)/$(APP_ID).desktop"
	@echo "Type=Application" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "Name=PipeWire Audio Gate Router" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "Comment=Noise-Gated PipeWire Direct Patchbay" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "Exec=$(PREFIX)/bin/$(TARGET)" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "Icon=$(APP_ID)" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "StartupWMClass=$(APP_ID)" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "Terminal=false" >> "$(APPDIR)/$(APP_ID).desktop"
	@echo "Categories=AudioVideo;Audio;" >> "$(APPDIR)/$(APP_ID).desktop"
	-gtk-update-icon-cache -f -t "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

uninstall:
	rm -f "$(BINDIR)/$(TARGET)"
	rm -f "$(ICONDIR)/$(APP_ID).png"
	rm -f "$(APPDIR)/$(APP_ID).desktop"
	rm -f "$(HOME)/.config/autostart/audio_gate_router.desktop"
	rm -f "$(HOME)/.local/share/icons/hicolor/512x512/apps/$(APP_ID).png"
	rm -rf "$(HOME)/.local/share/audio_gate_router"
	-gtk-update-icon-cache -f -t "$(DESTDIR)$(PREFIX)/share/icons/hicolor" 2>/dev/null || true

clean:
	rm -f $(TARGET) $(ICON_HDR)
