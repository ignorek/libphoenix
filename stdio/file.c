/*
 * Phoenix-RTOS
 *
 * libphoenix
 *
 * fs.c
 *
 * Copyright 2017, 2018 Phoenix Systems
 * Author: Aleksander Kaminski, Jan Sikorski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/msg.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/threads.h>

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>


#define F_EOF (1 << 0)
#define F_WRITING (1 << 1)
#define F_LINE (1 << 2)
#define F_ERROR (1 << 3)

static FILE stdin_file  = {0, 0};
static FILE stdout_file = {1, 0};
static FILE stderr_file = {2, 0};
FILE *stdin, *stdout, *stderr;


static int string2mode(const char *mode)
{
	int next_char = 1;

	if (mode[0] == 'r') {
		if (mode[next_char] == 'b')
			next_char += 1;

		if (mode[next_char] == '\0')
			return O_RDONLY;
		else if (mode[next_char] == '+')
			return O_RDWR;
		else
			return -1;
	} else if (mode[0] == 'w') {
		if (mode[next_char] == 'b')
			next_char += 1;

		if (mode[next_char] == '\0')
			return O_WRONLY | O_CREAT | O_TRUNC;
		else if (mode[next_char] == '+')
			return O_RDWR | O_CREAT | O_TRUNC;
		else
			return -1;
	} else if (mode[0] == 'a') {
		if (mode[next_char] == 'b')
			next_char += 1;

		if (mode[next_char] == '\0')
			return O_APPEND | O_CREAT;
		else if (mode[next_char] == '+')
			return O_RDWR | O_CREAT | O_APPEND;
		else
			return -1;
	}

	return -1;
}


static void *buffAlloc(void)
{
	void *ret;

#ifndef NOMMU
	if ((ret = mmap(NULL, BUFSIZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, NULL, 0)) == MAP_FAILED)
		return NULL;
#else
	ret = malloc(BUFSIZ);
#endif

	return ret;
}


static void buffFree(void *ptr)
{
#ifndef NOMMU
	munmap(ptr, BUFSIZ);
#else
	free(ptr);
#endif
}




int fclose(FILE *file)
{
	int err;

	if (file == NULL)
		return -EINVAL;

	fflush(file);
	err = close(file->fd);

	if (file->buffer != NULL)
		buffFree(file->buffer);

	resourceDestroy(file->lock);
	free(file);

	return err;
}


FILE *fopen(const char *filename, const char *mode)
{
	int m;
	FILE *f;
	int fd;

	if (filename == NULL || mode == NULL)
		return NULL;

	if ((m = string2mode(mode)) < 0)
		return NULL;

	if ((fd = open(filename, m, DEFFILEMODE)) < 0)
		return NULL;

	if ((f = calloc(1, sizeof(FILE))) == NULL)
		return NULL;

	if ((f->buffer = buffAlloc()) == NULL) {
		close(fd);
		free(f);
		return NULL;
	}

	mutexCreate(&f->lock);
	f->fd = fd;
	fflush(f);
	return f;
}


FILE *fdopen(int fd, const char *mode)
{
//	unsigned int m;
	FILE *f;

	if (mode == NULL)
		return NULL;

#if 0 /* TODO: check if mode is compatible with fd's mode */
	if ((m = string2mode(mode)) < 0)
		return NULL;
#endif

	if ((f = calloc(1, sizeof(FILE))) == NULL)
		return NULL;

	if ((f->buffer = buffAlloc()) == NULL) {
		close(fd);
		free(f);
		return NULL;
	}

	mutexCreate(&f->lock);
	f->fd = fd;
	fflush(f);
	return f;
}


FILE *freopen(const char *pathname, const char *mode, FILE *stream)
{
	int m;

	if (mode == NULL || stream == NULL)
		return NULL;

	fflush(stream);

	if (pathname != NULL) {
		close(stream->fd);
		if ((m = string2mode(mode)) < 0)
			return NULL;

		stream->fd = open(pathname, m);
	}
	else {
		/* change mode */
	}

	return stream;
}


