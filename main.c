#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>
#include <errno.h>


#define LOG_DELIM " "	// Строка-разделитель для log_data

static const char size_suffix[] = "BKMGTP";

// Приводилка размеров для log_data
// double number, char suffix_counter
void make_human_like(double *number, char *suffix_counter)
{
	for (*suffix_counter = 0; *number >= 1000 && *suffix_counter < sizeof(size_suffix); ++*suffix_counter)
		*number /= 1024;
}


void unmake_human_like(size_t *res, double number, char suffix)
{
	double tmp = number;
	for (unsigned int i = 0; i < sizeof(size_suffix); ++i)
		if (suffix == size_suffix[i]) {
			for (unsigned int j = 0; j < i; ++j)
				tmp *= 1024;
		}
	*res = tmp;
}



// Считывалка размеров из строк (аргументов)
// char status, const char *string, size_t size
// status == 1 on error
char sscanf_size(const char *string, size_t *size)
{
	double tmp;
	char suffix;
	switch (sscanf(string, "%lf%c", &tmp, &suffix)) {
		case 2: unmake_human_like(size, tmp, suffix); return 0;
		case 1: *size = tmp; return 0;
		default: return 1;
	}
	return 0;
}


void log_data(char use_terminal, size_t test_id, size_t data_size, size_t block_size, double time_diff)
{
	double size = data_size,									// i
		   speed = size / time_diff,	speed_old = speed,		// j
		   bsize = block_size;									// k
	
	char i, j, k;
	
	make_human_like(&size,	&i);
	make_human_like(&speed,	&j);
	make_human_like(&bsize,	&k);
	
	if (use_terminal)	// Скорость, размер блока, размер данных, врема передачи данных
		printf("%8zu: Speed: %10.6lf%c/s,  block: %8.3lf%c,  size: %8.3lf%c,  time: %12.6lfs\n",
			   test_id, speed, size_suffix[(int)j], bsize, size_suffix[(int)k], size, size_suffix[(int)i], time_diff);
	else	// Скорость, размер блока, размер данных, врема передачи данных + всё то же в человеческом формате
		printf("%zu"LOG_DELIM"%lf"LOG_DELIM"%ld"LOG_DELIM"%ld"LOG_DELIM"%lf"LOG_DELIM"%lf%c/s"LOG_DELIM"%lf%c"LOG_DELIM"%lf%c"LOG_DELIM"%lfs\n",
			   test_id, speed_old, block_size, data_size, time_diff,	// Для автоматической обработки
			   speed, size_suffix[(int)j], bsize, size_suffix[(int)k], size, size_suffix[(int)i], time_diff);	// Для человека
}


int write_data(int fd, sem_t *sem, size_t data_size, size_t block_size)
{
	// Создание массива данных
	char data[block_size];
	
	// Отправка размера
	if (write(fd, &data_size, sizeof(data_size)) != sizeof(data_size)) {
		perror("Data size write error");
		return -1;
	}
	
	// Отправка размера блока
	if (write(fd, &block_size, sizeof(block_size)) != sizeof(block_size)) {
		perror("Block size write error");
		return -1;
	}
	
	// Ожидаем разрешения продолжить отправку
	sem_wait(sem);
	
	// Получение времени
	struct timeval write_time;
	if (gettimeofday(&write_time, NULL) < 0) {
		perror("Write time get error");
		return -2;
	}
	
	// Отправка данных
	{
		int write_status = 0;
		for (size_t total = 0; total < data_size; total += block_size) {
			char *p = data, *p_max = data + block_size;
			size_t bs = block_size;
			for (; p < p_max; p += write_status, bs -= write_status)
				if ((write_status = write(fd, p, bs)) <= 0) {
					perror("Data write error");
					return -1;
				}
		}
	}
	
	// Отправка времени
	if (write(fd, &write_time, sizeof(write_time)) != sizeof(write_time)) {
		perror("Write time write error");
		return -1;
	}
	
	return 0;
}


