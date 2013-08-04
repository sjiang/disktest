/*
 * disktest - a quick and dirty multithreaded read/write seek test
 *
 * Author: Steve Jiang (@sjiang) email: gh at iamsteve.com
 *
 */


#include <stdio.h>
#include <sys/types.h>

#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEBUG(msg, args...) do {if (g_debug) fprintf(stderr, "[%s] " msg "\n", __FUNCTION__ , ##args );} while (0)
#define LOG(msg, args...) do { printf("[%s] " msg "\n", __FUNCTION__ , ##args ); } while (0)
#define ERROR(msg, args...) do {fprintf(stderr, "[%s] " msg "\n", __FUNCTION__ , ##args );} while (0)

#ifndef ULLONG_MAX
#define ULLONG_MAX (18446744073709551615ULL)
#endif

static int g_debug;

typedef struct testconf_ {
  int die_read;
  int die_write;
  int direct;
  off_t block_size;
  off_t byte_offset;
  size_t size;
  int read_threads;
  int write_threads;
  unsigned long long time_deadline;
  unsigned long total_reads;
  unsigned long reads_remaining;
  unsigned long total_writes;
  unsigned long writes_remaining;
  off_t seek_limit_blocks;
  char* write_buffer;
} test_config;

typedef struct wctx_ {
  int fd;
  unsigned long *results;
  unsigned long results_size;
  unsigned long ops_done;
  off_t min_seek_byte;
  off_t max_seek_byte;
  unsigned long long start;
  unsigned long long end;
} test_context;


test_config* gconf = NULL;

void usage()
{
  fprintf(stderr, "usage: disktest [options] [-rwRWt] <device or file>\n"
          "\t-b [bytes]      block size (default 4K)\n"
          "\t-o [bytes]      offset into block (default 0)\n"
          "\t-s [bytes]      size to read/write (default 4K)\n"
          "\t-r [threads]    num read threads\n"
          "\t-w [threads]    num write threads\n"
          "\t-R [count]      total number of reads to do (across all threads)\n"
          "\t-W [count]      total number of writes to do (across all threads)\n"
          "\t-t [seconds]    time to run\n"
          "\t-S [bytes]      seek range limit (default is the size of the file/device)\n"
          "\t-d              debug on\n"
          "\t-D              use O_DIRECT (bypass page cache)\n"
          "\t-h              print this help\n");
}

unsigned long long timeval_to_usec(struct timeval* val) {
  unsigned long long ret = val->tv_usec;
  ret += (unsigned long long)val->tv_sec * 1000000ULL;
  return ret;
}

void reset_conf(test_config* conf) {
  conf->die_read = 0;
  conf->die_write = 0;
  conf->direct = 0;
  conf->block_size = 4096;
  conf->byte_offset = 0;
  conf->size = 4096;
  conf->read_threads = 0;
  conf->write_threads = 0;
  conf->time_deadline = ULLONG_MAX;
  conf->total_reads = 0;
  conf->reads_remaining = 0;
  conf->total_writes = 0;
  conf->writes_remaining = 0;
  conf->write_buffer = NULL;
}

void reset_context(test_context* tc) {
  tc->ops_done = 0;
  tc->results_size = 1000;
  tc->results = (unsigned long*)malloc(sizeof(unsigned long) * tc->results_size);
  tc->max_seek_byte = 0;
  tc->min_seek_byte = LLONG_MAX;
  tc->start = 0;
  tc->end = 0;
  tc->fd = -1;
}

void* alloc_aligned(size_t size) {
    int rc;
    void* aligned = NULL;
    size_t aligned_size = sizeof(void*);
    while (aligned_size < size) {
      aligned_size = aligned_size << 1;
      if (aligned_size & (1UL << (8*sizeof(size_t)-2))) {
	ERROR("memalign size too large");
	return NULL;
      }
    }
    if((rc = posix_memalign(&aligned, sysconf(_SC_PAGESIZE), aligned_size)) != 0
       || !aligned) {
      ERROR("Failed to allocate aligned buffer: %d", rc);
      return NULL;
    }
    return aligned;
}

