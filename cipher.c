#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_SHELL_NAME_LEN 256      // Not including the null terminator (e.g., "1234567" is valid).
#define MAX_DIRECTORY_LEN 4096      // Not including the null terminator.
#define MAX_TOKEN_SIZE 4096
#define MAX_TOKEN_COUNT 4096
#define MAX_INPUT_SIZE 10000
#define MAX_PID_NAME_LEN 256
#define READ_WRITE_BUFFER_SIZE 4096
#define MAX_MARK_COUNT 128
#define MAX_MARK_NAME_LEN 64
#define MARKS_FILE_NAME ".cipher_marks"

char* prompt_value = NULL;
int token_count = 0;
int optional_token_count = 0;
char** token_values = NULL;
char* input_redirect = NULL;
char* output_redirect = NULL;
int background_active = 0;
int pipeline_stage_active = 0;
pid_t background_pids[MAX_TOKEN_COUNT];
int background_statuses[MAX_TOKEN_COUNT];
int background_finished[MAX_TOKEN_COUNT];
int background_count = 0;

int debug_level = 0;
int exit_status = 0;
int exit_active = 0;

char* working_directory = NULL;
char* procfs = NULL;

/* ------------------------------------------------------[ Token initialization ]------------------------------------------------------ */

// Initializes the table of strings that stores tokenized strings.
void init_tokens()
{
    token_values = (char**)malloc(sizeof(char*) * MAX_TOKEN_COUNT);
    for (int ix = 0; ix < MAX_TOKEN_COUNT; ix++) token_values[ix] = (char*)malloc(sizeof(char) * MAX_TOKEN_SIZE);
}

// Deinitializes the table of strings that stores tokenized strings.
void deinit_tokens()
{
    for (int ix = 0; ix < MAX_TOKEN_COUNT; ix++) free(token_values[ix]);
    free(token_values);
}

/* ---------------------------------------------------------[ Status helpers ]--------------------------------------------------------- */

void set_success()
{
    exit_status = 0;
    fflush(stdout);
}

void set_errno_status(const char* command)
{
    int err = errno;
    perror(command);
    exit_status = err;
}

void set_missing_args_status(const char* command, const char* message)
{
    fprintf(stderr, "%s: %s\n", command, message);
    exit_status = EINVAL;
}

int get_effective_token_count()
{
    return token_count - optional_token_count;
}

int check_args(int required_count, const char* command, const char* message)
{
    if (get_effective_token_count() >= required_count) return 1;
    set_missing_args_status(command, message);
    return 0;
}

void print_integer_result(int value)
{
    printf("%d\n", value);
    set_success();
}

/* ----------------------------------------------------------[ Help command ]---------------------------------------------------------- */

void print_help()
{
    printf("Commands:\n");

    printf(" Basic:\n");
    printf("    debug [LEVEL]     Set debug level (0=off, higher=more verbose)\n");
    printf("    prompt [POZIV]    Set the shell prompt\n");
    printf("    status            Show last command exit status\n");
    printf("    exit [STATUS]     Exit the shell\n");
    printf("    help              Show this help message\n");
    printf("\n");

    printf(" Output:\n");
    printf("    print ARGS...     Print arguments without newline\n");
    printf("    echo ARGS...      Print arguments with newline\n");
    printf("    len ARGS...       Print total length of all arguments\n");
    printf("    sum NUMS...       Print sum of all numeric arguments\n");
    printf("    calc NUM OP NUM   Calculate: +, -, *, /, %%\n");
    printf("\n");

    printf(" Paths:\n");
    printf("    basename PATH     Print final component of path\n");
    printf("    dirname PATH      Print directory component of path\n");
    printf("\n");

    printf(" Directories:\n");
    printf("    dirch [DIR]       Change working directory (default: /)\n");
    printf("    dirwd [MODE]      Print working directory (base/full)\n");
    printf("    dirmk DIR         Create directory\n");
    printf("    dirrm DIR         Remove directory\n");
    printf("    dirls [DIR]       List directory contents\n");
    printf("\n");

    printf(" Files:\n");
    printf("    rename OLD NEW    Rename file or directory\n");
    printf("    unlink FILE       Remove file\n");
    printf("    remove FILE       Remove file or directory\n");
    printf("    cpcat FILE        Display file contents\n");
    printf("\n");

    printf(" Links:\n");
    printf("    linkhard SRC DST  Create hard link\n");
    printf("    linksoft SRC DST  Create symbolic link\n");
    printf("    linkread LINK     Read symbolic link target\n");
    printf("    linklist FILE     List hard links to file\n");
    printf("\n");

    printf(" Process:\n");
    printf("    pid               Print current process ID\n");
    printf("    ppid              Print parent process ID\n");
    printf("    uid               Print real user ID\n");
    printf("    euid              Print effective user ID\n");
    printf("    gid               Print real group ID\n");
    printf("    egid              Print effective group ID\n");
    printf("    sysinfo           Print system information\n");
    printf("\n");

    printf(" Procfs:\n");
    printf("    proc [DIR]        Show or set procfs directory\n");
    printf("    pids              List process IDs\n");
    printf("    pinfo             List process information\n");
    printf("\n");

    printf(" Background:\n");
    printf("    waitone [PID]     Wait for one background process\n");
    printf("    waitall           Wait for all background processes\n");
    printf("\n");

    printf(" Pipelines:\n");
    printf("    pipes STAGES...   Execute quoted command stages as a pipeline\n");
    printf("\n");

    fflush(stdout);
}

/* -----------------------------------------------------[ Debug handler commands ]----------------------------------------------------- */


