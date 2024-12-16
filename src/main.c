#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <libgen.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

/*

initialize shell data

in work loop :
    X display promt
    X get input into static allocated input buffer------------------EXAMPLE : (char[INPUT_BUFFER_SIZE])"ls -l ../"
    X divide into tokens--------------------------------------------EXAMPLE : "ls -l ../" --> ["ls","-l","../",NULL]
    process input :
        add to the history
        if shell command :
            execute shell command-----------------------------------EXAMPLE : cd, exit, jobs, fg, kill
        if system command :
            if contains & :
                execute like background
            else :
                execute like foreground
    process signals :
        UNDEFINDED NOW


*/

#define SHELL_NAME "Tshell"
#define COMMAND_SIZE 256
#define MAX_ARGS_COUNT 64

#define INPUT_BUFFER_SIZE (COMMAND_SIZE * MAX_ARGS_COUNT)
#define PATH_MAX 4096

#define INITIAL_BG_TASKS_COUNT 8
#define MAX_BG_TASKS_COUNT 128


#define EXIT_FLAG 0b00000001

/*

flag byte : 0   0   0   0 | 0   0   0 | 0 |
                          |     2     | 1 |

1 - Exit flag
2 - Exit status | Not implemented yet

*/

typedef unsigned char FLAG;

typedef union flags
{
    FLAG flags_byte;
    struct {
        FLAG exit_flag:1;
    } flags_bitmap; 
} flags;

typedef struct {
    pid_t pid;
    char* cmd;

    int status;
} task;

typedef struct {
    task foreground_task;
    task* background_tasks;

    int bg_tasks_count;
    int bg_tasks_capacity;
} tasks;

void init_tasks(tasks* t){
    t->background_tasks = malloc(sizeof(task) * INITIAL_BG_TASKS_COUNT);
    t->bg_tasks_capacity = INITIAL_BG_TASKS_COUNT;
    t->bg_tasks_count = 0;
}

void init_flags(flags* f){
    memset(f,0,sizeof f);
}

int check_flags(flags* f, FLAG flags_to_check){
    return (f->flags_byte & flags_to_check) == flags_to_check;
}

void display_banner() {
    printf("\n\n");
    printf(" ████████╗███████╗██╗  ██╗███████╗██╗     ██╗     \n");
    printf(" ╚══██╔══╝██╔════╝██║  ██║██╔════╝██║     ██║     \n");
    printf("    ██║   ███████╗███████║█████╗  ██║     ██║     \n");
    printf("    ██║   ╚════██║██╔══██║██╔══╝  ██║     ██║     \n");
    printf("    ██║   ███████║██║  ██║███████╗███████╗███████╗\n");
    printf("    ╚═╝   ╚══════╝╚═╝  ╚═╝╚══════╝╚══════╝╚══════╝\n");
    printf("\n");
    printf("@ Powered by Tolik :D  --- use on your own risk ---\n\n\n");
}

void display_shell_prompt() {
    char path[PATH_MAX];
    char* username = getenv("USER"); // Get the username
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);

    if (len != -1) {
        path[len] = '\0'; // Null-terminate the string
    } else {
        perror("readlink");
        return;
    }

    // Combine shell name and username
    char shell_info[PATH_MAX];
    snprintf(shell_info, sizeof(shell_info), "%s : %s", SHELL_NAME, username ? username : "Unknown User");

    // Get current directory
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    char path_info[PATH_MAX];
    snprintf(path_info, sizeof(path_info), "workdir : %.1000s", cwd);

    if (path_info != NULL) {
        // Calculate the maximum length between shell info and current directory
        size_t shell_info_length = strlen(shell_info);
        size_t cwd_length = strlen(path_info);
        size_t max_length = shell_info_length > cwd_length ? shell_info_length : cwd_length;

        // Add some padding for aesthetics
        max_length += 4;  // 2 spaces on each side of the text
        
        // Print the top border of the dynamic box
        printf("┌");
        for (size_t i = 0; i < max_length; i++) {
            printf("─");
        }
        printf("┐\n");

        // Print the shell info line (shell name + user)
        printf("│ %-*s │\n", (int)(max_length - 2), shell_info);

        // Print the current directory line
        printf("│ %-*s │\n", (int)(max_length - 2), path_info);

        // Print the bottom border of the dynamic box
        printf("└");
        for (size_t i = 0; i < max_length; i++) {
            printf("─");
        }
        printf("┘\n");
    }

    // Print the prompt symbol
    printf("> ");
}