void add_result(test_context* tc, unsigned long idx, unsigned long value) {
  while (idx >= tc->results_size) {
    tc->results_size *= 2;
    tc->results = (unsigned long*)realloc(tc->results, sizeof(unsigned long)*tc->results_size);
  }
  tc->results[idx] = value;
}

void* read_worker(void* data) {
  struct timeval tv;
  off_t block, start;
  unsigned long long remaining;
  unsigned long long op_time;
  test_context *tc = (test_context*)data;
  char* read_buf = NULL;
  char strerrbuf[256];
  memset(strerrbuf, 0, sizeof(strerrbuf));

  read_buf = (char*)alloc_aligned(gconf->size);
  if (!read_buf) {
    return NULL;
  }

  gettimeofday(&tv, NULL);
  tc->start = timeval_to_usec(&tv);

  while(1) {
    if (gconf->die_read) {
      break;
    }
    remaining = __sync_sub_and_fetch(&gconf->reads_remaining, 1);
    if (!remaining) {
      gconf->die_read = 1;
    }
    gettimeofday(&tv, NULL);
    op_time = timeval_to_usec(&tv);
    if (op_time >= gconf->time_deadline) break;

    block = (off_t)(labs(random())) % (off_t)(gconf->seek_limit_blocks+1);
    start = block * gconf->block_size + gconf->byte_offset;
    if (tc->min_seek_byte > start) tc->min_seek_byte = start;
    if (tc->max_seek_byte < start) tc->max_seek_byte = start;

    if (lseek(tc->fd, start, SEEK_SET) < 0) {
      strerror_r(errno, strerrbuf, sizeof(strerrbuf));
      ERROR("Failed to seek to %lld: %s (%d)", (long long)start, strerrbuf, errno);
      break;
    }
    if (read(tc->fd, read_buf, gconf->size) < 0) {
      strerror_r(errno, strerrbuf, sizeof(strerrbuf));
      ERROR("Failed to read %lu bytes at %lld: %s (%d)", gconf->size, (long long)start, strerrbuf, errno);
      break;
    }
    gettimeofday(&tv, NULL);
    op_time = (timeval_to_usec(&tv) - op_time);

    add_result(tc, tc->ops_done, (unsigned long)op_time);
    tc->ops_done++;
  }

  if (read_buf) free(read_buf);
  gettimeofday(&tv, NULL);
  tc->end = timeval_to_usec(&tv);
  return NULL;
}

void* write_worker(void* data) {
  struct timeval tv;
  off_t block, start;
  unsigned long long remaining;
  unsigned long long op_time;
  test_context *tc = (test_context*)data;
  char strerrbuf[256];
  memset(strerrbuf, 0, sizeof(strerrbuf));

  gettimeofday(&tv, NULL);
  tc->start = timeval_to_usec(&tv);

  while(1) {
    if (gconf->die_write) {
      break;
    }
    remaining = __sync_sub_and_fetch(&gconf->writes_remaining, 1);
    if (!remaining) {
      gconf->die_write = 1;
    }
    gettimeofday(&tv, NULL);
    op_time = timeval_to_usec(&tv);
    if (op_time >= gconf->time_deadline) break;

    block = (off_t)(labs(random())) % (off_t)(gconf->seek_limit_blocks+1);
    start = block * gconf->block_size + gconf->byte_offset;
    if (tc->min_seek_byte > start) tc->min_seek_byte = start;
    if (tc->max_seek_byte < start) tc->max_seek_byte = start;

    if (lseek(tc->fd, start, SEEK_SET) < 0) {
      strerror_r(errno, strerrbuf, sizeof(strerrbuf));
      ERROR("Failed to seek to %lld: %s (%d)", (long long)start, strerrbuf, errno);
      break;
    }
    if (write(tc->fd, gconf->write_buffer, gconf->size) < 0) {
      strerror_r(errno, strerrbuf, sizeof(strerrbuf));
      ERROR("Failed to write %lu bytes at %lld: %s (%d)", gconf->size, (long long)start, strerrbuf, errno);
      break;
    }
    gettimeofday(&tv, NULL);
    op_time = (timeval_to_usec(&tv) - op_time);

    add_result(tc, tc->ops_done, (unsigned long)op_time);
    tc->ops_done++;
  }

  gettimeofday(&tv, NULL);
  tc->end = timeval_to_usec(&tv);
  return NULL;
}