void print_builtin_foreground()
{
    assert(token_values != NULL);
    printf("Executing builtin '%s' in foreground\n", token_values[0]);
    fflush(stdout);
}

void print_builtin_background()
{
    assert(token_values != NULL);
    printf("Executing builtin '%s' in background\n", token_values[0]);
    fflush(stdout);
}

void print_builtin_execution()
{
    if (background_active) print_builtin_background();
    else print_builtin_foreground();
    fflush(stdout);
}

void print_external()
{
    printf("External command '");
    int effective_count = get_effective_token_count();
    for (int ix = 0; ix < effective_count; ix++) printf("%s%s", token_values[ix], ix < effective_count - 1 ? " " : "");
    printf("'\n");
    fflush(stdout);
}

/* -----------------------------------------------------[ Token handler commands ]----------------------------------------------------- */


// Tokenizes the string, pointed to by `str`.
void set_tokens(const char* str)
{
    char buffer[MAX_TOKEN_SIZE];
    int token_ix = 0;
    int buffer_ix = 0;
    int str_ix = 0;
    int str_len = strlen(str);
    while (str_ix < str_len)
    {
        // Trim leading whitespaces
        while (str_ix < str_len && isspace(str[str_ix])) str_ix++;
        if (str_ix >= str_len) break;

        buffer_ix = 0;
        // Handle quoted strings
        if (str[str_ix] == '"')
        {
            str_ix++;
            while (str_ix < str_len && str[str_ix] != '"') buffer[buffer_ix++] = str[str_ix++];
            if (str_ix < str_len && str[str_ix] == '"') str_ix++;
            buffer[buffer_ix] = '\0';
        }
        // Handle comments
        else if (str[str_ix] == '#') break;
        // Handle regular tokens
        else
        {
            while (str_ix < str_len && !isspace(str[str_ix])) buffer[buffer_ix++] = str[str_ix++];
            buffer[buffer_ix] = '\0';
        }

        if (buffer_ix > 0)
        {
            if (debug_level > 0) printf("Token %d: '%s'\n", token_ix++, buffer);
            assert(token_values[token_count] != NULL);
            memcpy(token_values[token_count++], buffer, strlen(buffer) + 1);
        }
    }
    fflush(stdout);
}

// Sets the values for `input_redirect`, `output_redirect` and `background`, if these arguments are present and correctly formatted in the command.
void set_optional_tokens()
{
    for (int ix = token_count - 1; ix >= 0; ix--)
    {
        if (token_values[ix] == NULL) continue;
        switch (token_values[ix][0])
        {
        case '<':
            input_redirect = token_values[ix] + 1;
            optional_token_count++;
            break;
        case '>':
            output_redirect = token_values[ix] + 1;
            optional_token_count++;
            break;
        case '&':
            background_active = 1;
            optional_token_count++;
            break;
        default:
            break;
        }
    }

    if (debug_level > 0)
    {
        if (input_redirect != NULL) printf("Input redirect: '%s'\n", input_redirect);
        if (output_redirect != NULL) printf("Output redirect: '%s'\n", output_redirect);
        if (background_active) printf("Background: 1\n");
    }
    fflush(stdout);
}

/* --------------------------------------------------------[ Redirection helpers ]------------------------------------------------------- */

typedef struct
{
    int input_fd;
    int output_fd;
} RedirectState;

void init_redirect_state(RedirectState* state)
{
    state->input_fd = -1;
    state->output_fd = -1;
}

int redirect_input(const char* command)
{
    if (input_redirect == NULL) return 1;

    int in = open(input_redirect, O_RDONLY);
    if (in < 0)
    {
        set_errno_status(command);
        return 0;
    }

    if (dup2(in, STDIN_FILENO) < 0)
    {
        set_errno_status(command);
        close(in);
        return 0;
    }

    close(in);
    return 1;
}

