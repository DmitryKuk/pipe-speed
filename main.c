#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>
#include <errno.h>

// Простой алгоритм (1): получаем размеры из аргументов или stdin.
// Сложный алгорит (0): получаем диапазоны: LOWER INC UPPER REPEATS.
#define SIMPLE_ALGO 0


sem_t *sem;


#define LOG_DELIM " "	// Строка-разделитель для log_data

// Приводилка размеров
#define MAKE_HUMAN_LIKE(number, counter) \
	do { \
		for (counter = 0; number >= 1000 && counter < sizeof(size_suffix); ++counter) number /= 1024; \
	} while (0)


void log_data(char use_terminal, size_t test_id, size_t data_size, double time_diff, double total_size, double total_time)
{
	double size = data_size,									// i
		   speed = size / time_diff,	speed_old = speed,		// j
		   tsize = total_size,									// k
		   tspeed = tsize / total_time,	tspeed_old = tspeed;	// t
	
	static const char size_suffix[] = "\0KMGP";	// B, KB, MB, GB, PB
	char i, j, k, t;
	
	MAKE_HUMAN_LIKE(size,	i);
	MAKE_HUMAN_LIKE(speed,	j);
	MAKE_HUMAN_LIKE(tsize,	k);
	MAKE_HUMAN_LIKE(tspeed,	t);
	
	if (use_terminal)
		printf("%8zu: Speed: %9.6lf%cB/s,  time: %10.6lfs,  size: %7.3lf%cB\nTotal:    Speed: %9.6lf%cB/s,  time: %10.6lfs,  size: %7.3lf%cB\n\n",
			   test_id, speed, size_suffix[(int)j], time_diff, size, size_suffix[(int)i],	// Текущий test
			   tspeed, size_suffix[(int)t], total_time, tsize, size_suffix[(int)k]);		// Общее
	else
		printf("%zu"LOG_DELIM"%zu"LOG_DELIM"%lf"LOG_DELIM"%lf"LOG_DELIM"%lf"LOG_DELIM"%lf"LOG_DELIM"%lf"LOG_DELIM"%lf%cB"LOG_DELIM"%lf%cB/s"LOG_DELIM"%lf%cB"LOG_DELIM"%lf%cB/s\n",
			   test_id, data_size, speed_old, time_diff,
			   total_size, tspeed_old, total_time,
			   size, size_suffix[(int)i], speed, size_suffix[(int)j],
			   tsize, size_suffix[(int)k], tspeed, size_suffix[(int)t]);
}


