/**
 ******************************************************************************
 * @author: Annk
 * @date:   05/04/2026
 ******************************************************************************
**/

#ifndef __NEW_CMD_LINE_H__
#define __NEW_CMD_LINE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdint.h>
#include <time.h>

/*
 * Nhóm cấu hình public.
 * Có thể override các macro này trước khi include header nếu project cần
 * thay đổi độ dài input, độ sâu history hoặc kích thước prompt.
 */
#ifndef NEW_CMD_MAX_INPUT_LEN
#define NEW_CMD_MAX_INPUT_LEN 256U
#endif

#ifndef NEW_CMD_MAX_ARGS
#define NEW_CMD_MAX_ARGS 10U
#endif

#ifndef NEW_CMD_HISTORY_DEPTH
#define NEW_CMD_HISTORY_DEPTH 8U
#endif

#ifndef NEW_CMD_MAX_PROMPT_LEN
#define NEW_CMD_MAX_PROMPT_LEN 32U
#endif

#define NEW_CMD_DEFAULT_PROMPT "ftel> "

/* Mã trạng thái mức cao được trả về bởi parser và bộ điều phối lệnh. */
typedef enum {
	NEW_CMD_STATUS_OK = 0,
	NEW_CMD_STATUS_EMPTY_INPUT,
	NEW_CMD_STATUS_NULL_CONTEXT,
	NEW_CMD_STATUS_INPUT_TOO_LONG,
	NEW_CMD_STATUS_TOO_MANY_ARGS,
	NEW_CMD_STATUS_PARSE_ERROR,
	NEW_CMD_STATUS_NOT_FOUND,
	NEW_CMD_STATUS_HANDLER_ERROR
} new_cmd_status_t;

/* Khai báo trước để handler có thể nhận runtime context. */
typedef struct new_cmd_context new_cmd_context_t;

/* Callback ghi dữ liệu khi cần chuyển hướng output thay vì dùng stdout. */
typedef void (*new_cmd_writer_t)(const char* text, void* user_data);

/* Kiểu hàm handler dùng cho cả bảng lệnh built-in và bảng lệnh mở rộng. */
typedef int32_t (*new_cmd_handler_t)(new_cmd_context_t* ctx, int argc, char* argv[]);

/* Một phần tử trong bảng lệnh: tên, hàm xử lý, cú pháp dùng và mô tả. */
typedef struct {
	const char* name;
	new_cmd_handler_t handler;
	const char* usage;
	const char* description;
} new_cmd_command_t;

/*
 * Trạng thái runtime của shell.
 * Cấu trúc này được tái sử dụng cho cả module CLI tổng quát và phần demo
 * workspace-manager chạy trên máy host khi biên dịch với
 * NEW_CMD_LINE_ENABLE_MAIN.
 */
struct new_cmd_context {
	const char* app_name;
	const char* version;
	const new_cmd_command_t* external_commands;
	new_cmd_writer_t writer;
	void* writer_user_data;
	time_t started_at;
	uint32_t history_total;
	uint8_t running;
	uint8_t history_count;
	uint8_t history_head;
	char prompt[NEW_CMD_MAX_PROMPT_LEN];
	char history[NEW_CMD_HISTORY_DEPTH][NEW_CMD_MAX_INPUT_LEN];
};

/* Khởi tạo context với giá trị mặc định và thông tin nhận diện tùy chọn. */
void new_cmd_line_init(new_cmd_context_t* ctx, const char* app_name, const char* version);

/* Thay đường xuất stdout bằng callback ghi dữ liệu tùy chỉnh. */
void new_cmd_line_set_writer(new_cmd_context_t* ctx, new_cmd_writer_t writer, void* user_data);

/* Cập nhật prompt đang hiển thị trong vòng lặp REPL tương tác. */
void new_cmd_line_set_prompt(new_cmd_context_t* ctx, const char* prompt);

/* Gắn thêm bảng lệnh mở rộng bên ngoài, bổ sung cho built-in commands. */
void new_cmd_line_attach_commands(new_cmd_context_t* ctx, const new_cmd_command_t* commands);

/* Yêu cầu vòng lặp tương tác dừng sau khi lệnh hiện tại chạy xong. */
void new_cmd_line_stop(new_cmd_context_t* ctx);

/* Bắt đầu vòng lặp read-eval-print dạng blocking, đọc từ stdin. */
void new_cmd_line_run(new_cmd_context_t* ctx);

/* In output có định dạng bằng writer đã cấu hình hoặc fallback sang stdout. */
void new_cmd_line_printf(new_cmd_context_t* ctx, const char* format, ...);

/* Xử lý một dòng lệnh đơn mà không cần đi vào vòng lặp tương tác. */
new_cmd_status_t new_cmd_line_process(new_cmd_context_t* ctx, const char* input);

/* Chuyển mã trạng thái sang chuỗi dễ đọc để debug hoặc log. */
const char* new_cmd_line_status_string(new_cmd_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* __NEW_CMD_LINE_H__ */