int redirect_output(const char* command)
{
    if (output_redirect == NULL) return 1;

    fflush(stdout);

    int out = open(output_redirect, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
    {
        set_errno_status(command);
        return 0;
    }

    if (dup2(out, STDOUT_FILENO) < 0)
    {
        set_errno_status(command);
        close(out);
        return 0;
    }

    close(out);
    return 1;
}

int apply_redirects(const char* command)
{
    if (!redirect_input(command)) return 0;
    if (!redirect_output(command)) return 0;
    return 1;
}

int save_redirects(const char* command, RedirectState* state)
{
    init_redirect_state(state);

    if (input_redirect != NULL)
    {
        state->input_fd = dup(STDIN_FILENO);
        if (state->input_fd < 0)
        {
            set_errno_status(command);
            return 0;
        }
    }

    if (output_redirect != NULL)
    {
        fflush(stdout);
        state->output_fd = dup(STDOUT_FILENO);
        if (state->output_fd < 0)
        {
            set_errno_status(command);
            if (state->input_fd >= 0) close(state->input_fd);
            init_redirect_state(state);
            return 0;
        }
    }

    return 1;
}

void restore_redirects(RedirectState* state)
{
    if (state->output_fd >= 0)
    {
        fflush(stdout);
        dup2(state->output_fd, STDOUT_FILENO);
        close(state->output_fd);
        state->output_fd = -1;
    }

    if (state->input_fd >= 0)
    {
        dup2(state->input_fd, STDIN_FILENO);
        close(state->input_fd);
        state->input_fd = -1;
    }
}

int apply_foreground_redirects(const char* command, RedirectState* state)
{
    if (!save_redirects(command, state)) return 0;
    if (!apply_redirects(command))
    {
        restore_redirects(state);
        return 0;
    }
    return 1;
}

/* -----------------------------------------------[ Built-in function command handlers ]----------------------------------------------- */


void handle_debug_command()
{
    int effective_count = get_effective_token_count();
    if (effective_count == 1) printf("%d\n", debug_level);
    else debug_level = atoi(token_values[1]);
    set_success();
}

void handle_prompt_command()
{
    int effective_count = get_effective_token_count();
    exit_status = (effective_count > 1 && strlen(token_values[1]) > MAX_SHELL_NAME_LEN) ? 1 : 0;
    if (exit_status == 0 && effective_count > 1) strcpy(prompt_value, token_values[1]);
    if (effective_count == 1) printf("%s\n", prompt_value);
    fflush(stdout);
}

void handle_status_command()
{
    printf("%d\n", exit_status);
    fflush(stdout);
}

void handle_exit_command()
{
    if (get_effective_token_count() > 1) exit_status = atoi(token_values[1]);
    exit_active = 1;
    fflush(stdout);
}

void handle_help_command()
{
    print_help();
    set_success();
}

void handle_print_command()
{
    int effective_count = get_effective_token_count();
    for (int ix = 1; ix < effective_count; ix++) printf("%s%s", token_values[ix], ix < effective_count - 1 ? " " : "");
    set_success();
}

void handle_echo_command()
{
    handle_print_command();
    printf("\n");
    set_success();
}

void handle_len_command()
{
    int len = 0;
    int effective_count = get_effective_token_count();
    for (int ix = 1; ix < effective_count; ix++) len += strlen(token_values[ix]);
    print_integer_result(len);
}

void handle_sum_command()
{
    int sum = 0;
    int effective_count = get_effective_token_count();
    for (int ix = 1; ix < effective_count; ix++) sum += atoi(token_values[ix]);
    print_integer_result(sum);
}

int operation_add(int op1, int op2) { return op1 + op2; }
int operation_subtract(int op1, int op2) { return op1 - op2; }
int operation_multiply(int op1, int op2) { return op1 * op2; }
int operation_divide(int op1, int op2) { return op1 / op2; }
int operation_modulus(int op1, int op2) { return op1 % op2; }

typedef struct
{
    const char* name;
    int (*handler)(int, int);
} Operation;

Operation builtin_operations[] = {
    {"+", operation_add},
    {"-", operation_subtract},
    {"*", operation_multiply},
    {"/", operation_divide},
    {"%", operation_modulus},
    {NULL, NULL}
};

void handle_calc_command()
{
    if (!check_args(4, "calc", "Insufficient amount of parameters")) return;

    char* operator = token_values[2];
    int operand1 = atoi(token_values[1]);
    int operand2 = atoi(token_values[3]);
    int result;
    int found = 0;
    for (int operator_ix = 0; builtin_operations[operator_ix].name != NULL; operator_ix++)
    {
        if (!strcmp(operator, builtin_operations[operator_ix].name))
        {
            result = builtin_operations[operator_ix].handler(operand1, operand2);
            printf("%d\n", result);
            exit_status = 0;
            found = 1;
            break;
        }
    }
    if (!found)
    {
        fprintf(stderr, "calc: Unknown operator '%s'\n", operator);
        exit_status = EINVAL;
        return;
    }
    set_success();
}

void handle_basename_command()
{
    if (get_effective_token_count() < 2)
    {
        exit_status = 1;
        return;
    }

    const char* directory = token_values[1];
    const char* basename = strrchr(directory, '/');

    if (basename == NULL) printf("%s\n", directory);
    else printf("%s\n", basename + 1);
    set_success();
}

void handle_dirname_command()
{
    if (get_effective_token_count() < 2)
    {
        exit_status = 1;
        return;
    }

    const char* directory = token_values[1];
    const char* last_slash = strrchr(directory, '/');

    if (last_slash == NULL) { printf(".\n"); }
    else if (last_slash == directory) { printf("/\n"); }
    else { printf("%.*s\n", (int)(last_slash - directory), directory); }
    set_success();
}

void handle_dirch_command()
{
    const char* directory = get_effective_token_count() < 2 ? "/" : token_values[1];

    if (chdir(directory) != 0)
    {
        set_errno_status("dirch");
        return;
    }

    if (getcwd(working_directory, MAX_DIRECTORY_LEN + 1) == NULL)
    {
        set_errno_status("dirch");
        return;
    }

    set_success();
}

void handle_dirwd_command()
{
    const char* mode = get_effective_token_count() > 1 ? token_values[1] : "base";
    if (strcmp(mode, "base") == 0)
    {
        if (strcmp(working_directory, "/") == 0) printf("/\n");
        else
        {
            const char* basename = strrchr(working_directory, '/');
            printf("%s\n", basename ? basename + 1 : working_directory);
        }
    }
    else if (strcmp(mode, "full") == 0) printf("%s\n", working_directory);
    else
    {
        fprintf(stderr, "dirwd: Unknown mode '%s'\n", mode);
        exit_status = EINVAL;
        return;
    }
    set_success();
}

void handle_dirmk_command()
{
    if (!check_args(2, "dirmk", "Missing directory name")) return;

    if (mkdir(token_values[1], S_IRWXU | S_IRWXG | S_IRWXO) != 0)
    {
        set_errno_status("dirmk");
        return;
    }

    set_success();
}

void handle_dirrm_command()
{
    if (!check_args(2, "dirrm", "Missing directory name")) return;

    if (rmdir(token_values[1]) != 0)
    {
        set_errno_status("dirrm");
        return;
    }

    set_success();
}

void handle_dirls_command()
{
    const char* directory_ch = get_effective_token_count() > 1 ? token_values[1] : working_directory;
    DIR* directory_dr = opendir(directory_ch);
    if (directory_dr == NULL)
    {
        set_errno_status("dirls");
        return;
    }

    struct dirent* entry;
    int first_content = 1;
    while ((entry = readdir(directory_dr)) != NULL)
    {
        printf("%s%s", first_content ? "" : "  ", entry->d_name);
        first_content = 0;
    }
    printf("\n");

    closedir(directory_dr);
    set_success();
}

void handle_rename_command()
{
    if (!check_args(3, "rename", "Insufficient amount of parameters")) return;

    if (rename(token_values[1], token_values[2]) != 0)
    {
        set_errno_status("rename");
        return;
    }

    set_success();
}

void handle_unlink_command()
{
    if (!check_args(2, "unlink", "Insufficient amount of parameters")) return;

    if (unlink(token_values[1]) != 0)
    {
        set_errno_status("unlink");
        return;
    }

    set_success();
}

void handle_remove_command()
{
    if (!check_args(2, "remove", "Insufficient amount of parameters")) return;

    if (remove(token_values[1]) != 0)
    {
        set_errno_status("remove");
        return;
    }

    set_success();
}

void handle_linkhard_command()
{
    if (!check_args(3, "linkhard", "Insufficient amount of parameters")) return;

    if (link(token_values[1], token_values[2]) != 0)
    {
        set_errno_status("linkhard");
        return;
    }

    set_success();
}

void handle_linksoft_command()
{
    if (!check_args(3, "linksoft", "Insufficient amount of parameters")) return;

    if (symlink(token_values[1], token_values[2]) != 0)
    {
        set_errno_status("linksoft");
        return;
    }

    set_success();
}

void handle_linkread_command()
{
    if (!check_args(2, "linkread", "Insufficient amount of parameters")) return;

    char buffer[MAX_TOKEN_SIZE];
    ssize_t len = readlink(token_values[1], buffer, sizeof(buffer) - 1);

    if (len == -1)
    {
        set_errno_status("linkread");
        return;
    }

    buffer[len] = '\0';
    printf("%s\n", buffer);
    set_success();
}

void handle_linklist_command()
{
    if (!check_args(2, "linklist", "Insufficient amount of parameters")) return;

    struct stat file_stat;
    if (stat(token_values[1], &file_stat) != 0)
    {
        set_errno_status("linklist");
        return;
    }

    DIR* directory_dr = opendir(working_directory);
    if (directory_dr == NULL)
    {
        set_errno_status("linklist");
        return;
    }

    struct dirent* entry;
    struct stat entry_stat;
    int first_content = 1;
    while ((entry = readdir(directory_dr)) != NULL)
    {
        if (stat(entry->d_name, &entry_stat) == 0)
        {
            if (entry_stat.st_ino == file_stat.st_ino)
            {
                printf("%s%s", first_content ? "" : "  ", entry->d_name);
                first_content = 0;
            }
        }
    }
    printf("\n");

    closedir(directory_dr);
    set_success();
}

void handle_cpcat_command()
{
    int effective_count = get_effective_token_count();
    if (effective_count < 2 && input_redirect == NULL && !pipeline_stage_active)
    {
        set_missing_args_status("cpcat", "Insufficient amount of parameters");
        return;
    }

    int in = -1;
    int out = -1;
    char buffer[READ_WRITE_BUFFER_SIZE];
    ssize_t bytes_read;

    // Open input file, or use redirected standard input when no file is specified.
    if (effective_count < 2 || strcmp(token_values[1], "-") == 0)
    {
        in = STDIN_FILENO;
    }
    else
    {
        in = open(token_values[1], O_RDONLY);
        if (in < 0)
        {
            set_errno_status("cpcat");
            return;
        }
    }

    // Open output file if specified as a regular argument.
    // Redirection to stdout has already been handled by the shell.
    if (effective_count >= 3)
    {
        out = open(token_values[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0)
        {
            set_errno_status("cpcat");
            if (in != STDIN_FILENO) close(in);
            return;
        }
    }
    else
    {
        out = STDOUT_FILENO;
    }

    // Copy contents
    while ((bytes_read = read(in, buffer, READ_WRITE_BUFFER_SIZE)) > 0)
    {
        ssize_t bytes_written = 0;
        while (bytes_written < bytes_read)
        {
            ssize_t result = write(out, buffer + bytes_written, bytes_read - bytes_written);
            if (result < 0)
            {
                set_errno_status("cpcat");
                if (in != STDIN_FILENO) close(in);
                if (out != STDOUT_FILENO) close(out);
                return;
            }
            bytes_written += result;
        }
    }

    // Handle read errors
    if (bytes_read < 0)
    {
        set_errno_status("cpcat");
        if (in != STDIN_FILENO) close(in);
        if (out != STDOUT_FILENO) close(out);
        return;
    }

    // Close files
    if (in != STDIN_FILENO) close(in);
    if (out != STDOUT_FILENO) close(out);

    set_success();
}

void print_pid_result(pid_t value)
{
    printf("%jd\n", (intmax_t)value);
    set_success();
}

void print_uid_result(uid_t value)
{
    printf("%ju\n", (uintmax_t)value);
    set_success();
}

void print_gid_result(gid_t value)
{
    printf("%ju\n", (uintmax_t)value);
    set_success();
}

void handle_pid_command() { print_pid_result(getpid()); }
void handle_ppid_command() { print_pid_result(getppid()); }
void handle_uid_command() { print_uid_result(getuid()); }
void handle_euid_command() { print_uid_result(geteuid()); }
void handle_gid_command() { print_gid_result(getgid()); }
void handle_egid_command() { print_gid_result(getegid()); }

void handle_sysinfo_command()
{
    struct utsname response;
    if (uname(&response) == -1)
    {
        set_errno_status("sysinfo");
        return;
    }

    printf("Sysname: %s\n", response.sysname);
    printf("Nodename: %s\n", response.nodename);
    printf("Release: %s\n", response.release);
    printf("Version: %s\n", response.version);
    printf("Machine: %s\n", response.machine);

    set_success();
}

void handle_proc_command()
{
    if (get_effective_token_count() < 2)
    {
        printf("%s\n", procfs);
        set_success();
        return;
    }

    const char* directory = token_values[1];
    if (access(directory, F_OK | R_OK) == 0)
    {
        strcpy(procfs, directory);
        set_success();
    }
    else
    {
        exit_status = 1;
    }
}

int is_digit(char c) { return (c >= '0' && c <= '9'); }

int is_pid_directory(const struct dirent* directory)
{
    if (directory->d_type != DT_DIR) return 0;

    for (const char* directory_name = directory->d_name; *directory_name != '\0'; directory_name++)
    {
        if (!is_digit(*directory_name)) return 0;
    }
    return 1;
}

void handle_pids_command()
{
    struct dirent** entries;
    int entry_count = scandir(procfs, &entries, is_pid_directory, alphasort);
    if (entry_count < 0)
    {
        set_errno_status("pids");
        return;
    }

    for (int ix = 0; ix < entry_count; ix++)
    {
        printf("%s\n", entries[ix]->d_name);
        free(entries[ix]);
    }
    free(entries);
    set_success();
}

typedef struct
{
    int pid;
    int ppid;
    char name[MAX_PID_NAME_LEN];
    char state;
} ProcessInfo;

int get_pinfo(int pid, ProcessInfo* info)
{
    char directory[MAX_DIRECTORY_LEN];
    snprintf(directory, sizeof(directory), "%s/%d/stat", procfs, pid);

    FILE* stat_file = fopen(directory, "r");
    if (stat_file == NULL) return 0;

    // `(%4095[^)])` matches strings inside parenthesis.
    // NOTE: The numeric value inside `(%4095[^)])` must equal `MAX_DIRECTORY_LEN - 1`!
    fscanf(stat_file, "%d (%4095[^)]) %c %d",
           &info->pid,
           info->name,
           &info->state,
           &info->ppid
    );
    fclose(stat_file);
    return 1;
}

void handle_pinfo_command()
{
    struct dirent** entries;
    int entry_count = scandir(procfs, &entries, is_pid_directory, alphasort);
    if (entry_count < 0)
    {
        set_errno_status("pinfo");
        return;
    }

    printf("  PID  PPID STANJE IME\n");
    for (int ix = 0; ix < entry_count; ix++)
    {
        ProcessInfo info;
        int pid = atoi(entries[ix]->d_name);
        if (get_pinfo(pid, &info))
        {
            printf("%5d %5d %6c %s\n",
                   info.pid,
                   info.ppid,
                   info.state,
                   info.name
            );
        }
        free(entries[ix]);
    }

    free(entries);
    set_success();
}

int get_child_exit_status(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int find_background_child(pid_t pid)
{
    for (int ix = 0; ix < background_count; ix++)
    {
        if (background_pids[ix] == pid) return ix;
    }
    return -1;
}

void remove_background_child(int child_ix)
{
    for (int ix = child_ix; ix < background_count - 1; ix++)
    {
        background_pids[ix] = background_pids[ix + 1];
        background_statuses[ix] = background_statuses[ix + 1];
        background_finished[ix] = background_finished[ix + 1];
    }
    background_count--;
}

void register_background_child(pid_t pid)
{
    if (background_count >= MAX_TOKEN_COUNT) return;

    background_pids[background_count] = pid;
    background_statuses[background_count] = 0;
    background_finished[background_count] = 0;
    background_count++;
}

void set_background_child_status(pid_t pid, int status)
{
    int child_ix = find_background_child(pid);
    if (child_ix < 0) return;

    background_statuses[child_ix] = get_child_exit_status(status);
    background_finished[child_ix] = 1;
}

void cleanup_background_children()
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) set_background_child_status(pid, status);
}

void handle_waitone_command()
{
    pid_t requested_pid = get_effective_token_count() > 1 ? (pid_t)atoi(token_values[1]) : -1;
    pid_t pid;
    int status;
    int child_ix;

    cleanup_background_children();

    if (requested_pid > 0)
    {
        child_ix = find_background_child(requested_pid);
        if (child_ix < 0)
        {
            exit_status = 0;
            return;
        }

        if (background_finished[child_ix])
        {
            exit_status = background_statuses[child_ix];
            remove_background_child(child_ix);
            return;
        }

        pid = waitpid(requested_pid, &status, 0);
        if (pid < 0)
        {
            exit_status = 0;
            return;
        }

        exit_status = get_child_exit_status(status);
        child_ix = find_background_child(pid);
        if (child_ix >= 0) remove_background_child(child_ix);
        return;
    }

    for (child_ix = 0; child_ix < background_count; child_ix++)
    {
        if (background_finished[child_ix])
        {
            exit_status = background_statuses[child_ix];
            remove_background_child(child_ix);
            return;
        }
    }

    if (background_count == 0)
    {
        exit_status = 0;
        return;
    }

    pid = waitpid(-1, &status, 0);
    if (pid < 0)
    {
        exit_status = 0;
        return;
    }

    exit_status = get_child_exit_status(status);
    child_ix = find_background_child(pid);
    if (child_ix >= 0) remove_background_child(child_ix);
}

void handle_waitall_command()
{
    pid_t pid;
    int status;
    int child_ix;

    cleanup_background_children();

    while (background_count > 0)
    {
        child_ix = background_count - 1;
        if (background_finished[child_ix])
        {
            remove_background_child(child_ix);
            continue;
        }

        pid = waitpid(background_pids[child_ix], &status, 0);
        if (pid < 0)
        {
            remove_background_child(child_ix);
            continue;
        }

        child_ix = find_background_child(pid);
        if (child_ix >= 0) remove_background_child(child_ix);
    }

    set_success();
}


/* ---------------------------------------------------------[ Pipeline helpers ]-------------------------------------------------------- */

void close_pipeline_pipes(int pipe_fds[][2], int pipe_count)
{
    for (int ix = 0; ix < pipe_count; ix++)
    {
        close(pipe_fds[ix][0]);
        close(pipe_fds[ix][1]);
    }
}

void reset_command_state()
{
    token_count = 0;
    optional_token_count = 0;
    input_redirect = NULL;
    output_redirect = NULL;
    background_active = 0;
}

void execute_command();

void execute_pipeline_stage(const char* stage_command)
{
    char command_buffer[MAX_TOKEN_SIZE];
    strncpy(command_buffer, stage_command, MAX_TOKEN_SIZE - 1);
    command_buffer[MAX_TOKEN_SIZE - 1] = '\0';

    reset_command_state();
    pipeline_stage_active = 1;

    set_tokens(command_buffer);
    if (token_count == 0) exit(0);
    set_optional_tokens();
    execute_command();
    exit(exit_status);
}

int setup_pipeline_child(int stage_ix, int stage_count, int pipe_fds[][2])
{
    if (stage_ix > 0)
    {
        if (dup2(pipe_fds[stage_ix - 1][0], STDIN_FILENO) < 0)
        {
            set_errno_status("pipes");
            return 0;
        }
    }
    else if (input_redirect != NULL)
    {
        if (!redirect_input("pipes")) return 0;
    }

    if (stage_ix < stage_count - 1)
    {
        if (dup2(pipe_fds[stage_ix][1], STDOUT_FILENO) < 0)
        {
            set_errno_status("pipes");
            return 0;
        }
    }
    else if (output_redirect != NULL)
    {
        if (!redirect_output("pipes")) return 0;
    }

    return 1;
}

void wait_pipeline_children(pid_t* pids, int pid_count)
{
    int status;
    int last_status = 0;

    for (int ix = 0; ix < pid_count; ix++)
    {
        if (waitpid(pids[ix], &status, 0) < 0)
        {
            set_errno_status("waitpid");
            continue;
        }

        if (ix == pid_count - 1) last_status = get_child_exit_status(status);
    }

    exit_status = last_status;
}

void handle_pipes_command()
{
    int effective_count = get_effective_token_count();
    int stage_count = effective_count - 1;
    int pipe_count = stage_count - 1;
    int pipe_fds[MAX_TOKEN_COUNT][2];
    pid_t pids[MAX_TOKEN_COUNT];
    int started_count = 0;

    if (stage_count < 2)
    {
        set_missing_args_status("pipes", "Insufficient amount of parameters");
        return;
    }

    for (int ix = 0; ix < pipe_count; ix++)
    {
        if (pipe(pipe_fds[ix]) < 0)
        {
            set_errno_status("pipes");
            close_pipeline_pipes(pipe_fds, ix);
            return;
        }
    }

    fflush(stdout);
    fflush(stderr);
    fflush(stdin);

    for (int stage_ix = 0; stage_ix < stage_count; stage_ix++)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            set_errno_status("fork");
            close_pipeline_pipes(pipe_fds, pipe_count);
            wait_pipeline_children(pids, started_count);
            return;
        }

        if (pid == 0)
        {
            if (!setup_pipeline_child(stage_ix, stage_count, pipe_fds))
            {
                close_pipeline_pipes(pipe_fds, pipe_count);
                exit(exit_status);
            }

            close_pipeline_pipes(pipe_fds, pipe_count);
            execute_pipeline_stage(token_values[stage_ix + 1]);
        }

        pids[started_count++] = pid;
    }

    close_pipeline_pipes(pipe_fds, pipe_count);

    if (background_active)
    {
        for (int ix = 0; ix < started_count; ix++) register_background_child(pids[ix]);
        set_success();
        return;
    }

    wait_pipeline_children(pids, started_count);
}