int write_data(int fd, size_t data_size)
{
	// Создание массива данных
	char data[data_size];
	
	// Отправка размера
	if (write(fd, &data_size, sizeof(data_size)) != sizeof(data_size)) {
		perror("Data size write error");
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
		int write_status;
		for (char *p = data, *p_max = data + data_size; p < p_max; p += write_status, data_size -= write_status) {
			if ((write_status = write(fd, p, data_size)) < 0) {
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


int read_data(int fd, size_t *data_size, struct timeval *write_time, struct timeval *read_time)
{
	int status;
	if ((status = read(fd, data_size, sizeof(size_t))) == sizeof(size_t)) {
		char data[*data_size];
		
		// Разрешаем ребёнку начать отправку
		sem_post(sem);
		
		// Получение данных
		{
			int read_status;
			size_t ds = *data_size;
			
			for (char *p = data, *p_max = data + *data_size; p < p_max; p += read_status, ds -= read_status)
				if ((read_status = read(fd, p, ds)) < 0) {
					perror("Data read error");
					return -1;
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
	}
	
	if (status < 0) {
		perror("Data size read error");
		return -1;
	}
	
	if (status == 0) return 1;
	
	return 0;
}


int main(int argc, const char **argv)
{
	// Флаги: использование stdin или argv для размеров и вывод в терминал или в файл
	char use_stdin = ((argc < 2)? 1: 0);
	char use_terminal = (isatty(STDOUT_FILENO)? 1: 0);
	
	// Создание конвейера
	int fd[2];
	if (pipe(fd)) {
		perror("Pipe creation error");
		return 1;
	}
	
	
	// Создание семафора
	char sem_name[] = "      pipe-speed-semaphore";
	for (int i = 0; i < 1000000; ++i) {
		sprintf(sem_name, "%6dpipe-speed-semaphore", i);
		if ((sem = sem_open(sem_name, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR, 0)) == NULL && errno != EEXIST) {
			close(fd[0]);
			close(fd[1]);
			perror("Semaphore create error");
			return 2;
		} else {
			if (use_terminal) printf("Semaphore #%d opened\n", i);
			break;
		}
	}
	
	
	int status = 0;
	pid_t pid = fork();
	if (pid == 0) {	// Код ребёнка
		close(fd[0]);
		fclose(stdout);
		
		if (use_terminal) {
			printf("Child pid: %d\n", getpid());
			fflush(stdout);
		}
		
		
#if !defined(SIMPLE_ALGO) || SIMPLE_ALGO == 1
		for (int i = 1; use_stdin || i < argc; ++i) {
			// Получение размера данных для тестирования
			size_t data_size = 0;
			if (use_stdin) {
				scanf("%zu", &data_size);
				if (feof(stdin)) break;
			} else
				sscanf(argv[i], "%zu", &data_size);
			
			if (data_size == 0) continue;
			
			if (write_data(fd[1], data_size) != 0) {
				status = 1;
				break;
			}
		}
#else
		size_t lower = 0, inc = 0, upper = 0, repeats = 1;
		if (use_stdin) {
			switch (scanf("%zu%zu%zu%zu", &lower, &inc, &upper, &repeats)) {
			case 4:
				break;
			case 3:
				repeats = 1;
				break;
			default:
				fprintf(stderr, "Incorrect input!\n");
				goto child_end;
				break;
			}
		} else {
			switch (argc) {
			case 5:
				if (sscanf(argv[4], "%zu", &repeats) != 1) {
					fprintf(stderr, "Incorrect arguments!\n");
					goto child_end;
				}
				// no break here!
			case 4:
				if (sscanf(argv[1], "%zu", &lower) != 1
						|| sscanf(argv[2], "%zu", &inc) != 1
						|| sscanf(argv[3], "%zu", &upper) != 1) {
					fprintf(stderr, "Incorrect arguments!\n");
					goto child_end;
				}
				break;
			default:
				fprintf(stderr, "Incorrect arguments!\n");
				goto child_end;
				break;
			}
		}
		
		if (lower <= upper && inc != 0 && repeats != 0) {
			if (use_terminal) {
				printf("Lower: %zu,  Inc: %zu,  Upper: %zu,  Repeats: %zu\n", lower, inc, upper, repeats);
				fflush(stdout);
			}
		} else goto child_end;
		
		if (lower == 0) lower += inc;
		for (; lower < upper; lower += inc)
			for (size_t i = 0; i < repeats; ++i)
				if (write_data(fd[1], lower) != 0) {
					status = 1;
					break;
				}
	child_end:
#endif	// SIMPLE_ALGO
		
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
		
		if (use_terminal) {
			printf("Parent pid: %d\n", pid);
			fflush(stdout);
		}
		
		size_t test_id = 0;	// Счётчик тестов
		size_t data_size;
		double total_size = 0, total_time = 0;
		struct timeval write_time, read_time;
		
		
		// Получение данных
		while ((status = read_data(fd[0], &data_size, &write_time, &read_time)) == 0) {
			// Формирование и печать отчёта
			double time_diff = read_time.tv_sec - write_time.tv_sec,
				   delta = (read_time.tv_usec >= write_time.tv_usec) * 1e-6;
			if (delta < 0) time_diff -= 1 + delta;
			else time_diff += delta;
			
			// Усреднённая информация
			total_size += data_size;
			total_time += time_diff;
			
			// Печать отчёта
			log_data(use_terminal, test_id, data_size, time_diff, total_size, total_time);
			
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