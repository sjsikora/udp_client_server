# Compiler & flags
CC := gcc
CFLAGS := -Wall -Wextra -Iinclude -g -MMD -MP

# Directories
SRC_DIR := src
INC_DIR := include
LIB_DIR := lib
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

# Build client
$(CLIENT_BIN): $(CLIENT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@

# Build server
$(SERVER_BIN): $(SERVER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@

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