/* --------------------------------------------------------[ Custom functions ]-------------------------------------------------------- */

typedef struct
{
    char name[MAX_MARK_NAME_LEN + 1];
    char directory[MAX_DIRECTORY_LEN + 1];
} DirectoryMark;

DirectoryMark directory_marks[MAX_MARK_COUNT];
int directory_mark_count = 0;

char marks_file_path[MAX_DIRECTORY_LEN + 1];

int get_marks_file_path(char* buffer, size_t buffer_size)
{
    const char* home = getenv("HOME");

    if (home == NULL || strlen(home) == 0)
    {
        snprintf(buffer, buffer_size, "%s", MARKS_FILE_NAME);
    }
    else
    {
        snprintf(buffer, buffer_size, "%s/%s", home, MARKS_FILE_NAME);
    }

    return 1;
}

int is_valid_mark_name(const char* name)
{
    if (name == NULL || strlen(name) == 0)
        return 0;

    if (strlen(name) > MAX_MARK_NAME_LEN)
        return 0;

    for (const char* ch = name; *ch != '\0'; ch++)
    {
        if (isspace(*ch) || *ch == '\t' || *ch == '\n')
            return 0;
    }

    return 1;
}

int find_directory_mark(DirectoryMark* marks, int mark_count, const char* name)
{
    for (int ix = 0; ix < mark_count; ix++)
    {
        if (strcmp(marks[ix].name, name) == 0)
            return ix;
    }

    return -1;
}

