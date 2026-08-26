CC          := gcc
CFLAGS      := -std=c23 -Wall -Wextra -Wpedantic -g -O2 -Isrc -MMD -MP -D_GNU_SOURCE

SRC_DIR     := src
BUILD_DIR   := build
BIN_DIR     := bin
TARGET      := $(BIN_DIR)/klang

SRCS        := $(shell find $(SRC_DIR) -type f -name '*.c')
OBJS        := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
DEPS        := $(OBJS:.o=.d)

FASM            := fasm
FASMFLAGS       := -m 524288

SELFHOST_DIR    := selfhost
SELFHOST_TARGET := $(BIN_DIR)/klang-selfhost
SELFHOST_SRCS   := $(shell find $(SELFHOST_DIR) -type f -name '*.kl' 2>/dev/null)
SELFHOST_ASM    := $(BUILD_DIR)/selfhost.asm
SELFHOST_OBJ    := $(BUILD_DIR)/selfhost.o

YELLOW      := \033[0;33m
CYAN        := \033[0;36m
GREEN       := \033[0;32m
RESET       := \033[0m

.PHONY: all clean run selfhost

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

selfhost: $(SELFHOST_TARGET)

$(SELFHOST_TARGET): $(TARGET) $(SELFHOST_SRCS)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	@printf "%b[KLANG]%b $(SELFHOST_DIR)/main.kl -> $(SELFHOST_ASM)\n" "$(YELLOW)" "$(RESET)"
	@$(TARGET) $(SELFHOST_DIR)/main.kl -I $(SELFHOST_DIR) -o $(SELFHOST_ASM)
	@printf "%b[FASM]%b  $(SELFHOST_ASM)\n" "$(GREEN)" "$(RESET)"
	@$(FASM) $(FASMFLAGS) $(SELFHOST_ASM) $(SELFHOST_OBJ)
	@printf "%b[LINK]%b $@\n" "$(CYAN)" "$(RESET)"
	@$(CC) -no-pie $(SELFHOST_OBJ) -o $@