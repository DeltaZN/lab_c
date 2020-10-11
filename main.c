#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdatomic.h>
#include <linux/futex.h>
#include <syscall.h>
#include <asm/errno.h>
#include <errno.h>

#define log_level 0
#define megabyte_size 1024*1024
#define A 120
#define D 73
#define E 150
#define G 136
#define I 147
#define observe_block 1

typedef struct {
    int thread_number;
    int ints_per_thread;
    int *start;
    FILE *file;
} thread_generator_data;

typedef struct {
    int ints_per_file;
    int files;
    int *start;
    int *end;
    int *futexes;
} thread_writer_data;

typedef struct {
    int thread_number;
    int file_number;
    int *futexes;
} thread_reader_data;

static int
futex(int *uaddr, int futex_op, int val,
      const struct timespec *timeout, int *uaddr2, int val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val,
                   timeout, uaddr2, val3);
}

/* Acquire the futex pointed to by 'futexp': wait for its value to
          become 1, and then set the value to 0. */
static void
fwait(int *futexp)
{
    int s;

    /* atomic_compare_exchange_strong(ptr, oldval, newval)
       atomically performs the equivalent of:

           if (*ptr == *oldval)
               *ptr = newval;

       It returns true if the test yielded true and *ptr was updated. */

    while (1) {

        /* Is the futex available? */
        const int one = 1;
        if (atomic_compare_exchange_strong(futexp, &one, 0))
            break;      /* Yes */

        /* Futex is not available; wait */

        s = futex(futexp, FUTEX_WAIT, 0, NULL, NULL, 0);
        if (s == -1 && errno != EAGAIN)
            printf("futex-FUTEX_WAIT");
    }
}

static void
fpost(int *futexp)
{
    int s;

    /* atomic_compare_exchange_strong() was described in comments above */

    const int zero = 0;
    if (atomic_compare_exchange_strong(futexp, &zero, 1)) {
        s = futex(futexp, FUTEX_WAKE, 1, NULL, NULL, 0);
        if (s  == -1)
            printf("futex-FUTEX_WAKE");
    }
}



int read_int_from_file(FILE *file) {
    int i = 0;
    fread(&i, 4, 1, file);
    return i;
}

char* seq_read(FILE* file_ptr) {
    fseek(file_ptr, 0, SEEK_END);
    long file_size = ftell(file_ptr);
    rewind(file_ptr);
    char *buffer = (char*) malloc(file_size);
    int blocks = file_size / G;
    int last_block_size = file_size % G;
    char* buf_ptr;
    for (int i = 0; i < blocks; ++i) {
        buf_ptr = buffer + G*i;
        fread(buf_ptr, G, 1, file_ptr);
    }
    if (last_block_size > 0) {
        buf_ptr = buffer + G*blocks;
        fread(buf_ptr, last_block_size, 1, file_ptr);
    }
    return buffer;
}

void *fill_with_random(void *thread_data) {
    thread_generator_data *data = (thread_generator_data *) thread_data;
    if (log_level > 1) {
        printf("[GENERATOR-%d] started...\n", data->thread_number);
    }
    while (1) {
        for (int i = 0; i < data->ints_per_thread; i++) {
            data->start[i] = read_int_from_file(data->file);
        }
    }
    if (log_level > 1) {
        printf("[GENERATOR-%d] finished...\n", data->thread_number);
    }
    return NULL;
}

void *read_files(void *thread_data) {
    thread_reader_data *data = (thread_reader_data *) thread_data;
    if (log_level > 1) {
        printf("[READER-%d] started...\n", data->thread_number);
    }
    while (1) {
        char filename[6] = "lab1_0";
        filename[5] = '0' + data->file_number;
        FILE *file_ptr = NULL;
        while (file_ptr == NULL) {
            if (observe_block) {
                printf("[READER-%d] wait for mutex %d...\n", data->thread_number, data->file_number);
            }
            fwait(&data->futexes[data->file_number]);
            if (observe_block) {
                printf("[READER-%d] captured mutex %d!\n", data->thread_number, data->file_number);
            }
            file_ptr = fopen(filename, "rb");
            if (file_ptr == NULL) {
                fpost(&data->futexes[data->file_number]);
                if (observe_block) {
                    printf("[READER-%d] free mutex %d!\n", data->thread_number, data->file_number);
                }
                if (log_level > 2) {
                    printf("[READER-%d] I/O error on open file %s.\n", data->thread_number, filename);
                }
            }
        }
        fseek(file_ptr, 0, SEEK_END);
        long file_size = ftell(file_ptr);
        rewind(file_ptr);
        char *buffer = seq_read(file_ptr);
        fclose(file_ptr);
        fpost(&data->futexes[data->file_number]);
        if (observe_block) {
            printf("[READER-%d] free mutex %d!\n", data->thread_number, data->file_number);
        }
        int *int_buf = (int *) buffer;
        long sum = 0;
        for (int i = 0; i < file_size / 4; i++) {
            sum += int_buf[i];
        }

        if (log_level > 0) {
            printf("[READER-%d] file %s sum is %ld.\n", data->thread_number, filename, sum);
        }

        free(buffer);
    }
    return NULL;
}

