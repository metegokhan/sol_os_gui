CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror

ROOT := ../..
WRITER_DIR := $(ROOT)/src/apps
TARGETS := writer_buffer_test writer_files_test doc_layout_test shell_parse_test

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

test: $(TARGETS)
	./writer_buffer_test
	./writer_files_test
	./doc_layout_test
	./shell_parse_test

clean:
	$(RM) $(TARGETS)