void printconf() {
  LOG("Block size: %lld", (long long) gconf->block_size);
  LOG("Byte offset: %lld", (long long) gconf->byte_offset);
  LOG("Size per operation: %lu", (unsigned long) gconf->size);
  LOG("Read threads: %d", gconf->read_threads);
  LOG("Write threads: %d", gconf->write_threads);
  LOG("Seek range is 0 to %llu bytes", (unsigned long long) gconf->seek_limit_blocks * gconf->block_size);
  if (gconf->direct) {
    LOG("Using O_DIRECT");
  }
}

int cmpul(const void* l1, const void* l2) {
  if (*((const unsigned long*)l1) < *((const unsigned long*)l2))
    return -1;
  if (*((const unsigned long*)l1) > *((const unsigned long*)l2))
    return 1;
  return 0;
}

void printresults(const char* pool_name, test_context* c, int size) {
  unsigned int i;
  unsigned long long start_min = ULLONG_MAX, end_max = 0, total_ops = 0;
  off_t seek_min = LLONG_MAX, seek_max = 0;
  double total_elapsed = 0;
  unsigned long *all_results, *pos;

  if (size) {
    for (i = 0; i < size; ++i) {
      DEBUG("----------------------");
      DEBUG("%s thread %d", pool_name, i);
      DEBUG("----------------------");
      DEBUG("operations completed: %lu", c[i].ops_done);
      DEBUG("start: %llu", c[i].start);
      if (c[i].start < start_min) start_min = c[i].start;
      DEBUG("end: %llu", c[i].end);
      if (c[i].end > end_max) end_max = c[i].end;
      DEBUG("elapsed seconds: %f", ((double)(c[i].end - c[i].start))/1000000.0);
      total_ops += c[i].ops_done;
      DEBUG("min seek byte: %lld",(long long)c[i].min_seek_byte);
      DEBUG("max seek byte: %lld",(long long)c[i].max_seek_byte);
      if (c[i].min_seek_byte < seek_min) seek_min = c[i].min_seek_byte;
      if (c[i].max_seek_byte > seek_max) seek_max = c[i].max_seek_byte;
    }
  
    LOG("----------------------");
    LOG("total %s ops: %llu", pool_name, total_ops);
    if (total_ops) {
      // collect all the elapsed
      pos = all_results = (unsigned long*)malloc(total_ops * sizeof(unsigned long));
      for (i = 0; i < size; ++i) {
	memcpy(pos, c[i].results, c[i].ops_done*sizeof(unsigned long));
	pos += c[i].ops_done;
      }
      qsort(all_results, total_ops, sizeof(unsigned long), &cmpul);
      for (i = 0; i < total_ops; ++i) {
	total_elapsed += all_results[i];
      }

      LOG( "10%% %s usec: %lu", pool_name, all_results[(total_ops / 10ULL)] );
      LOG( "25%% %s usec: %lu", pool_name, all_results[(total_ops / 4ULL)] );
      LOG( "50%% %s usec: %lu", pool_name, all_results[(total_ops / 2ULL)] );
      LOG( "75%% %s usec: %lu", pool_name, all_results[(total_ops*3ULL / 4ULL)] );
      LOG( "90%% %s usec: %lu", pool_name, all_results[(total_ops*9ULL / 10ULL)] );
      LOG( "99%% %s usec: %lu", pool_name, all_results[(total_ops*99ULL / 100ULL)] );
      LOG( "99.9%% %s usec: %lu", pool_name, all_results[(total_ops*999ULL / 1000ULL)] );

      LOG("min %s usec: %lu", pool_name, all_results[0]);
      LOG("max %s usec: %lu", pool_name, all_results[total_ops-1]);
      free(all_results);
    }
    LOG("elapsed seconds: %f",((double)(end_max - start_min))/1000000.0);
    LOG("%s ops per second: %f", pool_name, ((double)total_ops)/ (((double)(end_max - start_min))/1000000.0));
    DEBUG("min seek byte: %lld",(long long)seek_min);
    DEBUG("max seek byte: %lld",(long long)seek_max);
  }
}