static int full_read(int fd, void *ptr, size_t size)
{
	int err;
	int total = 0;

	while (size) {
		if ((err = read(fd, ptr, size)) < 0)
			return -1;
		else if (!err)
			return total;
		ptr += err;
		total += err;
		size -= err;
	}

	return total;
}


static int full_write(int fd, const void *ptr, size_t size)
{
	int err;
	while (size) {
		if ((err = write(fd, ptr, size)) < 0)
			return -1;
		ptr += err;
		size -= err;
	}

	return size;
}


size_t fread_unlocked(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	int err;
	size_t readsz = nmemb * size;
	size_t bytes;

	if (!readsz || (stream->flags & F_EOF))
		return 0;

	if (stream->buffer == NULL) {
		/* unbuffered read */
		if ((err = full_read(stream->fd, ptr, readsz)) < 0)
			return 0;

		return err / size;
	}

	/* flush write buffer if writing */
	if (stream->flags & F_WRITING) {
		if (fflush_unlocked(stream) < 0)
			return 0;

		stream->flags &= ~F_WRITING;
		stream->bufpos = stream->bufeof = BUFSIZ;
	}

	/* refill read buffer */
	if (stream->bufpos == stream->bufeof) {
		if ((err = read(stream->fd, stream->buffer, BUFSIZ)) == -1)
			return 0;

		stream->bufpos = 0;
		stream->bufeof = err;

		if (!err) {
			stream->flags |= F_EOF;
			return 0;
		}
	}

	if ((bytes = stream->bufeof - stream->bufpos)) {
		bytes = min(bytes, readsz);
		memcpy(ptr, stream->buffer + stream->bufpos, bytes);

		ptr += bytes;
		stream->bufpos += bytes;
		readsz -= bytes;

		if (!readsz)
			return nmemb;
	}

	/* read remainder directly into ptr */
	if ((err = full_read(stream->fd, ptr, readsz)) != -1) {
		bytes += err;

		if (err < readsz)
			stream->flags |= F_EOF;
	}
	else {
		stream->flags |= F_ERROR;
		return 0;
	}

	return bytes / size;
}


size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret;
	mutexLock(stream->lock);
	ret = fread_unlocked(ptr, size, nmemb, stream);
	mutexUnlock(stream->lock);
	return ret;
}


size_t fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	int err;
	size_t writesz = nmemb * size;
	char *nl;

	if (!writesz)
		return 0;

	if (stream->buffer == NULL) {
		/* unbuffered write */
		if ((err = write(stream->fd, ptr, writesz)) < 0)
			return 0;

		return err / size;
	}

	/* discard reading buffer */
	if (!(stream->flags & F_WRITING)) {
		fflush_unlocked(stream);
		stream->flags |= F_WRITING;
		stream->bufpos = 0;
	}

	/* write to buffer if fits */
	if ((BUFSIZ - stream->bufpos) > writesz) {
		memcpy(stream->buffer + stream->bufpos, ptr, writesz);
		stream->bufpos += writesz;

		if ((stream->flags & F_LINE) && (nl = memrchr(stream->buffer, '\n', stream->bufpos)) != NULL) {
			if ((err = full_write(stream->fd, stream->buffer, nl - stream->buffer + 1)) < 0)
				return 0;

			stream->bufpos -= nl - stream->buffer + 1;
			memmove(stream->buffer, nl + 1, stream->bufpos);
		}

		return nmemb;
	}

	/* flush buffer and write to file */
	if ((err = write(stream->fd, stream->buffer, stream->bufpos)) == -1)
		return 0;

	stream->bufpos = 0;

	if ((err = full_write(stream->fd, ptr, writesz)) == -1)
		return 0;

	return nmemb;
}


size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	size_t ret;
	mutexLock(stream->lock);
	ret = fwrite_unlocked(ptr, size, nmemb, stream);
	mutexUnlock(stream->lock);
	return ret;
}


int fgetc_unlocked(FILE *stream)
{
	unsigned char c;

	if (fread_unlocked(&c, 1, 1, stream) != 1)
		return EOF;

	return c;
}


int fgetc(FILE *stream)
{
	int ret;
	mutexLock(stream->lock);
	ret = fgetc_unlocked(stream);
	mutexUnlock(stream->lock);
	return ret;
}


