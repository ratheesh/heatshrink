#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>

#include "heatshrink_decoder.h"

#define DEF_WINDOW_SZ2 HEATSHRINK_STATIC_WINDOW_BITS
#define DEF_LOOKAHEAD_SZ2 HEATSHRINK_STATIC_LOOKAHEAD_BITS
#define DEF_DECODER_INPUT_BUFFER_SIZE HEATSHRINK_STATIC_INPUT_BUFFER_SIZE
#define DEF_BUFFER_SIZE (4 * 1024)

#if 0
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...) /* NO-OP */
#endif

#include <err.h>
#define HEATSHRINK_ERR(...) err(__VA_ARGS__)

/*
 * We have to open binary files with the O_BINARY flag on Windows. Most other
 * platforms don't differentiate between binary and non-binary files.
 */
#ifndef O_BINARY
#define O_BINARY 0
#endif

static const int version_major = HEATSHRINK_VERSION_MAJOR;
static const int version_minor = HEATSHRINK_VERSION_MINOR;
static const int version_patch = HEATSHRINK_VERSION_PATCH;
static const char author[] = HEATSHRINK_AUTHOR;
static const char url[] = HEATSHRINK_URL;

static void usage(void) {
    fprintf(stderr, "heatshrink version %u.%u.%u by %s\n",
	version_major, version_minor, version_patch, author);
    exit(1);
}

typedef enum { IO_READ, IO_WRITE, } IO_mode;
typedef enum { OP_ENC, OP_DEC, } Operation;

typedef struct {
    int fd;                     /* file descriptor */
    IO_mode mode;
    size_t fill;                /* fill index */
    size_t read;                /* read index */
    size_t size;
    size_t total;
    uint8_t buf[];
} io_handle;

typedef struct {
    uint8_t window_sz2;
    uint8_t lookahead_sz2;
    size_t decoder_input_buffer_size;
    size_t buffer_size;
    uint8_t verbose;
    Operation cmd;
    char *in_fname;
    char *out_fname;
    io_handle *in;
    io_handle *out;
} config;

