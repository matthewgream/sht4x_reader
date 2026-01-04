
CC=gcc
CFLAGS_COMMON=-Wall -Wextra -Wpedantic
CFLAGS_STRICT=\
    -Wstrict-prototypes \
    -Wold-style-definition \
    -Wcast-align -Wcast-qual -Wconversion \
    -Wfloat-equal -Wformat=2 -Wformat-security \
    -Winit-self -Wjump-misses-init \
    -Wlogical-op -Wmissing-include-dirs \
    -Wnested-externs -Wpointer-arith \
    -Wredundant-decls -Wshadow \
    -Wstrict-overflow=5 -Wswitch-default \
    -Wswitch-enum -Wundef \
    -Wunreachable-code -Wunused \
    -Wwrite-strings
CFLAGS=$(CFLAGS_COMMON) $(CFLAGS_STRICT) -O3 -march=native -fstack-protector-strong
LDFLAGS=-lmosquitto

TARGET=sht4x_reader
SOURCES=sht4x_reader.c

##

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: $(TARGET)
	./$(TARGET) /dev/sht4x

clean:
	rm -f $(TARGET)

format:
	clang-format -i $(SOURCES)

.PHONY: clean format

##

INSTALL_DIR = /usr/local/bin
DEFAULT_DIR = /etc/default
SYSTEMD_DIR = /etc/systemd/system
UDEV_DIR = /etc/udev/rules.d
define install_service_systemd
	-systemctl stop $(1) 2>/dev/null || true
	-systemctl disable $(1) 2>/dev/null || true
	cp $(2).service $(SYSTEMD_DIR)/$(1).service
	systemctl daemon-reload
	systemctl enable $(1)
	systemctl start $(1) || echo "Warning: Failed to start $(1)"
endef
define install_udev_rules
	cp $(1) $(UDEV_DIR)
	udevadm control --reload
	udevadm trigger
endef
install_target: $(TARGET)
	install -m 755 $(TARGET) $(INSTALL_DIR)
install_default: $(TARGET).default
	cp $(TARGET).default $(DEFAULT_DIR)/$(TARGET)
install_service: $(TARGET).service
	$(call install_service_systemd,$(TARGET),$(TARGET))
install_udev: 99-$(TARGET).rules
	$(call install_udev_rules,99-$(TARGET).rules)
install: install_target install_default install_service install_udev
restart:
	systemctl restart $(TARGET)
.PHONY: install install_target install_default install_service install_udev
.PHONY: restart