int fputc_unlocked(int c, FILE *stream)
{
	if (fwrite_unlocked(&c, 1, 1, stream) != 1)
		return EOF;

	return c;
}


int fputc(int c, FILE *stream)
{
	int ret;
	mutexLock(stream->lock);
	ret = fputc_unlocked(c, stream);
	mutexUnlock(stream->lock);
	return ret;
}


char *fgets_unlocked(char *str, int n, FILE *stream)
{
	int c, i = 0;
	while ((c = fgetc_unlocked(stream)) != EOF) {
		str[i++] = c;
		if (c == '\n' || i == n - 1)
			break;
	}

	if (i)
		str[i] = 0;
	else
		return NULL;

	return str;
}


char *fgets(char *str, int n, FILE *stream)
{
	char *ret;
	mutexLock(stream->lock);
	ret = fgets_unlocked(str, n, stream);
	mutexUnlock(stream->lock);
	return ret;
}


int fflush_unlocked(FILE *stream)
{
	if (stream == NULL) {
		fflush_unlocked(stdout);
		return 0; /* TODO: flush all FILE's */
	}

	if (stream->flags & F_WRITING) {
		if (stream->bufpos) {
			full_write(stream->fd, stream->buffer, stream->bufpos);
			stream->bufpos = 0;
		}
	}
	else {
		lseek(stream->fd, stream->bufpos - stream->bufeof, SEEK_CUR);
		stream->bufpos = stream->bufeof = BUFSIZ;
	}

	return 0;
}


int fflush(FILE *stream)
{
	int ret;

	if (stream == NULL)
		return fflush(stdout);

	mutexLock(stream->lock);
	ret = fflush_unlocked(stream);
	mutexUnlock(stream->lock);
	return ret;
}


int fseek_unlocked(FILE *stream, long offset, int whence)
{
	fflush_unlocked(stream);
	stream->flags &= ~F_EOF;
	return lseek(stream->fd, offset, whence);
}


int fseek(FILE *stream, long offset, int whence)
{
	int ret;
	mutexLock(stream->lock);
	ret = fseek_unlocked(stream, offset, whence);
	mutexUnlock(stream->lock);
	return ret;
}


long int ftell_unlocked(FILE *stream)
{
	return (long)fseek_unlocked(stream, 0, SEEK_CUR);
}


long int ftell(FILE *stream)
{
	long int ret;
	mutexLock(stream->lock);
	ret = ftell_unlocked(stream);
	mutexUnlock(stream->lock);
	return ret;
}


off_t ftello_unlocked(FILE *stream)
{
	return (off_t)fseek_unlocked(stream, 0, SEEK_CUR);
}


off_t ftello(FILE *stream)
{
	off_t ret;
	mutexLock(stream->lock);
	ret = ftello_unlocked(stream);
	mutexUnlock(stream->lock);
	return ret;
}


int fileno(FILE *stream)
{
	return stream->fd;
}


int fileno_unlocked(FILE *stream)
{
	return stream->fd;
}


int getc_unlocked(FILE *stream)
{
	return fgetc_unlocked(stream);
}


int getc(FILE *stream)
{
	int ret;
	mutexLock(stream->lock);
	ret = getc_unlocked(stream);
	mutexUnlock(stream->lock);
	return ret;
}


int putc_unlocked(int c, FILE *stream)
{
	char cc = c;
	fwrite_unlocked(&cc, 1, 1, stream);
	return c;
}


int putc(int c, FILE *stream)
{
	int ret;
	mutexLock(stream->lock);
	ret = putc_unlocked(c, stream);
	mutexUnlock(stream->lock);
	return ret;
}


int putchar_unlocked(int c)
{
	char cc = c;
	fwrite_unlocked(&cc, 1, 1, stdout);
	return c;
}


int putchar(int c)
{
	int ret;
	mutexLock(stdout->lock);
	ret = putchar_unlocked(c);
	mutexUnlock(stdout->lock);
	return ret;
}