static void die(char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static void report(config *cfg);

/* Open an IO handle. Returns NULL on error. */
static unsigned char _z_IOR_Handle[sizeof(io_handle) + DEF_BUFFER_SIZE];
static unsigned char _z_IOW_Handle[sizeof(io_handle) + DEF_BUFFER_SIZE];
static io_handle *handle_open(char *fname, IO_mode m, size_t buf_sz) {
    io_handle *io = NULL;
    /* io = malloc(sizeof(*io) + buf_sz); */
    if (m == IO_READ) {
	    io = (io_handle *)&_z_IOR_Handle[0];
	    if (io == NULL) { return NULL; }
	    memset(io, 0, sizeof(_z_IOR_Handle));
    }

    if (m == IO_WRITE) {
	    io = (io_handle *)&_z_IOW_Handle[0];
	    if (io == NULL) { return NULL; }
	    memset(io, 0, sizeof(_z_IOW_Handle));
    }

    io->fd = -1;
    io->size = buf_sz;
    io->mode = m;

    if (m == IO_READ) {
	if (0 == strcmp("-", fname)) {
	    io->fd = STDIN_FILENO;
	} else {
	    io->fd = open(fname, O_RDONLY | O_BINARY);
	}
    } else if (m == IO_WRITE) {
	if (0 == strcmp("-", fname)) {
	    io->fd = STDOUT_FILENO;
	} else {
	    io->fd = open(fname, O_WRONLY | O_BINARY | O_CREAT | O_TRUNC /*| O_EXCL*/, 0644);
	}
    }

    if (io->fd == -1) {         /* failed to open */
//free(io);
	HEATSHRINK_ERR(1, "open");
	return NULL;
    }

    return io;
}

/* Read SIZE bytes from an IO handle and return a pointer to the content.
 * BUF contains at least size_t bytes. Returns 0 on EOF, -1 on error. */
static ssize_t handle_read(io_handle *io, size_t size, uint8_t **buf) {
    LOG("@ read %zd\n", size);
    if (buf == NULL) { return -1; }
    if (size > io->size) {
	fprintf(stderr, "size %zd, io->size %zd\n", size, io->size);
	return -1;
    }
    if (io->mode != IO_READ) { return -1; }

    size_t rem = io->fill - io->read;
    if (rem >= size) {
	*buf = &io->buf[io->read];
	return size;
    } else {                    /* read and replenish */
	if (io->fd == -1) {     /* already closed, return what we've got */
	    *buf = &io->buf[io->read];
	    return rem;
	}

	memmove(io->buf, &io->buf[io->read], rem);
	io->fill -= io->read;
	io->read = 0;

	/* read from flash here */
	ssize_t read_sz = read(io->fd, &io->buf[io->fill], io->size - io->fill);

	if (read_sz < 0) { HEATSHRINK_ERR(1, "read"); }
	io->total += read_sz;
	if (read_sz == 0) {     /* EOF */
	    if (close(io->fd) < 0) { HEATSHRINK_ERR(1, "close"); }
	    io->fd = -1;
	}
	io->fill += read_sz;
	*buf = io->buf;
	return io->fill > size ? size : io->fill;
    }
}

/* Drop the oldest SIZE bytes from the buffer. Returns <0 on error. */
static int handle_drop(io_handle *io, size_t size) {
    LOG("@ drop %zd\n", size);
    if (io->read + size <= io->fill) {
	io->read += size;
    } else {
	return -1;
    }
    if (io->read == io->fill) {
	io->read = 0;
	io->fill = 0;
    }
    return 0;
}

/* Sink SIZE bytes from INPUT into the io handle. Returns the number of
 * bytes written, or -1 on error. */
static ssize_t handle_sink(io_handle *io, size_t size, uint8_t *input) {
    LOG("@ sink %zd\n", size);
    if (size > io->size) { return -1; }
    if (io->mode != IO_WRITE) { return -1; }

    if (io->fill + size > io->size) {

	/* write io->buf to FPGA here */
	ssize_t written = write(io->fd, io->buf, io->fill);

	LOG("@ flushing %zd, wrote %zd\n", io->fill, written);
	io->total += written;
	if (written == -1) { HEATSHRINK_ERR(1, "write"); }
	memmove(io->buf, &io->buf[written], io->fill - written);
	io->fill -= written;
    }
    memcpy(&io->buf[io->fill], input, size);
    io->fill += size;
    return size;
}

static void handle_close(io_handle *io) {
    if (io->fd != -1) {
	if (io->mode == IO_WRITE) {

	    /* write io->buf to FPGA here */
	    ssize_t written = write(io->fd, io->buf, io->fill);
	    io->total += written;
	    LOG("@ close: flushing %zd, wrote %zd\n", io->fill, written);
	    if (written == -1) { HEATSHRINK_ERR(1, "write"); }
	}
	close(io->fd);
	io->fd = -1;
    }
}

static void close_and_report(config *cfg) {
    handle_close(cfg->in);
    handle_close(cfg->out);
    if (cfg->verbose) { report(cfg); }
//free(cfg->in);
//    free(cfg->out);
}

static int decoder_sink_read(config *cfg, heatshrink_decoder *hsd,
	uint8_t *data, size_t data_sz) {
    io_handle *out = cfg->out;
    size_t sink_sz = 0;
    size_t poll_sz = 0;
    size_t out_sz = 4096;
    uint8_t out_buf[out_sz];
    memset(out_buf, 0, out_sz);

    HSD_sink_res sres;
    HSD_poll_res pres;
    HSD_finish_res fres;

    size_t sunk = 0;
    do {
	if (data_sz > 0) {
	    sres = heatshrink_decoder_sink(hsd, &data[sunk], data_sz - sunk, &sink_sz);
	    if (sres < 0) { die("sink"); }
	    sunk += sink_sz;
	}

	do {
	    pres = heatshrink_decoder_poll(hsd, out_buf, out_sz, &poll_sz);
	    if (pres < 0) { die("poll"); }
	    if (handle_sink(out, poll_sz, out_buf) < 0) die("handle_sink");
	} while (pres == HSDR_POLL_MORE);

	if (data_sz == 0 && poll_sz == 0) {
	    fres = heatshrink_decoder_finish(hsd);
	    if (fres < 0) { die("finish"); }
	    if (fres == HSDR_FINISH_DONE) { return 1; }
	}
    } while (sunk < data_sz);

    return 0;
}

static int decode(config *cfg) {
    uint8_t window_sz2 = cfg->window_sz2;
    size_t window_sz = 1 << window_sz2;
    size_t ibs = cfg->decoder_input_buffer_size;
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(ibs,
	window_sz2, cfg->lookahead_sz2);

    ssize_t read_sz = 0;

    io_handle *in = cfg->in;

    HSD_finish_res fres;

    /* Process input until end of stream */
    while (1) {
	uint8_t *input = NULL;
	read_sz = handle_read(in, window_sz, &input);
	if (input == NULL) {
	    fprintf(stderr, "handle read failure\n");
	    die("read");
	}
	if (read_sz == 0) {
	    fres = heatshrink_decoder_finish(hsd);
	    if (fres < 0) { die("finish"); }
	    if (fres == HSDR_FINISH_DONE) break;
	} else if (read_sz < 0) {
	    die("read");
	} else {
	    if (decoder_sink_read(cfg, hsd, input, read_sz)) { break; }
	    if (handle_drop(in, read_sz) < 0) { die("drop"); }
	}
    }
    if (read_sz == -1) { HEATSHRINK_ERR(1, "read"); }

    /* heatshrink_decoder_free(hsd); */
    close_and_report(cfg);
    return 0;
}

static void report(config *cfg) {
    size_t inb = cfg->in->total;
    size_t outb = cfg->out->total;
    fprintf(cfg->out->fd == STDOUT_FILENO ? stderr : stdout,
	"%s %0.2f %%\t %zd -> %zd (-w %u -l %u)\n",
	cfg->in_fname, 100.0 - (100.0 * outb) / inb, inb, outb,
	cfg->window_sz2, cfg->lookahead_sz2);
}

static void proc_args(config *cfg, int argc, char **argv) {
    cfg->window_sz2 = DEF_WINDOW_SZ2;
    cfg->lookahead_sz2 = DEF_LOOKAHEAD_SZ2;
    cfg->buffer_size = DEF_BUFFER_SIZE;
    cfg->decoder_input_buffer_size = DEF_DECODER_INPUT_BUFFER_SIZE;
    cfg->cmd = OP_DEC;
    cfg->verbose = 0;
    cfg->in_fname = "-";
    cfg->out_fname = "-";

    /* for testing only */
    if (argc > 1)
	cfg->in_fname = argv[1];
    if (argc > 2)
	cfg->out_fname = argv[2];
}

int main(int argc, char **argv) {
    config cfg;
    memset(&cfg, 0, sizeof(cfg));
    proc_args(&cfg, argc, argv);

    if (0 == strcmp(cfg.in_fname, cfg.out_fname)
	&& (0 != strcmp("-", cfg.in_fname))) {
	fprintf(stderr, "Refusing to overwrite file '%s' with itself.\n", cfg.in_fname);
	exit(1);
    }

    cfg.in = handle_open(cfg.in_fname, IO_READ, cfg.buffer_size);
    if (cfg.in == NULL) { die("Failed to open input file for read"); }
    cfg.out = handle_open(cfg.out_fname, IO_WRITE, cfg.buffer_size);
    if (cfg.out == NULL) { die("Failed to open output file for write"); }

    if (cfg.cmd == OP_ENC) {
	    printf("Not supported!\n");
	    return -1;
    } else if (cfg.cmd == OP_DEC) {
	return decode(&cfg);
    } else {
	usage();
    }
}