int main(int argc, char *argv[])
{
  int c, i, fd, flags;
  const char* fname;
  time_t num_seconds = 0;
  struct timeval starttimeval;
  struct stat fstat;
  off_t seek_limit_bytes = 0, dev_size = 0;
  pthread_t *read_workers = NULL, *write_workers = NULL;
  test_context *read_contexts;
  test_context *write_contexts;

  g_debug = 0;
  test_config* conf = (test_config*)malloc(sizeof(test_config));
  reset_conf(conf);

  srandom((unsigned int)time(NULL));
  setlinebuf(stderr);
  setlinebuf(stdout);

  /* process arguments */
  while (-1 != (c = getopt(argc, argv,
          "b:"  /* block size */
          "o:"  /* offset */
          "s:"  /* size (bytes) to read/write */
          "r:"  /* num read threads */
          "w:"  /* num write threads */
          "R:"  /* total number of reads to do (across all threads) */
          "W:"  /* total number of writes to do (across all threads) */
          "t:"  /* time (seconds) to run */
          "S:"  /* seek range limit (bytes) */
          "d"   /* debug */
          "D"   /* use O_DIRECT */
          "h?"  /* help */
        ))) {
    switch (c) {
    case 'b':
      conf->block_size = (off_t)strtoul(optarg, NULL, 10);
      break;
    case 'o':
      conf->byte_offset = (off_t)strtoul(optarg, NULL, 10);
      break;
    case 's':
      conf->size = (size_t)strtoul(optarg, NULL, 10);
      break;
    case 'r':
      conf->read_threads = atoi(optarg);
      break;
    case 'w':
      conf->write_threads = atoi(optarg);
      break;
    case 'R':
      conf->reads_remaining =
        conf->total_reads = (unsigned long)strtoul(optarg, NULL, 10);
      break;
    case 'W':
      conf->writes_remaining =
        conf->total_writes = (unsigned long)strtoul(optarg, NULL, 10);
      break;
    case 't':
      num_seconds = (time_t)strtol(optarg, NULL, 10);
      break;
    case 'S':
      seek_limit_bytes = (off_t)strtoull(optarg, NULL, 10);
      break;
    case 'd':
      g_debug = 1;
      break;
    case 'D':
      conf->direct = 1;
      break;
    case 'h':
    case '?':
      usage();
      exit(EXIT_SUCCESS);
    default:
      ERROR("Illegal argument \"%c\"", c);
      usage();
      return 1;
    }
  }

  if (!conf->total_reads && !conf->total_writes && !num_seconds) {
    ERROR("-r, -w, or -t is required");
    usage();
    return 1;
  }

  if (conf->byte_offset >= conf->block_size) {
    ERROR("offset (%lld) cannot be greater than block size (%lld)", (long long)conf->byte_offset, (long long)conf->block_size);
    return 1;
  }

  if (!conf->write_threads && conf->total_writes) conf->write_threads = 1;
  if (!conf->read_threads &&
      (conf->total_reads || !conf->write_threads)) conf->read_threads = 1;

  if (!conf->total_reads) conf->total_reads = conf->reads_remaining = ULONG_MAX;
  if (!conf->total_writes) conf->total_writes = conf->writes_remaining = ULONG_MAX;

  if (conf->write_threads) {
    //random data to write
    char* loc = conf->write_buffer = (char*)alloc_aligned(conf->size);
    char* end = loc + conf->size;
    if (!loc) return -1;

    for ( ; loc < end; loc += sizeof(long)) {
      long r = random();
      size_t size = (end-loc < sizeof(long)) ? (end-loc) : sizeof(long);
      memcpy(loc, &r, size);
    }
  }

  // get the device
  if (optind != argc - 1) {
    ERROR("Device or filename not specified");
    usage();
    return 1;
  }
  fname = argv[optind];
  if (stat(fname, &fstat)) {
    ERROR("Failed to stat %s: %s (%d)", fname, strerror(errno), errno);
    return -1;
  }

  fd = open(fname, O_RDWR);
  if (fd < 0) {
    ERROR("Failed to open %s: %s (%d)", fname, strerror(errno), errno);
    return -1;
  }

  if (S_ISBLK(fstat.st_mode)) {
    if (ioctl(fd, BLKGETSIZE64, &dev_size)) {
      ERROR("Failed to get file/device size: %s (%d)", strerror(errno), errno);
      close(fd);
      return -1;
    }
  } else if (S_ISREG(fstat.st_mode)) {
    dev_size = fstat.st_size;
  } else {
    ERROR("%s must be a device or regular file", fname);
    close(fd);
    return -1;
  }

  if (dev_size < seek_limit_bytes) {
    ERROR("File/device size %lld is smaller than seek limit %lld", (long long)dev_size, (long long)seek_limit_bytes);
    close(fd);
    return -1;
  }

  DEBUG("detected file/device size: %lld", (long long)dev_size);

  if (!seek_limit_bytes) seek_limit_bytes = dev_size;

  if (seek_limit_bytes <= conf->byte_offset + conf->size) {
    ERROR("Offset + size must be < seek limit");
    close(fd);
    return -1;
  }

  seek_limit_bytes -= conf->byte_offset + conf->size;
  conf->seek_limit_blocks = seek_limit_bytes/conf->block_size;

  gconf = conf;
  printconf();
  if (num_seconds) {
    LOG("Limiting test to %ld seconds", (long) num_seconds);
  }

  read_workers = (pthread_t*)malloc(conf->read_threads * sizeof(pthread_t));
  write_workers = (pthread_t*)malloc(conf->write_threads * sizeof(pthread_t));
  read_contexts = (test_context*)malloc(conf->read_threads * sizeof(test_context));
  write_contexts = (test_context*)malloc(conf->write_threads * sizeof(test_context));

  gettimeofday(&starttimeval, NULL);
  conf->time_deadline = num_seconds ? (timeval_to_usec(&starttimeval)+1000000ULL*num_seconds) : ULLONG_MAX;
  flags = O_RDONLY;
  if (conf->direct) flags |= O_DIRECT;

  for (i = 0; i < conf->read_threads; ++i) {
    test_context* tc = &read_contexts[i];
    reset_context(tc);
    tc->fd = open(fname, flags);
    if (tc->fd < 0) {
      ERROR("Failed to open %s: %s (%d)", fname, strerror(errno), errno);
      return -1;
    }
    pthread_create(&read_workers[i], NULL, &read_worker, tc);
  }

  flags = O_RDWR | O_SYNC;
  if (conf->direct) flags |= O_DIRECT;
  for (i = 0; i < conf->write_threads; ++i) {
    test_context* tc = &write_contexts[i];
    reset_context(tc);
    tc->fd = open(fname, flags);
    if (tc->fd < 0) {
      ERROR("Failed to open %s: %s (%d)", fname, strerror(errno), errno);
      return -1;
    }
    pthread_create(&write_workers[i], NULL, &write_worker, tc);
  }

  for (i = 0; i < conf->read_threads; ++i) {
    pthread_join(read_workers[i], NULL);
    DEBUG("...read thread %u done",i);
    close(read_contexts[i].fd);
  }

  for (i = 0; i < conf->write_threads; ++i) {
    pthread_join(write_workers[i], NULL);
    DEBUG("...write thread %u done",i);
    close(write_contexts[i].fd);
  }

  printresults("READ", read_contexts, gconf->read_threads);
  printresults("WRITE", write_contexts, gconf->write_threads);
  // TODO free context results?

  close(fd);

  return 0;
}

