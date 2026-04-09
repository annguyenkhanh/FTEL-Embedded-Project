/**
 ******************************************************************************
 * @author: Annk
 * @date:   05/04/2026
 *
 * Tổng quan module
 * ----------------
 * File này triển khai 2 lớp chính:
 * 1. Một bộ máy command-line cho terminal có thể tái sử dụng, bao gồm
 *    parsing, history, help, quản lý prompt và điều phối lệnh.
 * 2. Một demo workspace manager chạy trên máy host, có khả năng quét các
 *    project dùng Makefile và gọi build target từ một shell tương tác duy nhất.
 ******************************************************************************
**/

#include "new_cmd_line.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NEW_CMD_LINE_ENABLE_MAIN
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#define NEW_CMD_OUTPUT_BUFFER_SIZE 512U

/* -------------------------------------------------------------------------- */
/* Khai báo các lệnh tích hợp sẵn                                             */
/* -------------------------------------------------------------------------- */
static int32_t builtin_help(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_about(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_version(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_system(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_status(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_time(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_echo(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_prompt(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_history(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_clear(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t builtin_exit(new_cmd_context_t* ctx, int argc, char* argv[]);

#ifdef NEW_CMD_LINE_ENABLE_MAIN
static int read_line_with_completion(new_cmd_context_t* ctx, char* input, size_t input_size);
#endif

/* Các lệnh tích hợp sẵn luôn sẵn sàng, kể cả khi không gắn bảng lệnh bên ngoài. */
static const new_cmd_command_t g_builtin_commands[] = {
	{"help", builtin_help, "help [command]", "Show all commands or detailed help for one command."},
	{"about", builtin_about, "about", "Display shell purpose and supported features."},
	{"version", builtin_version, "version", "Print application version and build metadata."},
	{"system", builtin_system, "system", "Show compiler, platform and C standard information."},
	{"status", builtin_status, "status", "Show shell runtime status, prompt and command counters."},
	{"time", builtin_time, "time", "Show the current local time and shell uptime."},
	{"echo", builtin_echo, "echo <text>", "Print text exactly as provided to the command line."},
	{"prompt", builtin_prompt, "prompt [new prompt]", "Show or update the active shell prompt."},
	{"history", builtin_history, "history [clear]", "List command history or clear the history buffer."},
	{"clear", builtin_clear, "clear", "Clear the terminal screen with ANSI escape codes."},
	{"exit", builtin_exit, "exit", "Close the interactive terminal session."},
	{NULL, NULL, NULL, NULL}
};

/* Đường xuất mặc định khi chạy ở chế độ host-terminal. */
static void default_writer(const char* text, void* user_data) {
	FILE* stream = (user_data != NULL) ? (FILE*)user_data : stdout;

	fputs(text, stream);
	fflush(stream);
}

/* Điều hướng output của shell qua writer tùy chỉnh nếu đã được cấu hình. */
static void write_text(new_cmd_context_t* ctx, const char* text) {
	if ((ctx == NULL) || (text == NULL)) {
		return;
	}

	if (ctx->writer != NULL) {
		ctx->writer(text, ctx->writer_user_data);
		return;
	}

	default_writer(text, NULL);
}

/* So sánh không phân biệt hoa-thường giúp việc gõ lệnh thân thiện hơn. */
static int command_name_equals(const char* left, const char* right) {
	unsigned char lhs;
	unsigned char rhs;

	if ((left == NULL) || (right == NULL)) {
		return 0;
	}

	while ((*left != '\0') && (*right != '\0')) {
		lhs = (unsigned char)tolower((unsigned char)*left);
		rhs = (unsigned char)tolower((unsigned char)*right);

		if (lhs != rhs) {
			return 0;
		}

		left++;
		right++;
	}

	return (*left == '\0') && (*right == '\0');
}

/* Đếm số phần tử trong một bảng lệnh kết thúc bằng NULL. */
static uint32_t count_commands(const new_cmd_command_t* table) {
	uint32_t count = 0U;

	if (table == NULL) {
		return 0U;
	}

	while (table[count].name != NULL) {
		count++;
	}

	return count;
}

/* Tìm lệnh trùng khớp trong một bảng lệnh đơn lẻ. */
static const new_cmd_command_t* find_in_table(const new_cmd_command_t* table, const char* name) {
	uint32_t index = 0U;

	if ((table == NULL) || (name == NULL)) {
		return NULL;
	}

	while (table[index].name != NULL) {
		if (command_name_equals(table[index].name, name) != 0) {
			return &table[index];
		}

		index++;
	}

	return NULL;
}

/*
 * Phân giải tên lệnh theo thứ tự: lệnh mở rộng của người dùng trước, lệnh
 * tích hợp sẵn sau.
 * Dấu '*' ở đầu lệnh sẽ bị bỏ qua, để các kiểu gõ như
 * "*Build project demo" vẫn được map về handler "build" thông thường.
 */
static const new_cmd_command_t* find_command(const new_cmd_context_t* ctx, const char* name) {
	const new_cmd_command_t* command = NULL;
	const char* lookup_name = name;

	if ((ctx == NULL) || (name == NULL)) {
		return NULL;
	}

	while (*lookup_name == '*') {
		lookup_name++;
	}

	/* Ưu tiên lệnh mở rộng để shell có thể tái sử dụng cho nhiều project khác nhau. */
	command = find_in_table(ctx->external_commands, lookup_name);
	if (command != NULL) {
		return command;
	}

	return find_in_table(g_builtin_commands, lookup_name);
}

/* Hàm hỗ trợ nhỏ, được dùng khi in status và help. */
static uint32_t total_command_count(const new_cmd_context_t* ctx) {
	return count_commands(g_builtin_commands) + count_commands(ctx != NULL ? ctx->external_commands : NULL);
}

/* Sao chép prompt và đảm bảo không vượt quá giới hạn đã khai báo khi biên dịch. */
static void copy_prompt(new_cmd_context_t* ctx, const char* prompt) {
	size_t prompt_length = 0U;

	if (ctx == NULL) {
		return;
	}

	if ((prompt == NULL) || (prompt[0] == '\0')) {
		prompt = NEW_CMD_DEFAULT_PROMPT;
	}

	prompt_length = strlen(prompt);
	if (prompt_length >= NEW_CMD_MAX_PROMPT_LEN) {
		prompt_length = NEW_CMD_MAX_PROMPT_LEN - 1U;
	}

	memcpy(ctx->prompt, prompt, prompt_length);
	ctx->prompt[prompt_length] = '\0';
}

/* Cắt bỏ khoảng trắng ở đầu/cuối trước khi parse dòng lệnh. */
static new_cmd_status_t copy_trimmed_input(const char* input, char* output, size_t output_size) {
	const char* start = input;
	const char* end = NULL;
	size_t length = 0U;

	if ((input == NULL) || (output == NULL) || (output_size == 0U)) {
		return NEW_CMD_STATUS_EMPTY_INPUT;
	}

	while ((*start != '\0') && isspace((unsigned char)*start)) {
		start++;
	}

	end = start + strlen(start);
	while ((end > start) && isspace((unsigned char)end[-1])) {
		end--;
	}

	length = (size_t)(end - start);
	if (length == 0U) {
		output[0] = '\0';
		return NEW_CMD_STATUS_EMPTY_INPUT;
	}

	if (length >= output_size) {
		output[0] = '\0';
		return NEW_CMD_STATUS_INPUT_TOO_LONG;
	}

	memcpy(output, start, length);
	output[length] = '\0';
	return NEW_CMD_STATUS_OK;
}

/* Lưu các lệnh mới nhất vào bộ đệm history dạng vòng tròn. */
static void push_history(new_cmd_context_t* ctx, const char* command_line) {
	if ((ctx == NULL) || (command_line == NULL) || (command_line[0] == '\0')) {
		return;
	}

	strncpy(ctx->history[ctx->history_head], command_line, NEW_CMD_MAX_INPUT_LEN - 1U);
	ctx->history[ctx->history_head][NEW_CMD_MAX_INPUT_LEN - 1U] = '\0';
	ctx->history_head = (uint8_t)((ctx->history_head + 1U) % NEW_CMD_HISTORY_DEPTH);

	if (ctx->history_count < NEW_CMD_HISTORY_DEPTH) {
		ctx->history_count++;
	}

	ctx->history_total++;
}

/* Xóa vòng history mà không ảnh hưởng đến các trạng thái shell khác. */
static void clear_history(new_cmd_context_t* ctx) {
	uint32_t index = 0U;

	if (ctx == NULL) {
		return;
	}

	for (index = 0U; index < NEW_CMD_HISTORY_DEPTH; ++index) {
		ctx->history[index][0] = '\0';
	}

	ctx->history_count = 0U;
	ctx->history_head = 0U;
}

/*
 * Tách một dòng input thành các token kiểu argv.
 * Chuỗi nằm trong dấu nháy được giữ nguyên thành một tham số, và các ký tự
 * escape được copy qua để handler nhận được danh sách token "sạch".
 */
static new_cmd_status_t tokenize_input(const char* input, char* token_buffer, int* argc_out, char* argv[]) {
	const char* read_ptr = input;
	char* write_ptr = token_buffer;
	int argc = 0;

	if ((input == NULL) || (token_buffer == NULL) || (argc_out == NULL) || (argv == NULL)) {
		return NEW_CMD_STATUS_PARSE_ERROR;
	}

	/*
	 * Tách token vào bộ đệm tạm để argv trỏ tới các chuỗi ổn định,
	 * tránh sửa trực tiếp trên input gốc.
	 */
	while (*read_ptr != '\0') {
		while ((*read_ptr != '\0') && isspace((unsigned char)*read_ptr)) {
			read_ptr++;
		}

		if (*read_ptr == '\0') {
			break;
		}

		if (argc >= (int)NEW_CMD_MAX_ARGS) {
			return NEW_CMD_STATUS_TOO_MANY_ARGS;
		}

		argv[argc++] = write_ptr;

		if ((*read_ptr == '"') || (*read_ptr == '\'')) {
			char quote = *read_ptr++;

			while ((*read_ptr != '\0') && (*read_ptr != quote)) {
				if ((*read_ptr == '\\') && (read_ptr[1] != '\0')) {
					read_ptr++;
				}

				*write_ptr++ = *read_ptr++;
			}

			if (*read_ptr != quote) {
				*write_ptr = '\0';
				return NEW_CMD_STATUS_PARSE_ERROR;
			}

			read_ptr++;
		}
		else {
			while ((*read_ptr != '\0') && !isspace((unsigned char)*read_ptr)) {
				if ((*read_ptr == '\\') && (read_ptr[1] != '\0')) {
					read_ptr++;
				}

				*write_ptr++ = *read_ptr++;
			}
		}

		*write_ptr++ = '\0';
	}

	*argc_out = argc;
	return (argc == 0) ? NEW_CMD_STATUS_EMPTY_INPUT : NEW_CMD_STATUS_OK;
}

/* Ghép lại chuỗi từ argv[start_index..] cho các lệnh dạng echo/prompt. */
static void join_arguments(int argc, char* argv[], int start_index, char* output, size_t output_size) {
	size_t used = 0U;
	int index = 0;

	if ((output == NULL) || (output_size == 0U)) {
		return;
	}

	output[0] = '\0';

	for (index = start_index; index < argc; ++index) {
		int written = snprintf(
			output + used,
			output_size - used,
			"%s%s",
			(index > start_index) ? " " : "",
			argv[index]
		);

		if (written < 0) {
			output[0] = '\0';
			return;
		}

		if ((size_t)written >= (output_size - used)) {
			output[output_size - 1U] = '\0';
			return;
		}

		used += (size_t)written;
	}
}

/* Định dạng giá trị thời gian thành timestamp local dễ đọc. */
static void format_time_value(time_t raw_time, char* output, size_t output_size) {
	struct tm* local_snapshot = NULL;

	if ((output == NULL) || (output_size == 0U)) {
		return;
	}

	local_snapshot = localtime(&raw_time);
	if (local_snapshot == NULL) {
		snprintf(output, output_size, "unavailable");
		return;
	}

	strftime(output, output_size, "%Y-%m-%d %H:%M:%S", local_snapshot);
}

/* Định dạng uptime của shell theo dạng dd hh:mm:ss hoặc hh:mm:ss. */
static void format_uptime(const new_cmd_context_t* ctx, char* output, size_t output_size) {
	long uptime_seconds = 0L;
	long days = 0L;
	long hours = 0L;
	long minutes = 0L;
	long seconds = 0L;
	time_t now = 0;

	if ((ctx == NULL) || (output == NULL) || (output_size == 0U)) {
		return;
	}

	now = time(NULL);
	if (now >= ctx->started_at) {
		uptime_seconds = (long)(now - ctx->started_at);
	}

	days = uptime_seconds / 86400L;
	hours = (uptime_seconds % 86400L) / 3600L;
	minutes = (uptime_seconds % 3600L) / 60L;
	seconds = uptime_seconds % 60L;

	if (days > 0L) {
		snprintf(output, output_size, "%ldd %02ld:%02ld:%02ld", days, hours, minutes, seconds);
	}
	else {
		snprintf(output, output_size, "%02ld:%02ld:%02ld", hours, minutes, seconds);
	}
}

/* Các hàm hỗ trợ về compiler/platform giúp status in đủ thông tin. */
static const char* compiler_name(void) {
#if defined(__clang__)
	return "Clang/LLVM";
#elif defined(__GNUC__)
	return "GCC";
#elif defined(_MSC_VER)
	return "MSVC";
#else
	return "Unknown compiler";
#endif
}

static const char* platform_name(void) {
#if defined(_WIN32)
	return "Windows";
#elif defined(__APPLE__)
	return "macOS";
#elif defined(__linux__)
	return "Linux";
#elif defined(__unix__)
	return "Unix";
#else
	return "Unknown platform";
#endif
}

static const char* c_standard_name(void) {
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
	return "C23";
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201710L)
	return "C18";
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
	return "C11";
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
	return "C99";
#else
	return "C90/C89";
#endif
}

/* In mỗi lệnh trên một dòng ngắn gọn cho phần tóm tắt của `help`. */
static void print_command_summary(new_cmd_context_t* ctx, const new_cmd_command_t* table) {
	uint32_t index = 0U;

	if ((ctx == NULL) || (table == NULL)) {
		return;
	}

	while (table[index].name != NULL) {
		new_cmd_line_printf(
			ctx,
			"  %-12s %s\n",
			table[index].name,
			(table[index].description != NULL) ? table[index].description : ""
		);
		index++;
	}
}

/* In hướng dẫn chi tiết cho một lệnh cụ thể. */
static void print_command_help(new_cmd_context_t* ctx, const new_cmd_command_t* command) {
	if ((ctx == NULL) || (command == NULL)) {
		return;
	}

	new_cmd_line_printf(ctx, "Command : %s\n", command->name);
	new_cmd_line_printf(
		ctx,
		"Usage   : %s\n",
		(command->usage != NULL) ? command->usage : command->name
	);
	new_cmd_line_printf(
		ctx,
		"Details : %s\n",
		(command->description != NULL) ? command->description : "No description available."
	);
}

/* Dọn các byte còn sót trong stdin sau khi người dùng nhập quá dài. */
static void discard_stdin_line(void) {
	int ch = 0;

	do {
		ch = getchar();
	} while ((ch != '\n') && (ch != EOF));
}

/* Hàm in có định dạng công khai, được các command handler gọi lại. */
void new_cmd_line_printf(new_cmd_context_t* ctx, const char* format, ...) {
	char message[NEW_CMD_OUTPUT_BUFFER_SIZE];
	va_list arguments;

	if ((ctx == NULL) || (format == NULL)) {
		return;
	}

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);

	write_text(ctx, message);
}

/* Khởi tạo context runtime về trạng thái mặc định, rõ ràng. */
void new_cmd_line_init(new_cmd_context_t* ctx, const char* app_name, const char* version) {
	if (ctx == NULL) {
		return;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->app_name = (app_name != NULL) ? app_name : "FTEL Terminal Shell";
	ctx->version = (version != NULL) ? version : "1.0.0";
	ctx->started_at = time(NULL);
	ctx->running = 1U;
	copy_prompt(ctx, NEW_CMD_DEFAULT_PROMPT);
}

/* Gắn writer tùy chỉnh để sau này có thể nhúng shell vào môi trường khác. */
void new_cmd_line_set_writer(new_cmd_context_t* ctx, new_cmd_writer_t writer, void* user_data) {
	if (ctx == NULL) {
		return;
	}

	ctx->writer = writer;
	ctx->writer_user_data = user_data;
}

/* Hàm công khai để đổi prompt, được dùng bởi lệnh `prompt` và code bên ngoài. */
void new_cmd_line_set_prompt(new_cmd_context_t* ctx, const char* prompt) {
	copy_prompt(ctx, prompt);
}

/* Gắn thêm bảng lệnh mở rộng đặc thù cho từng project. */
void new_cmd_line_attach_commands(new_cmd_context_t* ctx, const new_cmd_command_t* commands) {
	if (ctx == NULL) {
		return;
	}

	ctx->external_commands = commands;
}

/* Đánh dấu để vòng lặp REPL dừng lại. */
void new_cmd_line_stop(new_cmd_context_t* ctx) {
	if (ctx == NULL) {
		return;
	}

	ctx->running = 0U;
}

/* Dạng chuỗi của status hữu ích cho gỡ lỗi hoặc test parser API. */
const char* new_cmd_line_status_string(new_cmd_status_t status) {
	switch (status) {
	case NEW_CMD_STATUS_OK:
		return "OK";
	case NEW_CMD_STATUS_EMPTY_INPUT:
		return "EMPTY_INPUT";
	case NEW_CMD_STATUS_NULL_CONTEXT:
		return "NULL_CONTEXT";
	case NEW_CMD_STATUS_INPUT_TOO_LONG:
		return "INPUT_TOO_LONG";
	case NEW_CMD_STATUS_TOO_MANY_ARGS:
		return "TOO_MANY_ARGS";
	case NEW_CMD_STATUS_PARSE_ERROR:
		return "PARSE_ERROR";
	case NEW_CMD_STATUS_NOT_FOUND:
		return "NOT_FOUND";
	case NEW_CMD_STATUS_HANDLER_ERROR:
		return "HANDLER_ERROR";
	default:
		return "UNKNOWN_STATUS";
	}
}

/*
 * Điểm vào xử lý một dòng lệnh đơn.
 * Hàm này có thể tái sử dụng ngoài vòng lặp tương tác, ví dụ cho unit test,
 * input từ script hoặc tích hợp với front-end khác.
 */
new_cmd_status_t new_cmd_line_process(new_cmd_context_t* ctx, const char* input) {
	char command_line[NEW_CMD_MAX_INPUT_LEN];
	char token_buffer[NEW_CMD_MAX_INPUT_LEN];
	char* argv[NEW_CMD_MAX_ARGS];
	const new_cmd_command_t* command = NULL;
	new_cmd_status_t status = NEW_CMD_STATUS_OK;
	int argc = 0;
	int32_t handler_status = 0;

	if (ctx == NULL) {
		return NEW_CMD_STATUS_NULL_CONTEXT;
	}

	/* Luồng xử lý: chuẩn hóa input, lưu history, tách argv, tìm và gọi handler. */
	status = copy_trimmed_input(input, command_line, sizeof(command_line));
	if (status == NEW_CMD_STATUS_EMPTY_INPUT) {
		return status;
	}

	if (status == NEW_CMD_STATUS_INPUT_TOO_LONG) {
		new_cmd_line_printf(ctx, "Error: command is too long. Maximum length is %u characters.\n", NEW_CMD_MAX_INPUT_LEN - 1U);
		return status;
	}

	push_history(ctx, command_line);

	status = tokenize_input(command_line, token_buffer, &argc, argv);
	if (status == NEW_CMD_STATUS_TOO_MANY_ARGS) {
		new_cmd_line_printf(ctx, "Error: too many arguments. Maximum supported arguments: %u.\n", NEW_CMD_MAX_ARGS);
		return status;
	}

	if (status == NEW_CMD_STATUS_PARSE_ERROR) {
		new_cmd_line_printf(ctx, "Error: invalid command syntax. Check quotes and escaped characters.\n");
		return status;
	}

	if (status != NEW_CMD_STATUS_OK) {
		return status;
	}

	command = find_command(ctx, argv[0]);
	if (command == NULL) {
		new_cmd_line_printf(ctx, "Unknown command: %s\n", argv[0]);
		new_cmd_line_printf(ctx, "Type 'help' to list available commands.\n");
		return NEW_CMD_STATUS_NOT_FOUND;
	}

	handler_status = command->handler(ctx, argc, argv);
	if (handler_status != 0) {
		return NEW_CMD_STATUS_HANDLER_ERROR;
	}

	return NEW_CMD_STATUS_OK;
}

/* Vòng lặp đọc-thực thi-in đang blocking cho chế độ terminal. */
void new_cmd_line_run(new_cmd_context_t* ctx) {
	char input[NEW_CMD_MAX_INPUT_LEN];

	if (ctx == NULL) {
		return;
	}

	/* REPL blocking đơn giản dành cho phiên terminal trên máy host. */
	ctx->running = 1U;
	new_cmd_line_printf(ctx, "%s (%s)\n", ctx->app_name, ctx->version);
	new_cmd_line_printf(ctx, "Interactive shell ready. Type 'help' to list commands.\n\n");

	while (ctx->running != 0U) {
#ifdef NEW_CMD_LINE_ENABLE_MAIN
		if (isatty(STDIN_FILENO) != 0) {
			if (read_line_with_completion(ctx, input, sizeof(input)) != 0) {
				new_cmd_line_printf(ctx, "\nTerminal input closed.\n");
				break;
			}
		}
		else
#endif
		{
		new_cmd_line_printf(ctx, "%s", ctx->prompt);

		if (fgets(input, sizeof(input), stdin) == NULL) {
			new_cmd_line_printf(ctx, "\nTerminal input closed.\n");
			break;
		}

		if ((strchr(input, '\n') == NULL) && (strlen(input) == (NEW_CMD_MAX_INPUT_LEN - 1U))) {
			discard_stdin_line();
			new_cmd_line_printf(ctx, "Error: command is too long. Maximum length is %u characters.\n", NEW_CMD_MAX_INPUT_LEN - 1U);
			continue;
		}
		}

		(void)new_cmd_line_process(ctx, input);
	}
}

/* -------------------------------------------------------------------------- */
/* Các lệnh shell tích hợp sẵn                                                */
/* -------------------------------------------------------------------------- */

/* `help` liệt kê các lệnh hoặc in hướng dẫn chi tiết cho một lệnh. */
static int32_t builtin_help(new_cmd_context_t* ctx, int argc, char* argv[]) {
	const new_cmd_command_t* command = NULL;

	if (argc == 1) {
		new_cmd_line_printf(ctx, "\nAvailable commands\n");
		if (ctx->external_commands != NULL) {
			print_command_summary(ctx, ctx->external_commands);
		}
		print_command_summary(ctx, g_builtin_commands);
		new_cmd_line_printf(ctx, "\nExamples\n");
		new_cmd_line_printf(ctx, "  projects\n");
		new_cmd_line_printf(ctx, "  build cmd-personal\n");
		new_cmd_line_printf(ctx, "  run project cmd-personal\n");
		new_cmd_line_printf(ctx, "  *Build project cmd-personal\n\n");
		return 0;
	}

	if (argc == 2) {
		command = find_command(ctx, argv[1]);
		if (command == NULL) {
			new_cmd_line_printf(ctx, "No help found for command: %s\n", argv[1]);
			return -1;
		}

		print_command_help(ctx, command);
		return 0;
	}

	new_cmd_line_printf(ctx, "Usage: help [command]\n");
	return -1;
}

/* `about` mô tả shell hiện tại và các tính năng chính. */
static int32_t builtin_about(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	new_cmd_line_printf(ctx, "%s\n", ctx->app_name);
	new_cmd_line_printf(ctx, "Version  : %s\n", ctx->version);
	new_cmd_line_printf(ctx, "Purpose  : Terminal workspace manager for Makefile-based learning projects.\n");
	new_cmd_line_printf(ctx, "Features : Built-in help, quoted argument parsing, history, custom prompts, project discovery and make target execution.\n");
	new_cmd_line_printf(ctx, "Tip      : Start with 'help' or 'help <command>'.\n");
	return 0;
}

/* `version` in thông tin nhận diện của app và thời điểm build. */
static int32_t builtin_version(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	new_cmd_line_printf(ctx, "Application : %s\n", ctx->app_name);
	new_cmd_line_printf(ctx, "Version     : %s\n", ctx->version);
	new_cmd_line_printf(ctx, "Build date  : %s %s\n", __DATE__, __TIME__);
	return 0;
}

/* `system` in thông tin môi trường ở thời điểm biên dịch. */
static int32_t builtin_system(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	new_cmd_line_printf(ctx, "Platform : %s\n", platform_name());
	new_cmd_line_printf(ctx, "Compiler : %s\n", compiler_name());
	new_cmd_line_printf(ctx, "Standard : %s\n", c_standard_name());
	new_cmd_line_printf(ctx, "Commands : %lu registered\n", (unsigned long)total_command_count(ctx));
	return 0;
}

/* `status` in trạng thái runtime của shell như uptime và history. */
static int32_t builtin_status(new_cmd_context_t* ctx, int argc, char* argv[]) {
	char start_time[32];
	char uptime[32];

	(void)argc;
	(void)argv;

	format_time_value(ctx->started_at, start_time, sizeof(start_time));
	format_uptime(ctx, uptime, sizeof(uptime));

	new_cmd_line_printf(ctx, "Shell     : %s\n", ctx->app_name);
	new_cmd_line_printf(ctx, "Version   : %s\n", ctx->version);
	new_cmd_line_printf(ctx, "Started   : %s\n", start_time);
	new_cmd_line_printf(ctx, "Uptime    : %s\n", uptime);
	new_cmd_line_printf(ctx, "Prompt    : %s\n", ctx->prompt);
	new_cmd_line_printf(ctx, "Commands  : %lu available\n", (unsigned long)total_command_count(ctx));
	new_cmd_line_printf(ctx, "History   : %lu total, %u stored\n", (unsigned long)ctx->history_total, ctx->history_count);
	return 0;
}

/* `time` in thời gian local hiện tại và uptime của shell. */
static int32_t builtin_time(new_cmd_context_t* ctx, int argc, char* argv[]) {
	char now_string[32];
	char uptime[32];
	time_t now = time(NULL);

	(void)argc;
	(void)argv;

	format_time_value(now, now_string, sizeof(now_string));
	format_uptime(ctx, uptime, sizeof(uptime));

	new_cmd_line_printf(ctx, "Local time : %s\n", now_string);
	new_cmd_line_printf(ctx, "Uptime     : %s\n", uptime);
	return 0;
}

/* `echo` in lại nội dung người dùng vừa truyền vào. */
static int32_t builtin_echo(new_cmd_context_t* ctx, int argc, char* argv[]) {
	char output[NEW_CMD_MAX_INPUT_LEN];

	if (argc == 1) {
		new_cmd_line_printf(ctx, "\n");
		return 0;
	}

	join_arguments(argc, argv, 1, output, sizeof(output));
	new_cmd_line_printf(ctx, "%s\n", output);
	return 0;
}

/* `prompt` hiển thị hoặc cập nhật chuỗi prompt hiện tại. */
static int32_t builtin_prompt(new_cmd_context_t* ctx, int argc, char* argv[]) {
	char new_prompt[NEW_CMD_MAX_PROMPT_LEN];
	size_t length = 0U;

	if (argc == 1) {
		new_cmd_line_printf(ctx, "Current prompt: %s\n", ctx->prompt);
		return 0;
	}

	join_arguments(argc, argv, 1, new_prompt, sizeof(new_prompt));
	length = strlen(new_prompt);
	if ((length > 0U) && (new_prompt[length - 1U] != ' ') && (length < (sizeof(new_prompt) - 1U))) {
		new_prompt[length] = ' ';
		new_prompt[length + 1U] = '\0';
	}

	new_cmd_line_set_prompt(ctx, new_prompt);
	new_cmd_line_printf(ctx, "Prompt updated to: %s\n", ctx->prompt);
	return 0;
}

/* `history` in bộ đệm vòng tròn hoặc xóa nó nếu được yêu cầu. */
static int32_t builtin_history(new_cmd_context_t* ctx, int argc, char* argv[]) {
	uint32_t start_index = 0U;
	uint32_t entry_number = 0U;
	uint32_t offset = 0U;
	uint32_t slot = 0U;

	if ((argc == 2) && command_name_equals(argv[1], "clear")) {
		clear_history(ctx);
		new_cmd_line_printf(ctx, "History cleared.\n");
		return 0;
	}

	if (argc != 1) {
		new_cmd_line_printf(ctx, "Usage: history [clear]\n");
		return -1;
	}

	if (ctx->history_count == 0U) {
		new_cmd_line_printf(ctx, "History is empty.\n");
		return 0;
	}

	start_index = (uint32_t)((ctx->history_head + NEW_CMD_HISTORY_DEPTH - ctx->history_count) % NEW_CMD_HISTORY_DEPTH);
	entry_number = ctx->history_total - ctx->history_count + 1U;

	for (offset = 0U; offset < ctx->history_count; ++offset) {
		slot = (start_index + offset) % NEW_CMD_HISTORY_DEPTH;
		new_cmd_line_printf(ctx, "%lu: %s\n", (unsigned long)(entry_number + offset), ctx->history[slot]);
	}

	return 0;
}

/* `clear` phát mã ANSI để xóa màn hình terminal tương thích. */
static int32_t builtin_clear(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	new_cmd_line_printf(ctx, "\033[2J\033[H");
	return 0;
}

/* `exit` kết thúc phiên tương tác một cách gọn gàng. */
static int32_t builtin_exit(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	new_cmd_line_printf(ctx, "Closing terminal session.\n");
	new_cmd_line_stop(ctx);
	return 0;
}

#ifdef NEW_CMD_LINE_ENABLE_MAIN
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* -------------------------------------------------------------------------- */
/* Phần demo bộ quản lý workspace                                             */
/* -------------------------------------------------------------------------- */

/*
 * Bộ quản lý bên dưới biến shell thành công cụ quản lý workspace:
 * - quét repository để tìm các project dùng Makefile
 * - phân giải tên project do người dùng nhập
 * - chạy các make target trong thư mục project được chọn
 */
#define NEW_CMD_MANAGER_MAX_PROJECTS 64U
#define NEW_CMD_MANAGER_MAX_DEPTH 8U

typedef struct {
	char relative_path[PATH_MAX];
	char short_name[64];
} new_cmd_project_t;

/* Trạng thái runtime toàn cục cho danh sách project và metadata workspace. */
typedef struct {
	char workspace_root[PATH_MAX];
	new_cmd_project_t projects[NEW_CMD_MANAGER_MAX_PROJECTS];
	uint32_t project_count;
	uint32_t refresh_count;
	uint8_t truncated;
} new_cmd_manager_t;

static new_cmd_manager_t g_manager;

static int32_t manager_workspace(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t manager_projects(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t manager_refresh(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t manager_build(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t manager_run(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t manager_clean(new_cmd_context_t* ctx, int argc, char* argv[]);
static int32_t manager_rebuild(new_cmd_context_t* ctx, int argc, char* argv[]);

static const new_cmd_command_t g_demo_commands[] = {
	{"workspace", manager_workspace, "workspace", "Show the active workspace root and discovery summary."},
	{"projects", manager_projects, "projects [refresh]", "List folders that contain a Makefile."},
	{"refresh", manager_refresh, "refresh", "Rescan the workspace for Makefile-based projects."},
	{"build", manager_build, "build [project] <name>", "Run `make` in the selected project folder."},
	{"run", manager_run, "run [project] <name>", "Run `make run` in the selected project folder."},
	{"clean", manager_clean, "clean [project] <name>", "Run `make clean` in the selected project folder."},
	{"rebuild", manager_rebuild, "rebuild [project] <name>", "Run `make clean` followed by `make`."},
	{NULL, NULL, NULL, NULL}
};

typedef struct {
	size_t token_start;
	size_t token_length;
	int token_index;
	char tokens[3][NEW_CMD_MAX_INPUT_LEN];
} new_cmd_completion_state_t;

static struct termios g_saved_terminal_state;
static int g_terminal_raw_active = 0;
static int g_terminal_handlers_installed = 0;

/* Chuẩn hóa cách đại diện project gốc để caller luôn lấy được path hợp lệ. */
static const char* manager_project_id(const new_cmd_project_t* project) {
	if ((project == NULL) || (project->relative_path[0] == '\0')) {
		return ".";
	}

	return project->relative_path;
}

/* Trả về thành phần cuối của path để hiển thị gọn hơn. */
static const char* manager_basename(const char* path) {
	const char* last_separator = NULL;

	if ((path == NULL) || (path[0] == '\0')) {
		return "";
	}

	last_separator = strrchr(path, '/');
	return (last_separator != NULL) ? (last_separator + 1) : path;
}

/* Nối hai thành phần path bằng dấu '/' và vẫn tôn trọng giới hạn buffer. */
static int manager_join_path(const char* base, const char* child, char* output, size_t output_size) {
	int written = 0;

	if ((base == NULL) || (child == NULL) || (output == NULL) || (output_size == 0U)) {
		return -1;
	}

	if ((child[0] == '\0') || ((child[0] == '.') && (child[1] == '\0'))) {
		written = snprintf(output, output_size, "%s", base);
	}
	else if (base[0] == '\0') {
		written = snprintf(output, output_size, "%s", child);
	}
	else {
		written = snprintf(output, output_size, "%s/%s", base, child);
	}

	if ((written < 0) || ((size_t)written >= output_size)) {
		return -1;
	}

	return 0;
}

/* Các hàm hỗ trợ filesystem cho quá trình quét workspace và chạy lệnh. */
static int manager_is_directory(const char* path) {
	struct stat info;

	if ((path == NULL) || (stat(path, &info) != 0)) {
		return 0;
	}

	return S_ISDIR(info.st_mode) ? 1 : 0;
}

static int manager_path_exists(const char* path) {
	struct stat info;
	return ((path != NULL) && (stat(path, &info) == 0)) ? 1 : 0;
}

/* Tìm root của repository bằng cách đi ngược lên đến khi gặp thư mục .git. */
static int manager_find_workspace_root(char* output, size_t output_size) {
	char current[PATH_MAX];
	char marker_path[PATH_MAX];
	char* last_separator = NULL;

	if ((output == NULL) || (output_size == 0U)) {
		return -1;
	}

	if (getcwd(current, sizeof(current)) == NULL) {
		return -1;
	}

	/*
	 * Đi ngược lên từng cấp đến khi gặp .git. Cách này giúp manager có
	 * một workspace root ổn định ngay cả khi được chạy trong thư mục con.
	 */
	for (;;) {
		if (manager_join_path(current, ".git", marker_path, sizeof(marker_path)) == 0) {
			if (manager_is_directory(marker_path) != 0) {
				snprintf(output, output_size, "%s", current);
				return 0;
			}
		}

		if ((strcmp(current, "/") == 0) || (strcmp(current, ".") == 0)) {
			break;
		}

		last_separator = strrchr(current, '/');
		if (last_separator == NULL) {
			break;
		}

		if (last_separator == current) {
			current[1] = '\0';
		}
		else {
			*last_separator = '\0';
		}
	}

	if (getcwd(current, sizeof(current)) == NULL) {
		return -1;
	}

	snprintf(output, output_size, "%s", current);
	return 0;
}

/* Bỏ qua các thư mục không bao giờ nên xem là project của người dùng. */
static int manager_should_skip_directory(const char* name) {
	if ((name == NULL) || (name[0] == '\0')) {
		return 1;
	}

	if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
		return 1;
	}

	if (name[0] == '.') {
		return 1;
	}

	if ((strcmp(name, "out") == 0) || (strcmp(name, "build") == 0)) {
		return 1;
	}

	return 0;
}

/* Xóa danh sách project trong RAM trước mỗi lần quét lại. */
static void manager_reset_projects(void) {
	memset(g_manager.projects, 0, sizeof(g_manager.projects));
	g_manager.project_count = 0U;
	g_manager.truncated = 0U;
}

/* Thêm một project vừa tìm thấy, bỏ qua bản trùng và tránh tràn giới hạn. */
static void manager_add_project(const char* relative_path) {
	uint32_t index = 0U;
	const char* final_path = relative_path;
	const char* short_name = NULL;

	if ((relative_path == NULL) || (relative_path[0] == '\0')) {
		final_path = ".";
	}

	for (index = 0U; index < g_manager.project_count; ++index) {
		if (strcmp(g_manager.projects[index].relative_path, final_path) == 0) {
			return;
		}
	}

	if (g_manager.project_count >= NEW_CMD_MANAGER_MAX_PROJECTS) {
		g_manager.truncated = 1U;
		return;
	}

	snprintf(
		g_manager.projects[g_manager.project_count].relative_path,
		sizeof(g_manager.projects[g_manager.project_count].relative_path),
		"%s",
		final_path
	);

	if (strcmp(final_path, ".") == 0) {
		short_name = manager_basename(g_manager.workspace_root);
	}
	else {
		short_name = manager_basename(final_path);
	}

	snprintf(
		g_manager.projects[g_manager.project_count].short_name,
		sizeof(g_manager.projects[g_manager.project_count].short_name),
		"%s",
		short_name
	);

	g_manager.project_count++;
}

/*
 * Quét đệ quy workspace để tìm các thư mục có Makefile.
 * Giới hạn độ sâu giúp tránh việc đi quá sâu trong cây thư mục quá lớn.
 */
static int manager_discover_recursive(const char* relative_path, uint32_t depth) {
	char absolute_path[PATH_MAX];
	DIR* directory = NULL;
	struct dirent* entry = NULL;
	int found_makefile = 0;

	if (depth > NEW_CMD_MANAGER_MAX_DEPTH) {
		return 0;
	}

	if (manager_join_path(g_manager.workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
		return -1;
	}

	directory = opendir(absolute_path);
	if (directory == NULL) {
		return -1;
	}

	/* Một thư mục được xem là "project" nếu nó chứa Makefile/makefile. */
	while ((entry = readdir(directory)) != NULL) {
		char child_absolute[PATH_MAX];
		struct stat child_info;

		if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0)) {
			continue;
		}

		if (manager_join_path(absolute_path, entry->d_name, child_absolute, sizeof(child_absolute)) != 0) {
			continue;
		}

		if (stat(child_absolute, &child_info) != 0) {
			continue;
		}

		if (S_ISDIR(child_info.st_mode)) {
			char child_relative[PATH_MAX];

			if (manager_should_skip_directory(entry->d_name) != 0) {
				continue;
			}

			if ((relative_path == NULL) || (relative_path[0] == '\0')) {
				snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name);
			}
			else {
				snprintf(child_relative, sizeof(child_relative), "%s/%s", relative_path, entry->d_name);
			}

			(void)manager_discover_recursive(child_relative, depth + 1U);
			continue;
		}

		if (S_ISREG(child_info.st_mode)) {
			if ((strcmp(entry->d_name, "Makefile") == 0) || (strcmp(entry->d_name, "makefile") == 0)) {
				found_makefile = 1;
			}
		}
	}

	(void)closedir(directory);

	if (found_makefile != 0) {
		manager_add_project(relative_path);
	}

	return 0;
}

/* Tạo lại danh sách project được cache từ workspace hiện tại. */
static int manager_refresh_projects(new_cmd_context_t* ctx) {
	manager_reset_projects();
	g_manager.refresh_count++;

	if (g_manager.workspace_root[0] == '\0') {
		if (manager_find_workspace_root(g_manager.workspace_root, sizeof(g_manager.workspace_root)) != 0) {
			if (ctx != NULL) {
				new_cmd_line_printf(ctx, "Error: unable to determine the workspace root.\n");
			}
			return -1;
		}
	}

	(void)manager_discover_recursive("", 0U);

	if ((ctx != NULL) && (g_manager.truncated != 0U)) {
		new_cmd_line_printf(
			ctx,
			"Warning: project list reached the limit of %u entries. Increase NEW_CMD_MANAGER_MAX_PROJECTS if needed.\n",
			NEW_CMD_MANAGER_MAX_PROJECTS
		);
	}

	return 0;
}

/* Khởi động một lần cho lớp workspace-manager. */
static void manager_initialize(new_cmd_context_t* ctx) {
	memset(&g_manager, 0, sizeof(g_manager));
	(void)ctx;

	if (manager_find_workspace_root(g_manager.workspace_root, sizeof(g_manager.workspace_root)) != 0) {
		g_manager.workspace_root[0] = '.';
		g_manager.workspace_root[1] = '\0';
	}

	(void)manager_refresh_projects(NULL);
}

/* Phân giải tên project người dùng nhập: ưu tiên full path, sau đó short name. */
static const new_cmd_project_t* manager_find_project(const char* query, int* ambiguous) {
	const new_cmd_project_t* match = NULL;
	uint32_t index = 0U;

	if (ambiguous != NULL) {
		*ambiguous = 0;
	}

	if ((query == NULL) || (query[0] == '\0')) {
		return NULL;
	}

	for (index = 0U; index < g_manager.project_count; ++index) {
		if (command_name_equals(g_manager.projects[index].relative_path, query) != 0) {
			return &g_manager.projects[index];
		}
	}

	for (index = 0U; index < g_manager.project_count; ++index) {
		if (command_name_equals(g_manager.projects[index].short_name, query) != 0) {
			if (match != NULL) {
				if (ambiguous != NULL) {
					*ambiguous = 1;
				}
				return NULL;
			}

			match = &g_manager.projects[index];
		}
	}

	return match;
}

static void interactive_restore_terminal(void) {
	if (g_terminal_raw_active == 0) {
		return;
	}

	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_terminal_state);
	g_terminal_raw_active = 0;
}

static void interactive_signal_handler(int signal_number) {
	interactive_restore_terminal();
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void interactive_install_handlers(void) {
	if (g_terminal_handlers_installed != 0) {
		return;
	}

	(void)atexit(interactive_restore_terminal);
	(void)signal(SIGINT, interactive_signal_handler);
	(void)signal(SIGTERM, interactive_signal_handler);
	(void)signal(SIGHUP, interactive_signal_handler);
	g_terminal_handlers_installed = 1;
}

static int interactive_enable_raw_mode(void) {
	struct termios raw_mode;

	if (tcgetattr(STDIN_FILENO, &g_saved_terminal_state) != 0) {
		return -1;
	}

	raw_mode = g_saved_terminal_state;
	raw_mode.c_lflag &= (tcflag_t)~(ICANON | ECHO);
	raw_mode.c_cc[VMIN] = 1;
	raw_mode.c_cc[VTIME] = 0;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_mode) != 0) {
		return -1;
	}

	g_terminal_raw_active = 1;
	return 0;
}

static int completion_prefix_matches(const char* candidate, const char* prefix) {
	if ((candidate == NULL) || (prefix == NULL)) {
		return 0;
	}

	while (*prefix != '\0') {
		if (*candidate == '\0') {
			return 0;
		}

		if (tolower((unsigned char)*candidate) != tolower((unsigned char)*prefix)) {
			return 0;
		}

		candidate++;
		prefix++;
	}

	return 1;
}

static size_t completion_common_prefix_length(const char* const matches[], size_t count) {
	size_t common_length = 0U;
	size_t index = 0U;
	char current = '\0';

	if ((matches == NULL) || (count == 0U) || (matches[0] == NULL)) {
		return 0U;
	}

	for (;;) {
		current = matches[0][common_length];
		if (current == '\0') {
			return common_length;
		}

		for (index = 1U; index < count; ++index) {
			if ((matches[index] == NULL) ||
				(tolower((unsigned char)matches[index][common_length]) != tolower((unsigned char)current))) {
				return common_length;
			}
		}

		common_length++;
	}
}

static void completion_parse_line(const char* line, size_t line_length, new_cmd_completion_state_t* state) {
	size_t index = 0U;
	int token_index = 0;

	memset(state, 0, sizeof(*state));
	state->token_start = line_length;
	state->token_length = 0U;
	state->token_index = 0;

	while (index < line_length) {
		size_t start = 0U;
		size_t length = 0U;

		while ((index < line_length) && isspace((unsigned char)line[index])) {
			index++;
		}

		if (index >= line_length) {
			break;
		}

		start = index;
		while ((index < line_length) && !isspace((unsigned char)line[index])) {
			index++;
		}

		length = index - start;
		if (token_index < (int)(sizeof(state->tokens) / sizeof(state->tokens[0]))) {
			size_t copy_length = length;

			if (copy_length >= sizeof(state->tokens[token_index])) {
				copy_length = sizeof(state->tokens[token_index]) - 1U;
			}

			memcpy(state->tokens[token_index], line + start, copy_length);
			state->tokens[token_index][copy_length] = '\0';
		}

		state->token_start = start;
		state->token_length = length;
		state->token_index = token_index;
		token_index++;
	}

	if ((line_length == 0U) || isspace((unsigned char)line[line_length - 1U])) {
		state->token_start = line_length;
		state->token_length = 0U;
		state->token_index = token_index;
	}
}

static int completion_add_candidate(
	const char* candidate,
	const char* prefix,
	const char* matches[],
	size_t* match_count,
	size_t max_matches
) {
	size_t index = 0U;

	if ((candidate == NULL) || (prefix == NULL) || (matches == NULL) || (match_count == NULL)) {
		return -1;
	}

	if (completion_prefix_matches(candidate, prefix) == 0) {
		return 0;
	}

	for (index = 0U; index < *match_count; ++index) {
		if (strcmp(matches[index], candidate) == 0) {
			return 0;
		}
	}

	if (*match_count >= max_matches) {
		return -1;
	}

	matches[*match_count] = candidate;
	(*match_count)++;
	return 0;
}

static void completion_collect_command_matches(
	const new_cmd_context_t* ctx,
	const char* prefix,
	const char* matches[],
	size_t* match_count,
	size_t max_matches
) {
	uint32_t index = 0U;
	const new_cmd_command_t* table = NULL;

	table = (ctx != NULL) ? ctx->external_commands : NULL;
	while ((table != NULL) && (table[index].name != NULL)) {
		(void)completion_add_candidate(table[index].name, prefix, matches, match_count, max_matches);
		index++;
	}

	index = 0U;
	while (g_builtin_commands[index].name != NULL) {
		(void)completion_add_candidate(g_builtin_commands[index].name, prefix, matches, match_count, max_matches);
		index++;
	}
}

static int completion_is_project_command(const char* command_name) {
	return (command_name_equals(command_name, "build") != 0) ||
		(command_name_equals(command_name, "run") != 0) ||
		(command_name_equals(command_name, "clean") != 0) ||
		(command_name_equals(command_name, "rebuild") != 0);
}

static void completion_collect_project_matches(
	const char* prefix,
	const char* matches[],
	size_t* match_count,
	size_t max_matches
) {
	uint32_t index = 0U;

	(void)manager_refresh_projects(NULL);

	for (index = 0U; index < g_manager.project_count; ++index) {
		(void)completion_add_candidate(g_manager.projects[index].short_name, prefix, matches, match_count, max_matches);
		(void)completion_add_candidate(
			g_manager.projects[index].relative_path,
			prefix,
			matches,
			match_count,
			max_matches
		);
	}
}

static size_t completion_collect_matches(
	new_cmd_context_t* ctx,
	const char* line,
	size_t line_length,
	const char* matches[],
	size_t max_matches,
	size_t* replace_start_out,
	size_t* prefix_length_out
) {
	new_cmd_completion_state_t state;
	char prefix[NEW_CMD_MAX_INPUT_LEN];
	const char* prefix_start = NULL;
	const char* first_token = NULL;
	size_t prefix_length = 0U;
	size_t replace_start = 0U;
	size_t star_offset = 0U;
	size_t match_count = 0U;

	completion_parse_line(line, line_length, &state);

	prefix_start = line + state.token_start;
	prefix_length = state.token_length;

	if ((state.token_index == 0) && (prefix_length > 0U) && (prefix_start[0] == '*')) {
		star_offset = 1U;
		prefix_start++;
		prefix_length--;
	}

	if (prefix_length >= sizeof(prefix)) {
		prefix_length = sizeof(prefix) - 1U;
	}

	memcpy(prefix, prefix_start, prefix_length);
	prefix[prefix_length] = '\0';

	replace_start = state.token_start + star_offset;

	if (state.token_index == 0) {
		completion_collect_command_matches(ctx, prefix, matches, &match_count, max_matches);
	}
	else {
		first_token = state.tokens[0];
		if ((first_token != NULL) && (first_token[0] == '*')) {
			first_token++;
		}

		if ((first_token != NULL) && (command_name_equals(first_token, "help") != 0) && (state.token_index == 1)) {
			completion_collect_command_matches(ctx, prefix, matches, &match_count, max_matches);
		}
		else if ((first_token != NULL) && (completion_is_project_command(first_token) != 0)) {
			if (state.token_index == 1) {
				(void)completion_add_candidate("project", prefix, matches, &match_count, max_matches);
				completion_collect_project_matches(prefix, matches, &match_count, max_matches);
			}
			else if ((state.token_index == 2) && (command_name_equals(state.tokens[1], "project") != 0)) {
				completion_collect_project_matches(prefix, matches, &match_count, max_matches);
			}
		}
	}

	*replace_start_out = replace_start;
	*prefix_length_out = prefix_length;
	return match_count;
}

static void completion_print_matches(new_cmd_context_t* ctx, const char* current_line, const char* const matches[], size_t count) {
	size_t index = 0U;

	fputc('\n', stdout);
	for (index = 0U; index < count; ++index) {
		fprintf(stdout, "%s\n", matches[index]);
	}

	fprintf(stdout, "%s%s", ctx->prompt, current_line);
	fflush(stdout);
}

static int completion_apply_candidate(
	char* buffer,
	size_t* length,
	size_t buffer_size,
	size_t replace_start,
	size_t typed_length,
	const char* candidate,
	int append_space
) {
	size_t candidate_length = 0U;
	size_t suffix_length = 0U;

	if ((buffer == NULL) || (length == NULL) || (candidate == NULL)) {
		return -1;
	}

	candidate_length = strlen(candidate);
	if (candidate_length < typed_length) {
		return -1;
	}

	if (*length != (replace_start + typed_length)) {
		return -1;
	}

	suffix_length = candidate_length - typed_length;
	if ((*length + suffix_length + ((append_space != 0) ? 1U : 0U)) >= buffer_size) {
		fputc('\a', stdout);
		fflush(stdout);
		return -1;
	}

	memcpy(buffer + *length, candidate + typed_length, suffix_length);
	*length += suffix_length;
	buffer[*length] = '\0';

	if (suffix_length > 0U) {
		fputs(candidate + typed_length, stdout);
	}

	if ((append_space != 0) && ((*length == 0U) || (buffer[*length - 1U] != ' '))) {
		buffer[*length] = ' ';
		(*length)++;
		buffer[*length] = '\0';
		fputc(' ', stdout);
	}

	fflush(stdout);
	return 0;
}

/* Parser dùng chung cho nhóm lệnh có dạng `build [project] <name>`. */
static const char* manager_parse_project_argument(new_cmd_context_t* ctx, int argc, char* argv[], const char* usage) {
	int index = 1;

	/* Chấp nhận cả "build demo" lẫn "build project demo" để CLI thân thiện hơn. */
	if ((argc > 1) && command_name_equals(argv[1], "project") != 0) {
		index = 2;
	}

	if (argc != (index + 1)) {
		new_cmd_line_printf(ctx, "Usage: %s\n", usage);
		return NULL;
	}

	return argv[index];
}

/* Phân giải tên project vừa nhập dựa trên danh sách project mới nhất. */
static int manager_resolve_project(
	new_cmd_context_t* ctx,
	const char* query,
	const new_cmd_project_t** project_out
) {
	const new_cmd_project_t* project = NULL;
	int ambiguous = 0;

	if (manager_refresh_projects(NULL) != 0) {
		new_cmd_line_printf(ctx, "Error: unable to refresh project list.\n");
		return -1;
	}

	project = manager_find_project(query, &ambiguous);
	if (project == NULL) {
		if (ambiguous != 0) {
			new_cmd_line_printf(ctx, "Error: project name '%s' is ambiguous. Use the full relative path.\n", query);
			return -1;
		}

		new_cmd_line_printf(ctx, "Error: project '%s' was not found. Use 'projects' to list valid names.\n", query);
		return -1;
	}

	*project_out = project;
	return 0;
}

/*
 * Chạy một make target trong project được chọn.
 * Output được stream trực tiếp ra terminal để manager đóng vai trò
 * điều phối nhẹ, thay vì trở thành bộ thu thập build log.
 */
static int manager_execute_make(new_cmd_context_t* ctx, const new_cmd_project_t* project, const char* target) {
	char project_path[PATH_MAX];
	pid_t child = 0;
	int child_status = 0;

	if ((ctx == NULL) || (project == NULL)) {
		return -1;
	}

	if (manager_join_path(g_manager.workspace_root, manager_project_id(project), project_path, sizeof(project_path)) != 0) {
		new_cmd_line_printf(ctx, "Error: failed to resolve project path for %s.\n", manager_project_id(project));
		return -1;
	}

	if (manager_path_exists(project_path) == 0) {
		new_cmd_line_printf(ctx, "Error: project path does not exist: %s\n", project_path);
		return -1;
	}

	new_cmd_line_printf(
		ctx,
		"\n==> Running: make -C %s%s%s\n",
		manager_project_id(project),
		(target != NULL) ? " " : "",
		(target != NULL) ? target : ""
	);

	fflush(stdout);
	fflush(stderr);

	/*
	 * fork + execvp giúp output thật của make chạy thẳng ra terminal,
	 * thay vì bị gom vào một buffer trong shell này.
	 */
	child = fork();
	if (child < 0) {
		new_cmd_line_printf(ctx, "Error: fork failed: %s\n", strerror(errno));
		return -1;
	}

	if (child == 0) {
		if (target != NULL) {
			char* const args[] = {"make", "-C", project_path, (char*)target, NULL};
			execvp("make", args);
		}
		else {
			char* const args[] = {"make", "-C", project_path, NULL};
			execvp("make", args);
		}

		fprintf(stderr, "execvp failed: %s\n", strerror(errno));
		_exit(127);
	}

	for (;;) {
		if (waitpid(child, &child_status, 0) >= 0) {
			break;
		}

		if (errno != EINTR) {
			new_cmd_line_printf(ctx, "Error: waitpid failed: %s\n", strerror(errno));
			return -1;
		}
	}

	if (WIFEXITED(child_status) != 0) {
		int exit_code = WEXITSTATUS(child_status);

		if (exit_code == 0) {
			new_cmd_line_printf(ctx, "==> Completed successfully: %s\n", manager_project_id(project));
			return 0;
		}

		new_cmd_line_printf(ctx, "==> Make failed with exit code %d for %s\n", exit_code, manager_project_id(project));
		return -1;
	}

	if (WIFSIGNALED(child_status) != 0) {
		new_cmd_line_printf(
			ctx,
			"==> Make terminated by signal %d for %s\n",
			WTERMSIG(child_status),
			manager_project_id(project)
		);
		return -1;
	}

	new_cmd_line_printf(ctx, "==> Make finished unexpectedly for %s\n", manager_project_id(project));
	return -1;
}

/* In bảng project đã tìm thấy theo định dạng gọn và dễ đọc. */
static void manager_print_projects(new_cmd_context_t* ctx) {
	uint32_t index = 0U;

	if (g_manager.project_count == 0U) {
		new_cmd_line_printf(ctx, "No Makefile-based projects were found under the workspace.\n");
		return;
	}

	new_cmd_line_printf(ctx, "Discovered projects\n");
	for (index = 0U; index < g_manager.project_count; ++index) {
		new_cmd_line_printf(
			ctx,
			"  %-18s %s\n",
			g_manager.projects[index].short_name,
			manager_project_id(&g_manager.projects[index])
		);
	}
}

/* `workspace` in root hiện tại của repository và tóm tắt kết quả quét. */
static int32_t manager_workspace(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	if (manager_refresh_projects(NULL) != 0) {
		new_cmd_line_printf(ctx, "Error: workspace scan failed.\n");
		return -1;
	}

	new_cmd_line_printf(ctx, "Workspace root : %s\n", g_manager.workspace_root);
	new_cmd_line_printf(ctx, "Projects       : %lu discovered\n", (unsigned long)g_manager.project_count);
	new_cmd_line_printf(ctx, "Refresh count  : %lu\n", (unsigned long)g_manager.refresh_count);
	new_cmd_line_printf(ctx, "Tip            : Use 'projects' to list them.\n");
	return 0;
}

/* `projects` liệt kê các thư mục project đang được tìm thấy. */
static int32_t manager_projects(new_cmd_context_t* ctx, int argc, char* argv[]) {
	if ((argc == 2) && (command_name_equals(argv[1], "refresh") == 0)) {
		new_cmd_line_printf(ctx, "Usage: projects [refresh]\n");
		return -1;
	}

	if (argc > 2) {
		new_cmd_line_printf(ctx, "Usage: projects [refresh]\n");
		return -1;
	}

	if (manager_refresh_projects(ctx) != 0) {
		return -1;
	}

	manager_print_projects(ctx);
	return 0;
}

/* `refresh` bắt buộc quét lại workspace. */
static int32_t manager_refresh(new_cmd_context_t* ctx, int argc, char* argv[]) {
	(void)argc;
	(void)argv;

	if (manager_refresh_projects(ctx) != 0) {
		return -1;
	}

	new_cmd_line_printf(ctx, "Workspace refreshed. %lu project(s) available.\n", (unsigned long)g_manager.project_count);
	return 0;
}

/* `build` chạy make target mặc định cho project được chọn. */
static int32_t manager_build(new_cmd_context_t* ctx, int argc, char* argv[]) {
	const char* query = NULL;
	const new_cmd_project_t* project = NULL;

	query = manager_parse_project_argument(ctx, argc, argv, "build [project] <name>");
	if (query == NULL) {
		return -1;
	}

	if (manager_resolve_project(ctx, query, &project) != 0) {
		return -1;
	}

	return manager_execute_make(ctx, project, NULL);
}

/* `run` thực thi target `make run` của project. */
static int32_t manager_run(new_cmd_context_t* ctx, int argc, char* argv[]) {
	const char* query = NULL;
	const new_cmd_project_t* project = NULL;

	query = manager_parse_project_argument(ctx, argc, argv, "run [project] <name>");
	if (query == NULL) {
		return -1;
	}

	if (manager_resolve_project(ctx, query, &project) != 0) {
		return -1;
	}

	return manager_execute_make(ctx, project, "run");
}

/* `clean` thực thi target `make clean` của project. */
static int32_t manager_clean(new_cmd_context_t* ctx, int argc, char* argv[]) {
	const char* query = NULL;
	const new_cmd_project_t* project = NULL;

	query = manager_parse_project_argument(ctx, argc, argv, "clean [project] <name>");
	if (query == NULL) {
		return -1;
	}

	if (manager_resolve_project(ctx, query, &project) != 0) {
		return -1;
	}

	return manager_execute_make(ctx, project, "clean");
}

/* `rebuild` là wrapper tiện lợi cho clean rồi đến build. */
static int32_t manager_rebuild(new_cmd_context_t* ctx, int argc, char* argv[]) {
	const char* query = NULL;
	const new_cmd_project_t* project = NULL;

	query = manager_parse_project_argument(ctx, argc, argv, "rebuild [project] <name>");
	if (query == NULL) {
		return -1;
	}

	if (manager_resolve_project(ctx, query, &project) != 0) {
		return -1;
	}

	if (manager_execute_make(ctx, project, "clean") != 0) {
		return -1;
	}

	return manager_execute_make(ctx, project, NULL);
}

/*
 * Bộ đọc dòng tương tác có hỗ trợ tab completion nhẹ.
 * Cách này giữ shell không phụ thuộc thêm thư viện ngoài, nhưng vẫn hỗ trợ:
 * - hoàn thành lệnh ở token đầu tiên
 * - hoàn thành tên lệnh sau `help`
 * - hoàn thành tên project sau build/run/clean/rebuild
 */
static int read_line_with_completion(new_cmd_context_t* ctx, char* input, size_t input_size) {
	const char* matches[(NEW_CMD_MANAGER_MAX_PROJECTS * 2U) + 32U];
	size_t input_length = 0U;
	int ch = 0;

	if ((ctx == NULL) || (input == NULL) || (input_size == 0U)) {
		return -1;
	}

	input[0] = '\0';

	interactive_install_handlers();
	if (interactive_enable_raw_mode() != 0) {
		return -1;
	}

	fprintf(stdout, "%s", ctx->prompt);
	fflush(stdout);

	for (;;) {
		ch = getchar();
		if (ch == EOF) {
			interactive_restore_terminal();
			return -1;
		}

		if ((ch == '\r') || (ch == '\n')) {
			fputc('\n', stdout);
			fflush(stdout);
			break;
		}

		if (ch == 4) {
			if (input_length == 0U) {
				interactive_restore_terminal();
				return -1;
			}

			continue;
		}

		if ((ch == 127) || (ch == 8)) {
			if (input_length > 0U) {
				input_length--;
				input[input_length] = '\0';
				fputs("\b \b", stdout);
				fflush(stdout);
			}

			continue;
		}

		if (ch == '\t') {
			size_t replace_start = 0U;
			size_t prefix_length = 0U;
			size_t match_count = 0U;
			size_t common_length = 0U;

			match_count = completion_collect_matches(
				ctx,
				input,
				input_length,
				matches,
				sizeof(matches) / sizeof(matches[0]),
				&replace_start,
				&prefix_length
			);

			if (match_count == 0U) {
				fputc('\a', stdout);
				fflush(stdout);
				continue;
			}

			if (match_count == 1U) {
				(void)completion_apply_candidate(
					input,
					&input_length,
					input_size,
					replace_start,
					prefix_length,
					matches[0],
					1
				);
				continue;
			}

			common_length = completion_common_prefix_length(matches, match_count);
			if (common_length > prefix_length) {
				char common_prefix[NEW_CMD_MAX_INPUT_LEN];

				if (common_length >= sizeof(common_prefix)) {
					common_length = sizeof(common_prefix) - 1U;
				}

				memcpy(common_prefix, matches[0], common_length);
				common_prefix[common_length] = '\0';
				(void)completion_apply_candidate(
					input,
					&input_length,
					input_size,
					replace_start,
					prefix_length,
					common_prefix,
					0
				);
			}
			else {
				completion_print_matches(ctx, input, matches, match_count);
			}

			continue;
		}

		if (isprint((unsigned char)ch) == 0) {
			continue;
		}

		if ((input_length + 1U) >= input_size) {
			fputc('\a', stdout);
			fflush(stdout);
			continue;
		}

		input[input_length++] = (char)ch;
		input[input_length] = '\0';
		fputc(ch, stdout);
		fflush(stdout);
	}

	interactive_restore_terminal();
	return 0;
}

/* Điểm vào standalone cho host-terminal khi bật NEW_CMD_LINE_ENABLE_MAIN. */
int main(void) {
	new_cmd_context_t ctx;

	/* Chuỗi khởi động cho demo workspace manager chạy trên máy host. */
	new_cmd_line_init(&ctx, "Personal CMD Manager", "v.0.0.1");
	manager_initialize(&ctx);
	new_cmd_line_attach_commands(&ctx, g_demo_commands);
	new_cmd_line_set_prompt(&ctx, "workspace> ");
	new_cmd_line_run(&ctx);
	return 0;
}
#endif