void get_input(char input_buffer[]){
    if(fgets(input_buffer, sizeof(char[INPUT_BUFFER_SIZE]), stdin)) {
        size_t input_len = strlen(input_buffer);
        if(input_len > 0 && input_buffer[input_len - 1] == '\n'){
            input_buffer[input_len - 1] = '\0';
        }
    }
}

char** tokenize_input(char input_buffer[], size_t *token_count){
    
    size_t capacity = 2;
    size_t cursor = 0;
    char** tokens = malloc(sizeof(char*) * capacity);
    if (tokens == NULL) {
        perror("malloc failed");
        exit(1);
    }

    char* token;
    token = strtok(input_buffer, " ");
    while (token != NULL)
    {
        if(cursor >= capacity){
            capacity *= 2;
            char** reallocated_tokens = (char**) realloc(tokens, sizeof(char*) * capacity);
            if(reallocated_tokens == NULL){
                perror("realocation failed");
                free(tokens);
                exit(1);
            }

            tokens = reallocated_tokens;
        }

        tokens[cursor] = malloc(strlen(token) + 1);
        if (tokens[cursor] == NULL) {
            perror("malloc failed for token");
            // Free previously allocated memory before exiting
            for (size_t i = 0; i < cursor; ++i) {
                free(tokens[i]);
            }
            free(tokens);
            exit(1);
        }
        strcpy(tokens[cursor], token);
        tokens[cursor][strlen(token)] = '\0';

        cursor++;
        token = strtok(NULL, " ");
    }

    tokens[cursor] = NULL;
    *token_count = cursor;

    return tokens;
}

void free_tokens(char** tokens, size_t token_count){
    for(size_t i = 0; i < token_count; i++){
        free(tokens[i]);
    }
    free(tokens);
}