int compare_directory_marks(const void* left, const void* right)
{
    const DirectoryMark* left_mark = (const DirectoryMark*)left;
    const DirectoryMark* right_mark = (const DirectoryMark*)right;

    return strcmp(left_mark->name, right_mark->name);
}

int load_directory_marks(DirectoryMark* marks, int* mark_count)
{
    char marks_file[MAX_DIRECTORY_LEN + 1];
    get_marks_file_path(marks_file, sizeof(marks_file));

    FILE* file = fopen(marks_file, "r");

    *mark_count = 0;

    if (file == NULL)
    {
        if (errno == ENOENT)
        {
            return 1;
        }

        set_errno_status("marks");
        return 0;
    }

    char line[MAX_DIRECTORY_LEN + MAX_MARK_NAME_LEN + 4];

    while (fgets(line, sizeof(line), file) != NULL)
    {
        if (*mark_count >= MAX_MARK_COUNT)
            break;

        size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        char* separator = strchr(line, '\t');

        if (separator == NULL)
            continue;

        *separator = '\0';

        const char* name = line;
        const char* directory = separator + 1;

        if (!is_valid_mark_name(name))
            continue;

        if (strlen(directory) > MAX_DIRECTORY_LEN)
            continue;

        strcpy(marks[*mark_count].name, name);
        strcpy(marks[*mark_count].directory, directory);

        (*mark_count)++;
    }

    fclose(file);

    qsort(marks, *mark_count, sizeof(DirectoryMark), compare_directory_marks);

    return 1;
}

