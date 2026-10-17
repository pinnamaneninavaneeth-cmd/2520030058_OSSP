#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>

volatile sig_atomic_t test_running = 0;

pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
long total_math_operations = 0;

typedef struct {
    int thread_id;
} ThreadData;

void handle_signals(int sig) {
    if (sig == SIGINT) {
        const char *msg = "\n\n[Signal] Interrupted by user (Ctrl+C)! Stopping active tests...\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        test_running = 0;
    } else if (sig == SIGALRM) {
        test_running = 0;
    }
}

void get_cpu_ticks(long *total_time, long *idle_time) {
    int fd = open("/proc/stat", O_RDONLY);
    if (fd < 0) return;

    char buffer[512];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0) return;
    buffer[bytes] = '\0';

    long user, nice, system, idle, iowait, irq, softirq;
    sscanf(buffer, "cpu %ld %ld %ld %ld %ld %ld %ld", 
           &user, &nice, &system, &idle, &iowait, &irq, &softirq);

    *idle_time = idle + iowait;
    *total_time = user + nice + system + idle + iowait + irq + softirq;
}

double get_cpu_usage_percentage(long prev_total, long prev_idle, long curr_total, long curr_idle) {
    long total_diff = curr_total - prev_total;
    long idle_diff = curr_idle - prev_idle;

    if (total_diff == 0) return 0.0;
    return ((double)(total_diff - idle_diff) / total_diff) * 100.0;
}

void get_memory_stats(double *used_mb, double *total_mb, double *used_percent) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return;

    char buffer[1024];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0) return;
    buffer[bytes] = '\0';

    long mem_total_kb = 0, mem_available_kb = 0;
    char *line = strtok(buffer, "\n");

    while (line != NULL) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line, "MemTotal: %ld kB", &mem_total_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line, "MemAvailable: %ld kB", &mem_available_kb);
        }
        line = strtok(NULL, "\n");
    }

    if (mem_total_kb > 0) {
        long mem_used_kb = mem_total_kb - mem_available_kb;
        *total_mb = mem_total_kb / 1024.0;
        *used_mb = mem_used_kb / 1024.0;
        *used_percent = ((double)mem_used_kb / mem_total_kb) * 100.0;
    }
}

void* cpu_stress_worker(void *arg) {
    long local_ops = 0;

    while (test_running) {
        volatile double x = 12345.6789;
        for (int i = 0; i < 5000; i++) {
            x = (x * 1.000001) + 0.00001;
        }
        local_ops++;
    }

    pthread_mutex_lock(&count_mutex);
    total_math_operations += local_ops;
    pthread_mutex_unlock(&count_mutex);

    pthread_exit(NULL);
}