void seq_write(void *ptr, int size, int n, int fd, const char* filepath) {
    struct stat fstat;
    stat(filepath, &fstat);
    int blksize = (int)fstat.st_blksize;
    int align = blksize-1;
    int bytes = size * n;
    // impossible to use G from the task because O_DIRECT flag requires aligned both the memory address and your buffer to the filesystem's block size
    int blocks = bytes / blksize;

    char *buff = (char *) malloc((int)blksize+align);
    // code from stackoverflow... wtf???
    char *wbuff = (char *)(((uintptr_t)buff+align)&~((uintptr_t)align));

    for (int i = 0; i < blocks; ++i) {
        char* buf_ptr = ptr + blksize*i;
        // copy from memory to write buffer
        for (int j = 0; j < blksize; j++) {
            buff[j] = buf_ptr[j];
        }
        if (pwrite(fd, wbuff, blksize, blksize*i) < 0) {
            free((char *)buff);
            printf("write error occurred\n");
            return;
        }
    }
    free((char *)buff);
}

void *write_to_files(void *thread_data) {
    thread_writer_data *data = (thread_writer_data *) thread_data;
    int *write_pointer = data->start;
    if (log_level > 1) {
        printf("[WRITER] started...\n");
    }
    while (1) {
        for (int i = 0; i < data->files; i++) {
            char filename[6] = "lab1_0";
            filename[5] = '0' + i;
            if (observe_block) {
                printf("[WRITER] waiting for mutex %d...\n", i);
            }
            fwait(&data->futexes[i]);
            if (observe_block) {
                printf("[WRITER] captured mutex %d\n", i);
            }
            // NOCACHE file write
            int current_file = open(filename, O_WRONLY | O_CREAT | __O_DIRECT, 00666);
            if (current_file == - 1) {
                printf("error on open file for write\n");
                fpost(&data->futexes[i]);
                return NULL;
            }
            int ints_to_file = data->ints_per_file;
            int is_done = 0;
            printf("[WRITER] write started\n");
            while (!is_done) {
                if (ints_to_file + write_pointer < data->end) {
                    seq_write(write_pointer, sizeof(int), ints_to_file, current_file, filename);
                    write_pointer += ints_to_file;
                    is_done = 1;
                } else {
                    int available = data->end - write_pointer;
                    seq_write(write_pointer, sizeof(int), available, current_file, filename);
                    write_pointer = data->start;
                    ints_to_file -= available;
                }
            }
            close(current_file);
            printf("[WRITER] write finished\n");
            fpost(&data->futexes[i]);
            if (observe_block) {
                printf("[WRITER] free mutex %d\n", i);
            }
        }
    }
    return NULL;
}

int main() {
    const char *devurandom_filename = "/dev/urandom";
    FILE *devurandom_file = fopen(devurandom_filename, "r");

    int *memory_region = malloc(A * megabyte_size);
    int *thread_data_start = memory_region;

    // generator threads start

    pthread_t *generator_threads = (pthread_t *) malloc(D * sizeof(pthread_t));
    thread_generator_data *generator_data = (thread_generator_data *) malloc(D * sizeof(thread_generator_data));

    int ints = A * 1024 * 256;
    int ints_per_thread = ints / D;
    for (int i = 0; i < D; ++i) {
        generator_data[i].thread_number = i;
        generator_data[i].ints_per_thread = ints_per_thread;
        generator_data[i].start = thread_data_start;
        generator_data[i].file = devurandom_file;
        thread_data_start += ints_per_thread;
    }
    generator_data[D - 1].ints_per_thread += ints % D;

    // generator threads end

    // writer thread start

    int files = A / E;
    if (A % E != 0) {
        files++;
    }

    int *futexes = malloc(sizeof(int) * files);
    for (int i = 0; i < files; ++i) {
        futexes[i] = 1;
    }

    thread_writer_data *writer_data = (thread_writer_data *) malloc(sizeof(thread_writer_data));
    pthread_t *thread_writer = (pthread_t *) malloc(sizeof(pthread_t));
    writer_data->ints_per_file = E * 1024 * 256;
    writer_data->files = files;
    writer_data->start = memory_region;
    writer_data->end = memory_region + ints;
    writer_data->futexes = futexes;

    // writer thread end

    // reader threads start

    pthread_t *reader_threads = (pthread_t *) malloc(I * sizeof(pthread_t));
    thread_reader_data *reader_data = (thread_reader_data *) malloc(I * sizeof(thread_reader_data));
    int file_number = 0;
    for (int i = 0; i < I; ++i) {
        if (file_number >= files) {
            file_number = 0;
        }
        reader_data[i].thread_number = i;
        reader_data[i].file_number = file_number;
        reader_data[i].futexes = futexes;
        file_number++;
    }

    // reader threads end

    for (int i = 0; i < D; ++i) {
        pthread_create(&(generator_threads[i]), NULL, fill_with_random, &generator_data[i]);
    }
    for (int i = 0; i < I; i++) {
//        pthread_join(generator_threads[i], NULL);
    }

    pthread_create(thread_writer, NULL, write_to_files, writer_data);
//    pthread_join(*thread_writer, NULL);

    for (int i = 0; i < I; ++i) {
        pthread_create(&(reader_threads[i]), NULL, read_files, &reader_data[i]);
    }
    for (int i = 0; i < I; i++) {
        pthread_join(reader_threads[i], NULL);
    }

    free(futexes);

    fclose(devurandom_file);

    free(generator_threads);
    free(generator_data);
    free(thread_writer);
    free(writer_data);
    free(reader_threads);
    free(reader_data);
    free(memory_region);
    return 0;
}