int save_directory_marks(DirectoryMark* marks, int mark_count)
{
    char marks_file[MAX_DIRECTORY_LEN + 1];
    get_marks_file_path(marks_file, sizeof(marks_file));

    qsort(marks, mark_count, sizeof(DirectoryMark), compare_directory_marks);

    FILE* file = fopen(marks_file, "w");

    if (file == NULL)
    {
        set_errno_status("marks");
        return 0;
    }

    for (int ix = 0; ix < mark_count; ix++)
    {
        fprintf(file, "%s\t%s\n", marks[ix].name, marks[ix].directory);
    }

    fclose(file);

    return 1;
}

void handle_mark_command()
{
    int effective_count = get_effective_token_count();

    DirectoryMark marks[MAX_MARK_COUNT];
    int mark_count = 0;

    if (!load_directory_marks(marks, &mark_count))
        return;

    if (effective_count == 1)
    {
        for (int ix = 0; ix < mark_count; ix++)
        {
            printf("%s -> %s\n", marks[ix].name, marks[ix].directory);
        }

        set_success();
        return;
    }

    if (effective_count != 2)
    {
        set_missing_args_status("mark", "Usage: mark [NAME]");
        return;
    }

    const char* name = token_values[1];

    if (!is_valid_mark_name(name))
    {
        fprintf(stderr, "mark: Invalid mark name '%s'\n", name);
        exit_status = EINVAL;
        return;
    }

    char current_directory[MAX_DIRECTORY_LEN + 1];

    if (getcwd(current_directory, sizeof(current_directory)) == NULL)
    {
        set_errno_status("mark");
        return;
    }

    if (strchr(current_directory, '\t') != NULL ||
        strchr(current_directory, '\n') != NULL)
    {
        fprintf(stderr, "mark: Current directory contains unsupported characters\n");
        exit_status = EINVAL;
        return;
    }

    int mark_ix = find_directory_mark(marks, mark_count, name);

    if (mark_ix < 0)
    {
        if (mark_count >= MAX_MARK_COUNT)
        {
            fprintf(stderr, "mark: Too many marks\n");
            exit_status = ENOMEM;
            return;
        }

        mark_ix = mark_count++;
    }

    strcpy(marks[mark_ix].name, name);
    strcpy(marks[mark_ix].directory, current_directory);

    if (!save_directory_marks(marks, mark_count))
        return;

    set_success();
}