int read_data(int fd, sem_t *sem, size_t *data_size, size_t *block_size, struct timeval *write_time, struct timeval *read_time)
{
	int status;
	if ((status = read(fd, data_size, sizeof(*data_size))) == sizeof(*data_size)	// Чтение размера данных
		&& (status = read(fd, block_size, sizeof(*block_size))) == sizeof(*block_size)) {	// ... и размер блока
		// Создаём массив для принимаемых данных
		char data[*block_size];
		
		// Разрешаем ребёнку начать отправку
		sem_post(sem);
		
		// Получение данных
		{
			int read_status = 0;
			for (size_t total = 0; total < *data_size; total += *block_size) {
				char *p = data, *p_max = data + *block_size;
				size_t bs = *block_size;
				for (; p < p_max; p += read_status, bs -= read_status)
					if ((read_status = read(fd, p, bs)) <= 0) {
						perror("Data read error");
						return -1;
					}
			}
		}
		
		// Сохранение времени получения
		if (gettimeofday(read_time, NULL) < 0) {
			perror("Read time get error");
			return -2;
		}
		
		// Получение времени отправки
		if (read(fd, write_time, sizeof(struct timeval)) != sizeof(struct timeval)) {
			perror("Write time read error");
			return -1;
		}
	} else if (status < 0) {
		perror("Data size read error");
		return -1;
	} else if (status == 0)
		return 1;
	
	return 0;
}


int main(int argc, char **argv)
{
	// Флаг: вывод в терминал или в файл
	char use_terminal = (isatty(STDOUT_FILENO)? 1: 0);
	
	// Создание конвейера
	int fd[2];
	if (pipe(fd)) {
		perror("Pipe creation error");
		return 1;
	}
	
	
	// Создание семафора
	sem_t *sem;
	char sem_name[] = "      pipe-speed-semaphore";
	for (int i = 0; i < 1000000; ++i) {
		sprintf(sem_name, "%6dpipe-speed-semaphore", i);
		if ((sem = sem_open(sem_name, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0)) == NULL && errno != EEXIST) {
			close(fd[0]);
			close(fd[1]);
			perror("Semaphore create error");
			return 2;
		} else {
			if (use_terminal) {
				printf("Semaphore #%d opened\n", i);
				fflush(stdout);
			}
			break;
		}
	}
	
	
	int status = 0;
	pid_t pid = fork();
	if (pid == 0) {	// Код ребёнка
		close(fd[0]);
		
		// Считывание параметров
		size_t data_size = 0, lower = 0, inc = 0, upper = 0;
		if (argc != 5
			|| sscanf_size(argv[1], &data_size)
			|| sscanf_size(argv[2], &lower)
			|| sscanf_size(argv[3], &inc)
			|| sscanf_size(argv[4], &upper)) {
			fprintf(stderr, "Incorrect arguments!\n");
			goto child_end;
		}
		
		if (lower <= upper && inc != 0 && data_size != 0) {
			if (use_terminal) {
				printf("Data size: %zu,  Lower: %zu,  Inc: %zu,  Upper: %zu\n", data_size, lower, inc, upper);
				fflush(stdout);
			}
		} else goto child_end;
		
		if (lower == 0) lower += inc;
		for (; lower <= upper; lower += inc) {
			if (write_data(fd[1], sem, data_size, lower) != 0) {
				status = 1;
				break;
			}
		}
		
	child_end:
		close(fd[1]);
		sem_close(sem);
	} else {	// Код родителя
		if (pipe < 0) {
			close(fd[0]);
			close(fd[1]);
			perror("Fork error");
			return 2;
		}
		
		close(fd[1]);
		fclose(stdin);
		
		if (use_terminal) {
			printf("Parent pid: %d\nChild pid: %d\n", getpid(), pid);
			fflush(stdout);
		}
		
		size_t test_id = 0;	// Счётчик тестов
		size_t data_size, block_size;	// Размер данных, количество повторов отправки
		struct timeval write_time, read_time;
		
		
		// Получение данных
		while ((status = read_data(fd[0], sem, &data_size, &block_size, &write_time, &read_time)) == 0) {
			// Формирование и печать отчёта
			double time_diff = read_time.tv_sec - write_time.tv_sec,
				   delta = (read_time.tv_usec - write_time.tv_usec) * 1e-6;
			if (delta < 0) time_diff -= 1 + delta;
			else time_diff += delta;
			
			// Печать отчёта
			log_data(use_terminal, test_id, data_size, block_size, time_diff);
			
			++test_id;
		}
		if (status == 1) status = 0;
		else status = -status;
		
		close(fd[0]);
		sem_close(sem);
		sem_unlink(sem_name);
		
		if (use_terminal) printf("Tests completed. Have a nice day!\n");
	}
	
	return status;
}