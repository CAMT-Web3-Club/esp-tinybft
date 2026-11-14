# Host (Linux) build of the TinyBFT cluster simulator.
#
#   make                 # build/sim_replica, build/sim_client
#   make SAN=address,undefined   # with sanitizers (separate build dir)
#   make EXTRA_CFLAGS='-DCONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS=5000'
#   make clean
#
# Every src/*.c except tbft_principal.c (replaced by stub_principal.c) and
# tbft_transport_espnow.c is compiled unmodified; shim/ comes first on the
# include path and provides sdkconfig.h, FreeRTOS, ESP-IDF, lwIP, mbedtls
# and PSA headers.

ROOT      := ../..
SRC_DIR   := $(ROOT)/src
INC_DIR   := $(ROOT)/include

CC        ?= gcc
SAN       ?=
OPT       ?= -O2
ifeq ($(SAN),)
BUILD     ?= build
SANFLAGS  :=
else
BUILD     ?= build-san
SANFLAGS  := -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

CSTD      := -std=gnu23
WARN      := -Wall -Wextra
CPPFLAGS  := -Ishim -I$(SRC_DIR) -I$(INC_DIR)
CFLAGS    := $(CSTD) $(WARN) $(OPT) -g -pthread $(SANFLAGS) $(EXTRA_CFLAGS)
LDFLAGS   := -pthread $(SANFLAGS)

LIB_SRCS  := $(filter-out $(SRC_DIR)/tbft_principal.c $(SRC_DIR)/tbft_transport_espnow.c,\
                          $(wildcard $(SRC_DIR)/*.c))
SHIM_SRCS := $(wildcard shim_src/*.c)
SIM_SRCS  := stub_principal.c sim_common.c

LIB_OBJS  := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/src/%.o,$(LIB_SRCS))
SHIM_OBJS := $(patsubst shim_src/%.c,$(BUILD)/shim/%.o,$(SHIM_SRCS))
SIM_OBJS  := $(patsubst %.c,$(BUILD)/sim/%.o,$(SIM_SRCS))
COMMON    := $(LIB_OBJS) $(SHIM_OBJS) $(SIM_OBJS)

HEADERS   := $(wildcard shim/*.h shim/*/*.h $(SRC_DIR)/*.h $(INC_DIR)/*.h) sim_common.h

all: $(BUILD)/sim_replica $(BUILD)/sim_client

$(BUILD)/sim_replica: $(BUILD)/sim/sim_replica.o $(COMMON)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/sim_client: $(BUILD)/sim/sim_client.o $(COMMON)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/src/%.o: $(SRC_DIR)/%.c $(HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/shim/%.o: shim_src/%.c $(HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/sim/%.o: %.c $(HEADERS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build build-san

.PHONY: all clean
