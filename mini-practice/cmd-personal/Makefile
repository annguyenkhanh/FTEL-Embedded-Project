# File build cuc bo don gian cho demo workspace manager chay tren may host.
# Binary duoc dat trong `out/` de tach file sinh ra khoi source tree.
CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -pedantic
TARGET := ftel_workspace_manager
SRC := new_cmd_line.c
OUT_DIR := out
OUTPUT := $(OUT_DIR)/$(TARGET)

# Phony target khong anh xa truc tiep toi file that va luon duoc chay khi goi.
.PHONY: all build run clean

# Diem vao mac dinh khi nguoi dung chi go `make`.
all: build

# Chi build binary cua manager.
build: $(OUTPUT)

# Bien dich voi `main()` standalone de chay truc tiep tren terminal.
$(OUTPUT): $(SRC) new_cmd_line.h | $(OUT_DIR)
	$(CC) $(CFLAGS) $(SRC) -DNEW_CMD_LINE_ENABLE_MAIN -o $(OUTPUT)

# Tao thu muc output neu hien tai chua ton tai.
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Chay manager tuong tac sau khi dam bao binary da duoc tao.
run: $(OUTPUT)
	./$(OUTPUT)

# Xoa cac file sinh ra va dua thu muc build ve trang thai sach.
clean:
	rm -rf $(OUT_DIR)
