# Compiler & flags
CC := gcc
# Added -I/usr/local/include for zlog headers
CFLAGS := -Wall -Wextra -Iinclude -I/usr/local/include -g -MMD -MP

# Added linker flags and libraries for zlog
LDFLAGS := -L/usr/local/lib
LDLIBS := -lzlog -lpthread

# Directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build

# Source files
SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Separate binaries
CLIENT_BIN := $(BUILD_DIR)/client
SERVER_BIN := $(BUILD_DIR)/server

# Identify objects per binary
CLIENT_OBJS := $(filter-out $(BUILD_DIR)/server.o,$(OBJS))
SERVER_OBJS := $(filter-out $(BUILD_DIR)/client.o,$(OBJS))

# Default target
all: $(CLIENT_BIN) $(SERVER_BIN)

# Build client (Added LDFLAGS and LDLIBS at the end)
$(CLIENT_BIN): $(CLIENT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# Build server (Added LDFLAGS and LDLIBS at the end)
$(SERVER_BIN): $(SERVER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# Compile source files to objects
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Include dependencies
-include $(DEPS)

# Clean
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

# Compile commands for clangd or other tools
.PHONY: compile_commands
compile_commands: all
	@mkdir -p $(BUILD_DIR)
	@bear -- $(MAKE)
