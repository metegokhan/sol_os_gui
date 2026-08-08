CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

ROOT := ../..
WRITER_DIR := $(ROOT)/src/apps
TARGETS := writer_buffer_test writer_files_test doc_layout_test shell_parse_test shell_launch_test app_file_types_test gameboy_support_test storage_mounts_test keys_test input_test hid_keyboard_report_test ps2_keyboard_test midi_codec_test

.PHONY: all test clean

all: $(TARGETS)

writer_buffer_test: writer_buffer_test.c $(WRITER_DIR)/solar_os_writer_buffer.c $(WRITER_DIR)/solar_os_writer_buffer.h esp_err.h
	$(CC) $(CFLAGS) -DSOLAR_OS_WRITER_HOST_TEST -I. -I$(WRITER_DIR) \
		writer_buffer_test.c $(WRITER_DIR)/solar_os_writer_buffer.c -o $@

doc_layout_test: doc_layout_test.c doc_test_support.c $(ROOT)/src/services/solar_os_doc.c $(ROOT)/src/services/solar_os_doc.h
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
		-D_GNU_SOURCE -ffunction-sections -fdata-sections -include compat.h \
		-I. -I$(ROOT)/src -I$(ROOT)/src/services \
		doc_layout_test.c doc_test_support.c $(ROOT)/src/services/solar_os_doc.c \
		-Wl,--gc-sections -o $@

writer_files_test: writer_files_test.c $(WRITER_DIR)/solar_os_writer_files.c $(WRITER_DIR)/solar_os_writer_files.h esp_err.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I. -I$(ROOT)/src/services -I$(WRITER_DIR) \
		writer_files_test.c $(WRITER_DIR)/solar_os_writer_files.c -o $@

shell_parse_test: shell_parse_test.c $(ROOT)/src/shell/solar_os_shell_parse.c $(ROOT)/src/shell/solar_os_shell_parse.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_parse_test.c $(ROOT)/src/shell/solar_os_shell_parse.c -o $@

shell_launch_test: shell_launch_test.c $(ROOT)/src/shell/solar_os_shell_launch.c $(ROOT)/src/shell/solar_os_shell_launch.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/shell \
		shell_launch_test.c $(ROOT)/src/shell/solar_os_shell_launch.c -o $@

app_file_types_test: app_file_types_test.c $(ROOT)/src/apps/solar_os_app_file_types.c $(ROOT)/src/apps/solar_os_app_file_types.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/apps \
		app_file_types_test.c $(ROOT)/src/apps/solar_os_app_file_types.c -o $@

gameboy_support_test: gameboy_support_test.c $(WRITER_DIR)/solar_os_gameboy_rom.c $(WRITER_DIR)/solar_os_gameboy_rom.h $(WRITER_DIR)/solar_os_gameboy_video.c $(WRITER_DIR)/solar_os_gameboy_video.h $(WRITER_DIR)/solar_os_gameboy_apu.c $(ROOT)/src/vendor/peanut_gb/peanut_gb.h $(ROOT)/src/vendor/peanut_gb/minigb_apu.c $(ROOT)/src/vendor/peanut_gb/minigb_apu.h
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
		-DSOLAR_OS_GAMEBOY_APU_HOST_TEST -I$(ROOT)/src -I$(WRITER_DIR) \
		gameboy_support_test.c $(WRITER_DIR)/solar_os_gameboy_rom.c \
		$(WRITER_DIR)/solar_os_gameboy_video.c \
		$(WRITER_DIR)/solar_os_gameboy_apu.c -o $@

storage_mounts_test: storage_mounts_test.c $(ROOT)/src/services/solar_os_storage.c $(ROOT)/src/services/solar_os_storage.h esp_log.h ff.h solar_os_board.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -ffunction-sections -fdata-sections -include compat.h \
		-I. -I$(ROOT)/src/services -I$(ROOT)/src/board -I$(ROOT)/src/drivers \
		storage_mounts_test.c $(ROOT)/src/services/solar_os_storage.c \
		-Wl,--gc-sections -o $@

keys_test: keys_test.c $(ROOT)/src/services/solar_os_keys.c $(ROOT)/src/services/solar_os_keys.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		keys_test.c $(ROOT)/src/services/solar_os_keys.c -o $@

input_test: input_test.c $(ROOT)/src/services/solar_os_input.c $(ROOT)/src/services/solar_os_input.h esp_timer.h nvs.h freertos/FreeRTOS.h
	$(CC) $(CFLAGS) -include compat.h -I. -I$(ROOT)/src/services \
		input_test.c $(ROOT)/src/services/solar_os_input.c -o $@

hid_keyboard_report_test: hid_keyboard_report_test.c $(ROOT)/src/services/solar_os_hid_keyboard_report.c $(ROOT)/src/services/solar_os_hid_keyboard_report.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		hid_keyboard_report_test.c $(ROOT)/src/services/solar_os_hid_keyboard_report.c -o $@

ps2_keyboard_test: ps2_keyboard_test.c $(ROOT)/src/services/solar_os_ps2_keyboard.c $(ROOT)/src/services/solar_os_ps2_keyboard.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		ps2_keyboard_test.c $(ROOT)/src/services/solar_os_ps2_keyboard.c -o $@

midi_codec_test: midi_codec_test.c $(ROOT)/src/services/solar_os_midi_codec.c $(ROOT)/src/services/solar_os_midi_codec.h
	$(CC) $(CFLAGS) -I$(ROOT)/src/services \
		midi_codec_test.c $(ROOT)/src/services/solar_os_midi_codec.c -o $@

test: $(TARGETS)
	./writer_buffer_test
	./writer_files_test
	./doc_layout_test
	./shell_parse_test
	./shell_launch_test
	./app_file_types_test
	./gameboy_support_test
	./storage_mounts_test
	./keys_test
	./input_test
	./hid_keyboard_report_test
	./ps2_keyboard_test
	./midi_codec_test

clean:
	$(RM) $(TARGETS)