void handle_jump_command()
{
    if (!check_args(2, "jump", "Missing mark name"))
        return;

    DirectoryMark marks[MAX_MARK_COUNT];
    int mark_count = 0;

    if (!load_directory_marks(marks, &mark_count))
        return;

    const char* name = token_values[1];
    int mark_ix = find_directory_mark(marks, mark_count, name);

    if (mark_ix < 0)
    {
        fprintf(stderr, "jump: Unknown mark '%s'\n", name);
        exit_status = ENOENT;
        return;
    }

    if (chdir(marks[mark_ix].directory) != 0)
    {
        set_errno_status("jump");
        return;
    }

    if (getcwd(working_directory, MAX_DIRECTORY_LEN + 1) == NULL)
    {
        set_errno_status("jump");
        return;
    }

    set_success();
}

void handle_unmark_command()
{
    if (!check_args(2, "unmark", "Missing mark name"))
        return;

    DirectoryMark marks[MAX_MARK_COUNT];
    int mark_count = 0;

    if (!load_directory_marks(marks, &mark_count))
        return;

    const char* name = token_values[1];
    int mark_ix = find_directory_mark(marks, mark_count, name);

    if (mark_ix < 0)
    {
        fprintf(stderr, "unmark: Unknown mark '%s'\n", name);
        exit_status = ENOENT;
        return;
    }

    for (int ix = mark_ix; ix < mark_count - 1; ix++)
    {
        marks[ix] = marks[ix + 1];
    }

    mark_count--;

    if (!save_directory_marks(marks, mark_count))
        return;

    set_success();
}