void process_tokens(char **tokens, size_t token_count, flags *f, tasks *t){
    
    // process shell commands

    if(strcmp(tokens[0],"cd") == 0){
        if(token_count > 2){
            printf("cd accept only 1 arg. Recieved : %zu", token_count - 1);
            return;
        }

        int cd_status = chdir(tokens[1]);
        if(cd_status == 0){
            printf("successfully changed dirrectory to %s\n",tokens[1]);
        } else {
            switch (errno)
            {
            case EACCES:
                printf("permisson denied to access %s\n",tokens[1]);
                break;
            case EFAULT:
                printf("path lie outside the accessible address space : %s\n", tokens[1]);
                break;
            case EIO:
                printf("I/O error occured for %s\n", tokens[1]);
                break;
            case ELOOP:
                printf("too many symbolic links for %s\n", tokens[1]);
                break;
            case ENAMETOOLONG:
                printf("path is too long : %s\n", tokens[1]);
                break;
            case ENOENT:
                printf("file does not exist : %s\n", tokens[1]);
                break;
            case ENOMEM:
                printf("there is insufficient kernel memory available for %s\n", tokens[1]);
                break;
            case ENOTDIR:
                printf("Component of path is not directory : %s\n", tokens[1]);
                break;

            default:
                printf("Unknown error occured for %s\n", tokens[1]);
                break;
            }
        }

        return;
    } else if (strcmp(tokens[0],"pwd") == 0){
        if(token_count > 1){
            printf("pwd accept only 0 arg. Recieved : %zu", token_count - 1);
            return;
        }

        char path[PATH_MAX];
        printf("%s\n", getcwd(path, PATH_MAX));
        return;
    } else if (strcmp(tokens[0],"exit") == 0){
        if(token_count > 2){
            printf("exit accept only 1 arg. Recieved : %zu", token_count - 1);
            return;
        }
        
        f->flags_byte |= EXIT_FLAG;
        return;
    } else if (strcmp(tokens[0],"jobs") == 0){
        printf("\nBackground tasks:\n");
        for (int i = 0; i < t->bg_tasks_count; i++) {
            printf("[%d] %s (PID: %d) - Status: %s\n", i + 1, t->background_tasks[i].cmd, t->background_tasks[i].pid,
                t->background_tasks[i].status == 0 ? "Running" : "Done");
        }
        return;
    } else if (strcmp(tokens[0],"fg") == 0) {
        if (token_count < 2) {
            printf("fg requires a job number\n");
            return;
        }

        int job_num = atoi(tokens[1]) - 1;
        if (job_num < 0 || job_num >= t->bg_tasks_count) {
            printf("Invalid job number\n");
            return;
        }

        task *bg_task = &t->background_tasks[job_num];
        printf("Bringing job [%d] %s to foreground...\n", job_num + 1, bg_task->cmd);

        // Wait for the background job to finish
        waitpid(bg_task->pid, NULL, 0);
        bg_task->status = 2; // Mark as done

        return;
    } else if (strcmp(tokens[0],"bg") == 0) {
        if (token_count < 2) {
            printf("bg requires a job number\n");
            return;
        }

        int job_num = atoi(tokens[1]) - 1;
        if (job_num < 0 || job_num >= t->bg_tasks_count) {
            printf("Invalid job number\n");
            return;
        }

        task *bg_task = &t->background_tasks[job_num];
        printf("Resuming job [%d] %s in background...\n", job_num + 1, bg_task->cmd);

        // Send SIGCONT to resume the background job
        kill(bg_task->pid, SIGCONT);
        bg_task->status = 0; // Mark as running

        return;
    }
    
    // process system commands

    if(tokens[token_count - 1] != NULL && strcmp(tokens[token_count - 1], "&") == 0){
        printf("background task occured...\n");
        tokens[token_count - 1] = NULL;
        pid_t pid = fork();

        if(pid == 0){
            // child process

            if (execvp(tokens[0], tokens) == -1) {
                perror("execvp failed");
                exit(EXIT_FAILURE);
            }
        } else if(pid > 0){
            // parent process

            printf("capacity : %d\ncount : %d\n", t->bg_tasks_capacity, t->bg_tasks_count);
            if(t->bg_tasks_count >= t->bg_tasks_capacity){
                if(t->bg_tasks_capacity * 2 <= MAX_BG_TASKS_COUNT){
                    t->bg_tasks_capacity *= 2;
                    t->background_tasks = (task*) realloc(t->background_tasks, sizeof(task) * t->bg_tasks_capacity);
                } else {
                    printf("CRITICAL : max bg tasks count");
                    exit(1);
                }
            }


            if(t->background_tasks != NULL){
                printf("creating background task for %s\n", tokens[0]);
                t->background_tasks[t->bg_tasks_count].pid = pid;
                t->background_tasks[t->bg_tasks_count].status = 0;
                t->background_tasks[t->bg_tasks_count].cmd = malloc(strlen(tokens[0]));
                strcpy(t->background_tasks[t->bg_tasks_count].cmd, tokens[0]);

                t->bg_tasks_count++;        

                printf("[Background] %d started: %s\n", pid, tokens[0]);
            } else {
                perror("Background task array does not allocated");
                exit(1);
            }
        }
        return;
    } else {
        pid_t pid = fork();
        if (pid == 0) {  // Child process
            if (execvp(tokens[0], tokens) == -1) {
                perror("execvp failed");
                exit(EXIT_FAILURE);
            }
        } else if (pid > 0) {  // Parent process
            // Wait for the foreground process to finish
            waitpid(pid, NULL, 0);
        }
        return;
    }

    printf("%s : uknown command\n", tokens[0]);
}

int main(void){
    display_banner();

    flags shell_flags;
    init_flags(&shell_flags);

    tasks shell_tasks;
    init_tasks(&shell_tasks);

    while (1)
    {
        display_shell_prompt();

        char input_buffer[INPUT_BUFFER_SIZE]; // fixed size input buffer
        get_input(input_buffer);

        size_t token_count;
        char** tokens = tokenize_input(input_buffer, &token_count);

        process_tokens(tokens, token_count, &shell_flags, &shell_tasks);

        free_tokens(tokens,token_count);

        //EXIT
        if(check_flags(&shell_flags, EXIT_FLAG)){
            printf("Stopping shell...\n");
            break;
        }
    }

    return EXIT_SUCCESS;
}