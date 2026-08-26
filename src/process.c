#include "process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

void process_args_init(ProcessArgs* p_args, Arena* arena) {
    p_args->count    = 0;
    p_args->capacity = 16;
    p_args->args     = ARENA_NEW_ARRAY(arena, const char*, p_args->capacity);
}

void process_args_add(ProcessArgs* p_args, Arena* arena, const char* arg) {
    if (p_args->count + 1 >= p_args->capacity) {
        size_t old_cap = p_args->capacity;
        size_t new_cap = old_cap * 2;

        p_args->args     = (const char**)arena_realloc(arena, (void*)p_args->args, old_cap * sizeof(char*), new_cap * sizeof(char*));
        p_args->capacity = new_cap;
    }

    p_args->args[p_args->count++] = arg;
    p_args->args[p_args->count]   = NULL;
}

static void process_print_command(const ProcessArgs* p_args) {
    fprintf(stderr, "[klang:driver]");

    for (size_t i = 0; i < p_args->count; ++i) {
        const char* arg = p_args->args[i];

        if (strchr(arg, ' ') != NULL) {
            fprintf(stderr, " \"%s\"", arg);
        } else {
            fprintf(stderr, " %s", arg);
        }
    }

    fprintf(stderr, "\n");
}

int process_exec(const ProcessArgs* p_args, bool verbose) {
    if (!p_args || p_args->count == 0) {
        return -1;
    }

    if (verbose) {
        process_print_command(p_args);

        pid_t pid = fork();

        if (pid < 0) {
            perror("klang: fork failed");
            return -1;
        }

        if (pid == 0) {
            execvp(p_args->args[0], (char* const*)p_args->args);
            fprintf(stderr, "klang: failed to execute '%s': ", p_args->args[0]);
            perror("");
            _exit(127);
        }

        int status = 0;

        if (waitpid(pid, &status, 0) < 0) {
            perror("klang: waitpid failed");
            return -1;
        }

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }

        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }

        return -1;
    }

    int pipe_fd[2];

    if (pipe(pipe_fd) < 0) {
        perror("klang: pipe failed");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("klang: fork failed");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipe_fd[0]);

        dup2(pipe_fd[1], STDOUT_FILENO);
        dup2(pipe_fd[1], STDERR_FILENO);

        close(pipe_fd[1]);

        execvp(p_args->args[0], (char* const*)p_args->args);
        _exit(127);
    }

    close(pipe_fd[1]);

    size_t cap = 4096;
    size_t len = 0;
    char* buf  = (char*)malloc(cap);

    if (!buf) {
        abort();
    }

    char temp[1024];
    ssize_t bytes_read = 0;

    while ((bytes_read = read(pipe_fd[0], temp, sizeof(temp))) > 0) {
        if (len + (size_t)bytes_read >= cap) {
            while (len + (size_t)bytes_read >= cap) {
                cap *= 2;
            }

            char* new_buf = (char*)realloc(buf, cap);

            if (!new_buf) {
                free(buf);
                abort();
            }

            buf = new_buf;
        }

        memcpy(buf + len, temp, (size_t)bytes_read);
        len += (size_t)bytes_read;
    }

    close(pipe_fd[0]);

    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        free(buf);
        perror("klang: waitpid failed");
        return -1;
    }

    int exit_code = -1;

    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }

    if (exit_code != 0 && len > 0) {
        fwrite(buf, 1, len, stderr);
    }

    free(buf);

    return exit_code;
}

bool process_find_executable(const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }

    if (strchr(name, '/') != NULL) {
        return access(name, X_OK) == 0;
    }

    const char* path_env = getenv("PATH");

    if (!path_env) {
        return false;
    }

    size_t name_len = strlen(name);
    char buffer[4096];
    const char* start = path_env;

    while (*start != '\0') {
        const char* end = strchr(start, ':');
        size_t len = end ? (size_t)(end - start) : strlen(start);

        if (len == 0) {
            if (name_len + 3 <= sizeof(buffer)) {
                buffer[0] = '.';
                buffer[1] = '/';
                memcpy(buffer + 2, name, name_len);
                buffer[2 + name_len] = '\0';

                if (access(buffer, X_OK) == 0) {
                    return true;
                }
            }
        } else if (len + 1 + name_len + 1 <= sizeof(buffer)) {
            memcpy(buffer, start, len);
            buffer[len] = '/';
            memcpy(buffer + len + 1, name, name_len);
            buffer[len + 1 + name_len] = '\0';

            if (access(buffer, X_OK) == 0) {
                return true;
            }
        }

        if (!end) {
            break;
        }

        start = end + 1;
    }

    return false;
}