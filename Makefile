# fanpro - fan control and thermal monitoring for Apple Silicon Macs.
# No external dependencies: macOS system frameworks only.

CC       ?= cc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
            -Wmissing-prototypes -Wno-unused-parameter
OPT      ?= -O2 -g
CPPFLAGS += -Iinclude -D_DARWIN_C_SOURCE
CFLAGS   += $(CSTD) $(WARN) $(OPT)
LDLIBS   := -framework IOKit -framework CoreFoundation

BUILD    := build
BIN      := $(BUILD)/bin
OBJ      := $(BUILD)/obj

LIB      := $(BUILD)/libfanpro.a

# Core library: everything the CLI, daemon, and tests share.
LIB_SRC  := $(wildcard src/smc/*.c) \
            $(wildcard src/sensors/*.c) \
            $(wildcard src/fan/*.c) \
            $(wildcard src/common/*.c)
CLI_SRC  := $(wildcard src/cli/*.c) $(wildcard src/tui/*.c)
DAE_SRC  := $(wildcard src/daemon/*.c)
TEST_SRC := $(wildcard tests/*.c)
# The daemon logic is tested too, so its objects (including fanprod.c for the
# state-marker and queue helpers) are linked into the test binary. fanprod.c's
# main() is renamed out of the way by -Dmain=fanprod_main_unused -Wno-missing-prototypes.

LIB_OBJ  := $(LIB_SRC:%.c=$(OBJ)/%.o)
CLI_OBJ  := $(CLI_SRC:%.c=$(OBJ)/%.o)
DAE_OBJ  := $(DAE_SRC:%.c=$(OBJ)/%.o)
TEST_OBJ := $(TEST_SRC:%.c=$(OBJ)/%.o)

DEPS     := $(LIB_OBJ:.o=.d) $(CLI_OBJ:.o=.d) $(DAE_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(DAE_TEST_OBJ:.o=.d)

.PHONY: all clean test check-live install uninstall fmt

# fanprod is built only once daemon sources exist (build step 8).
DAEMON_TARGET := $(if $(DAE_SRC),$(BIN)/fanprod,)

all: $(BIN)/fanpro $(DAEMON_TARGET)

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BIN)/fanpro: $(CLI_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(LIB) $(LDLIBS) -lncurses

$(BIN)/fanprod: $(DAE_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(DAE_OBJ) $(LIB) $(LDLIBS)

$(OBJ)/src/daemon/%.o: CPPFLAGS += -Isrc/daemon

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

# Fast unit tests: no hardware, no root, must stay well under a second.
$(BIN)/fanpro_test: CPPFLAGS += -Itests
$(OBJ)/tests/%.o: CPPFLAGS += -Isrc/daemon

DAE_TEST_OBJ := $(patsubst %.c,$(OBJ)/test_%.o,$(notdir $(DAE_SRC)))

$(OBJ)/test_%.o: src/daemon/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Isrc/daemon -Dmain=fanprod_main_unused $(CFLAGS) -Wno-missing-prototypes -MMD -MP -c -o $@ $<

$(BIN)/fanpro_test: $(TEST_OBJ) $(DAE_TEST_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJ) $(DAE_TEST_OBJ) $(LIB) $(LDLIBS)

test: $(BIN)/fanpro_test
	@$(BIN)/fanpro_test

# Read-only sanity pass against real hardware.  Deliberately not part of `test`.
check-live: $(BIN)/fanpro_test
	@FANPRO_LIVE=1 $(BIN)/fanpro_test

install: all
	install -d /usr/local/bin /usr/local/sbin /etc/fanpro
	install -m 0755 $(BIN)/fanpro  /usr/local/bin/fanpro
	install -m 0755 $(BIN)/fanprod /usr/local/sbin/fanprod
	@test -f /etc/fanpro/fanpro.conf || install -m 0644 etc/fanpro.conf.example /etc/fanpro/fanpro.conf
	@echo "installed.  run: sudo fanpro daemon install"

uninstall:
	rm -f /usr/local/bin/fanpro /usr/local/sbin/fanprod

clean:
	rm -rf $(BUILD)

-include $(DEPS)