void run_cpu_test(int duration) {
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("\n--- Starting CPU Stress Test (%d Seconds on %d Cores) ---\n", duration, num_cores);

    total_math_operations = 0;
    test_running = 1;

    pthread_t threads[num_cores];
    ThreadData t_data[num_cores];

    for (int i = 0; i < num_cores; i++) {
        t_data[i].thread_id = i + 1;
        pthread_create(&threads[i], NULL, cpu_stress_worker, &t_data[i]);
    }

    alarm(duration);

    long prev_total = 0, prev_idle = 0;
    get_cpu_ticks(&prev_total, &prev_idle);

    printf("Time Left | Current CPU Usage\n");
    printf("-----------------------------\n");

    int remaining = duration;
    while (test_running && remaining > 0) {
        sleep(1);
        remaining--;

        long curr_total = 0, curr_idle = 0;
        get_cpu_ticks(&curr_total, &curr_idle);
        double cpu_pct = get_cpu_usage_percentage(prev_total, prev_idle, curr_total, curr_idle);
        prev_total = curr_total;
        prev_idle = curr_idle;

        printf("   %2ds    |  [CPU] %.2f%%\n", remaining, cpu_pct);
    }

    for (int i = 0; i < num_cores; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("-----------------------------\n");
    printf("[Results] CPU Test Completed.\n");
    printf("Total math calculation batches completed: %ld\n\n", total_math_operations);
}

void run_memory_test(int mem_mb, int duration) {
    printf("\n--- Starting Memory Stress Test (Allocating %d MB for %d Seconds) ---\n", mem_mb, duration);

    size_t bytes = (size_t)mem_mb * 1024 * 1024;

    char *buffer = (char*)malloc(bytes);
    if (!buffer) {
        perror("Memory allocation failed");
        return;
    }

    char *mmap_buffer = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    long page_size = sysconf(_SC_PAGESIZE);

    for (size_t i = 0; i < bytes; i += page_size) {
        buffer[i] = 'X';
        if (mmap_buffer != MAP_FAILED) {
            mmap_buffer[i] = 'Y';
        }
    }

    test_running = 1;
    alarm(duration);

    printf("Time Left | Memory Used / Total MB | RAM Usage %%\n");
    printf("--------------------------------------------------\n");

    int remaining = duration;
    while (test_running && remaining > 0) {
        for (size_t i = 0; i < bytes; i += page_size * 16) {
            buffer[i] = (buffer[i] + 1) % 255;
        }

        sleep(1);
        remaining--;

        double used_mb = 0, total_mb = 0, ram_pct = 0;
        get_memory_stats(&used_mb, &total_mb, &ram_pct);
        printf("   %2ds    |   %.1f MB / %.1f MB  |   %.2f%%\n", remaining, used_mb, total_mb, ram_pct);
    }

    free(buffer);
    if (mmap_buffer != MAP_FAILED) {
        munmap(mmap_buffer, bytes);
    }

    printf("--------------------------------------------------\n");
    printf("[Results] Memory Test Completed. All RAM freed back to OS.\n\n");
}

void run_combined_test(int mem_mb, int duration) {
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("\n--- Starting Combined Test (%d Cores + %d MB RAM for %d Seconds) ---\n", 
           num_cores, mem_mb, duration);

    size_t bytes = (size_t)mem_mb * 1024 * 1024;
    char *buffer = (char*)malloc(bytes);
    if (buffer) {
        long page_size = sysconf(_SC_PAGESIZE);
        for (size_t i = 0; i < bytes; i += page_size) {
            buffer[i] = 'Z';
        }
    }

    total_math_operations = 0;
    test_running = 1;

    pthread_t threads[num_cores];
    ThreadData t_data[num_cores];

    for (int i = 0; i < num_cores; i++) {
        t_data[i].thread_id = i + 1;
        pthread_create(&threads[i], NULL, cpu_stress_worker, &t_data[i]);
    }

    alarm(duration);

    long prev_total = 0, prev_idle = 0;
    get_cpu_ticks(&prev_total, &prev_idle);

    printf("Time Left |  CPU Usage  |  RAM Usage (MB / %%) \n");
    printf("--------------------------------------------------\n");

    int remaining = duration;
    while (test_running && remaining > 0) {
        sleep(1);
        remaining--;

        long curr_total = 0, curr_idle = 0;
        get_cpu_ticks(&curr_total, &curr_idle);
        double cpu_pct = get_cpu_usage_percentage(prev_total, prev_idle, curr_total, curr_idle);
        prev_total = curr_total;
        prev_idle = curr_idle;

        double used_mb = 0, total_mb = 0, ram_pct = 0;
        get_memory_stats(&used_mb, &total_mb, &ram_pct);

        printf("   %2ds    |   %6.2f%%   |   %.1f MB (%.2f%%)\n", 
               remaining, cpu_pct, used_mb, ram_pct);
    }

    for (int i = 0; i < num_cores; i++) {
        pthread_join(threads[i], NULL);
    }

    if (buffer) free(buffer);

    printf("--------------------------------------------------\n");
    printf("[Results] Combined Stress Test Completed cleanly.\n\n");
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signals;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    int choice = 0;

    while (1) {
        double used_mb = 0, total_mb = 0, ram_pct = 0;
        get_memory_stats(&used_mb, &total_mb, &ram_pct);

        printf("=====================================================\n");
        printf("    CPU AND MEMORY STRESS TESTING TOOL (Ubuntu CLI)  \n");
        printf("=====================================================\n");
        printf("  [System Info] CPU Cores: %ld | Total RAM: %.1f MB\n", 
               sysconf(_SC_NPROCESSORS_ONLN), total_mb);
        printf("  [Current Baseline] Idle RAM Usage: %.1f MB (%.1f%%)\n", used_mb, ram_pct);
        printf("-----------------------------------------------------\n");
        printf("  1. Run CPU Stress Test\n");
        printf("  2. Run Memory Stress Test\n");
        printf("  3. Run Combined (CPU + Memory) Stress Test\n");
        printf("  4. Exit\n");
        printf("=====================================================\n");
        printf("Select an option (1-4): ");

        if (scanf("%d", &choice) != 1) {
            while (getchar() != '\n');
            printf("\nInvalid input! Please enter a number between 1 and 4.\n\n");
            continue;
        }

        if (choice == 4) {
            printf("\nExiting Stress Testing Tool. Goodbye!\n");
            break;
        }

        int duration = 10;
        int mem_mb = 256;

        switch (choice) {
            case 1:
                printf("\nEnter test duration in seconds (e.g., 10): ");
                scanf("%d", &duration);
                run_cpu_test(duration);
                break;

            case 2:
                printf("\nEnter memory size to allocate in MB (e.g., 512): ");
                scanf("%d", &mem_mb);
                printf("Enter test duration in seconds (e.g., 10): ");
                scanf("%d", &duration);
                run_memory_test(mem_mb, duration);
                break;

            case 3:
                printf("\nEnter memory size to allocate in MB (e.g., 512): ");
                scanf("%d", &mem_mb);
                printf("Enter test duration in seconds (e.g., 10): ");
                scanf("%d", &duration);
                run_combined_test(mem_mb, duration);
                break;

            default:
                printf("\nInvalid choice! Please select 1, 2, 3, or 4.\n\n");
                break;
        }
    }

    return 0;
}
