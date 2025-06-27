obj-m += tx_driver.o rx_driver.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f epaper-gpio.dtbo

dtbo: epaper-gpio.dts
	dtc -@ -I dts -O dtb -o epaper-gpio.dtbo epaper-gpio.dts

install: all dtbo
	sudo insmod tx_driver.ko
	sudo insmod rx_driver.ko
	sudo cp epaper-gpio.dtbo /boot/overlays/
	@if ! grep -q "dtoverlay=epaper-gpio" /boot/firmware/config.txt 2>/dev/null; then \
		echo "dtoverlay=epaper-gpio" | sudo tee -a /boot/firmware/config.txt; \
		echo "!!!!!! REBOOTING REQUIRED !!!!!!: sudo reboot"; \
	fi
	sudo chmod 666 /dev/epaper_tx /dev/epaper_rx 2>/dev/null || true

uninstall:
	sudo rmmod rx_driver tx_driver 2>/dev/null || true

.PHONY: all clean install uninstall dtbo