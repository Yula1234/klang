CC          := gcc
CFLAGS      := -std=c23 -Wall -Wextra -Wpedantic -g -O2 -Isrc -MMD -MP -D_GNU_SOURCE

SRC_DIR     := src
BUILD_DIR   := build
BIN_DIR     := bin
TARGET      := $(BIN_DIR)/klang

SRCS        := $(shell find $(SRC_DIR) -type f -name '*.c')
OBJS        := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
DEPS        := $(OBJS:.o=.d)

CYAN        := \033[0;36m
GREEN       := \033[0;32m
RESET       := \033[0m

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	@printf "%b[LINK]%b $@\n" "$(CYAN)" "$(RESET)"
	@$(CC) $(OBJS) -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@printf "%b[CC]%b   $<\n" "$(GREEN)" "$(RESET)"
	@$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts."

run: all
	@./$(TARGET)