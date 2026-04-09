# File build cục bộ đơn giản cho demo bộ quản lý workspace chạy trên máy host.
# Binary được đặt trong `out/` để tách file sinh ra khỏi cây source.
CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -pedantic
TARGET := ftel_workspace_manager
SRC := new_cmd_line.c
OUT_DIR := out
OUTPUT := $(OUT_DIR)/$(TARGET)

# Phony target không ánh xạ trực tiếp tới file thật và luôn được chạy khi gọi.
.PHONY: all build run clean

# Điểm vào mặc định khi người dùng chỉ gõ `make`.
all: build

# Chỉ build binary của bộ quản lý.
build: $(OUTPUT)

# Biên dịch với `main()` độc lập để chạy trực tiếp trên terminal.
$(OUTPUT): $(SRC) new_cmd_line.h | $(OUT_DIR)
	$(CC) $(CFLAGS) $(SRC) -DNEW_CMD_LINE_ENABLE_MAIN -o $(OUTPUT)

# Tạo thư mục output nếu hiện tại chưa tồn tại.
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Chạy bộ quản lý tương tác sau khi đảm bảo binary đã được tạo.
run: $(OUTPUT)
	./$(OUTPUT)

# Xóa các file sinh ra và đưa thư mục build về trạng thái sạch.
clean:
	rm -rf $(OUT_DIR)