int ungetc_unlocked(int c, FILE *stream)
{
	/* TODO: The file-position indicator is decremented by each successful call to ungetc(); */
	if (c == EOF)
		return EOF;

	/* flush write buffer if writing */
	if (stream->flags & F_WRITING) {
		if (fflush(stream) < 0)
			return EOF;

		stream->flags &= ~F_WRITING;
	}

	if (!stream->bufpos)
		return EOF;

	stream->buffer[--stream->bufpos] = c;
	stream->flags &= ~F_EOF;
	return c;
}


int ungetc(int c, FILE *stream)
{
	int ret;
	mutexLock(stream->lock);
	ret = ungetc_unlocked(c, stream);
	mutexUnlock(stream->lock);
	return ret;
}


int puts_unlocked(const char *s)
{
	int len = strlen(s), l = 0, err;

	while (l < len) {
		if ((err = fwrite_unlocked((void *)s + l, 1, len - l, stdout)) < 0)
			return -1;
		l += err;
	}
	putchar_unlocked('\n');

	return l;
}


int puts(const char *s)
{
	int ret;
	mutexLock(stdout->lock);
	ret = puts_unlocked(s);
	mutexUnlock(stdout->lock);
	return ret;
}


int fputs_unlocked(const char *s, FILE *stream)
{
	int len = strlen(s);
	return fwrite_unlocked(s, 1, len, stream);
}


int fputs(const char *s, FILE *stream)
{
	int ret;
	mutexLock(stream->lock);
	ret = fputs_unlocked(s, stream);
	mutexUnlock(stream->lock);
	return ret;
}


int ferror_unlocked(FILE *stream)
{
	return !!(stream->flags & F_ERROR);
}


int ferror(FILE *stream)
{
	return ferror_unlocked(stream);
}


void clearerr(FILE *stream)
{
	stream->flags &= ~F_ERROR;
}


int getchar_unlocked(void)
{
	return getc_unlocked(stdin);
}


int getchar(void)
{
	int ret;
	mutexLock(stdin->lock);
	ret = getc_unlocked(stdin);
	mutexUnlock(stdin->lock);
	return ret;
}


void rewind(FILE *file)
{
	fseek(file, 0, SEEK_SET);
}


int fseeko(FILE *stream, off_t offset, int whence)
{
	return fseek(stream, offset, whence);
}


void __fpurge(FILE *stream)
{
}


ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
	char buff[128] = { 0 };
	size_t linesz = 0;
	offs_t offs;
	int readsz, i, nl = 0;

	offs = (offs_t)ftell(stream);

	while ((readsz = fread(buff, 1, sizeof(buff), stream)) > 0) {
		for (i = 0; i < readsz; i++) {
			if (buff[i] == '\n') {
				linesz += i + 2;
				nl = 1;
				break;
			}
		}
		if (nl)
			break;
		linesz += readsz;
	}

	if (linesz == 0)
		return -1; // EOF

	if (!nl)
	   linesz++;

	if (*lineptr == NULL && linesz > 0)
		*lineptr = malloc(linesz);
	else if (*n < linesz)
		*lineptr = realloc(*lineptr, linesz);

	*n = linesz - 1;

	fseek(stream, offs, SEEK_SET);
	fread(*lineptr, 1, linesz - 1, stream);

	if (linesz > 0)
		(*lineptr)[linesz - 1] = 0;

	return linesz - 1;
}


int feof(FILE *stream)
{
	return !!(stream->flags & F_EOF);
}


void setbuf(FILE *stream, char *buf)
{
	/* TODO */
}


int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
	/* TODO */
	return 0;
}


void _file_init(void)
{
	stdin = &stdin_file;
	stdout = &stdout_file;
	stderr = &stderr_file;

	stdin->buffer = buffAlloc();
	stdout->buffer = buffAlloc();

	stdin->bufeof = stdin->bufpos = BUFSIZ;
	mutexCreate(&stdin->lock);

	stdout->bufpos = 0;
	stdout->flags = F_WRITING;
	mutexCreate(&stdout->lock);

	stderr->buffer = NULL;
	stderr->flags = F_WRITING;
	mutexCreate(&stderr->lock);

	if (isatty(stdout->fd))
		stdout->flags |= F_LINE;
}