/* -----------------------------------------------[ External function command handlers ]----------------------------------------------- */

void handle_external_command()
{
    int effective_count = get_effective_token_count();
    char* arguments[MAX_TOKEN_COUNT + 1];
    pid_t pid;
    int status;

    for (int ix = 0; ix < effective_count; ix++) arguments[ix] = token_values[ix];
    arguments[effective_count] = NULL;

    if (debug_level) print_external();

    fflush(stdout);
    fflush(stderr);
    fflush(stdin);

    pid = fork();
    if (pid < 0)
    {
        set_errno_status("fork");
        return;
    }

    if (pid == 0)
    {
        if (!apply_redirects(arguments[0])) exit(exit_status);
        execvp(arguments[0], arguments);
        perror("exec");
        exit(127);
    }

    if (background_active)
    {
        register_background_child(pid);
        set_success();
        return;
    }

    if (waitpid(pid, &status, 0) < 0)
    {
        set_errno_status("waitpid");
        return;
    }

    exit_status = get_child_exit_status(status);
}

/* -------------------------------------------------------[ Execution handler ]-------------------------------------------------------- */

typedef struct
{
    const char* name;
    void (*handler)(void);
} Command;

Command builtin_commands[] = {
    {"debug", handle_debug_command},
    {"prompt", handle_prompt_command},
    {"status", handle_status_command},
    {"exit", handle_exit_command},
    {"help", handle_help_command},
    {"print", handle_print_command},
    {"echo", handle_echo_command},
    {"len", handle_len_command},
    {"sum", handle_sum_command},
    {"calc", handle_calc_command},
    {"basename", handle_basename_command},
    {"dirname", handle_dirname_command},
    {"dirch", handle_dirch_command},
    {"dirwd", handle_dirwd_command},
    {"dirmk", handle_dirmk_command},
    {"dirrm", handle_dirrm_command},
    {"dirls", handle_dirls_command},
    {"rename", handle_rename_command},
    {"unlink", handle_unlink_command},
    {"remove", handle_remove_command},
    {"linkhard", handle_linkhard_command},
    {"linksoft", handle_linksoft_command},
    {"linkread", handle_linkread_command},
    {"linklist", handle_linklist_command},
    {"cpcat", handle_cpcat_command},
    {"pid", handle_pid_command},
    {"ppid", handle_ppid_command},
    {"uid", handle_uid_command},
    {"euid", handle_euid_command},
    {"gid", handle_gid_command},
    {"egid", handle_egid_command},
    {"sysinfo", handle_sysinfo_command},
    {"proc", handle_proc_command},
    {"pids", handle_pids_command},
    {"pinfo", handle_pinfo_command},
    {"waitone", handle_waitone_command},
    {"waitall", handle_waitall_command},
    {"pipes", handle_pipes_command},
    {"mark", handle_mark_command},
    {"unmark", handle_unmark_command},
    {"jump", handle_jump_command},
    {NULL, NULL},
};

void execute_command()
{
    char* command = token_values[0];
    for (int command_ix = 0; builtin_commands[command_ix].name != NULL; command_ix++)
    {
        if (!strcmp(command, builtin_commands[command_ix].name))
        {
            if (debug_level) print_builtin_execution();

            if (background_active)
            {
                pid_t pid;

                fflush(stdout);
                fflush(stderr);
                fflush(stdin);

                pid = fork();
                if (pid < 0)
                {
                    set_errno_status("fork");
                    return;
                }

                if (pid == 0)
                {
                    if (!apply_redirects(command)) exit(exit_status);
                    builtin_commands[command_ix].handler();
                    exit(exit_status);
                }

                register_background_child(pid);
                set_success();
                return;
            }

            RedirectState redirect_state;
            if (!apply_foreground_redirects(command, &redirect_state)) return;
            builtin_commands[command_ix].handler();
            restore_redirects(&redirect_state);
            return;
        }
    }

    handle_external_command();
}

/* --------------------------------------------------------------[ Main ]-------------------------------------------------------------- */

int main(int argc, char* argv[])
{
    // Set default prompt title
    prompt_value = malloc(sizeof(char) * (MAX_SHELL_NAME_LEN + 1));
    strcpy(prompt_value, "mysh");

    // Set default working directory
    working_directory = malloc(sizeof(char) * (MAX_DIRECTORY_LEN + 1));
    if (getcwd(working_directory, MAX_DIRECTORY_LEN + 1) == NULL)
    {
        strcpy(working_directory, "/");
    }

    // Set default process file system directory
    procfs = malloc(sizeof(char) * (MAX_DIRECTORY_LEN + 1));
    strcpy(procfs, "/proc");

    init_tokens();
    char buffer[MAX_INPUT_SIZE];
    while (1)
    {
        cleanup_background_children();

        // Set default values
        token_count = 0;
        optional_token_count = 0;
        input_redirect = NULL;
        output_redirect = NULL;
        background_active = 0;

        if (isatty(STDIN_FILENO))
        {
            printf("%s> ", prompt_value);
            fflush(stdout);
        }

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) break;

        int str_len = strlen(buffer);
        if (str_len > 0 && buffer[str_len - 1] == '\n') buffer[--str_len] = '\0';
        if (debug_level > 0) printf("Input line: '%s'\n", buffer);

        set_tokens(buffer);
        if (token_count == 0) continue;
        set_optional_tokens();
        execute_command();
        cleanup_background_children();

        if (exit_active) break;
    }

    free(prompt_value);
    free(working_directory);
    free(procfs);
    deinit_tokens();
    return exit_status;
}
