# disktest

### a quick and dirty multithreaded read/write seek test

This tool works only with gcc/Linux.

The test reads or writes small random data (default 4KB) to block aligned seeks of a specified file or device.

    usage: disktest [options] [-rwRWt] <device or file>
            -b [bytes]      block size (default 4K)
            -o [bytes]      offset into block (default 0)
            -s [bytes]      size to read/write (default 4K)
            -r [threads]    num read threads
            -w [threads]    num write threads
            -R [count]      total number of reads to do (across all threads)
            -W [count]      total number of writes to do (across all threads)
            -t [seconds]    time to run
            -S [bytes]      seek range limit (default is the size of the file/device)
            -d              debug on
            -D              use O_DIRECT (bypass page cache)
            -h              help

To test a file system, create a large file first.  e.g. for 500 GB:

    dd if=/dev/zero of=/data/mytestfile bs=4096 count=131072000

### Examples:

Read seek test with 32 threads, running for 30 seconds, bypass page cache:

    disktest -r 32 -t 30 -D /data/mytestfile

Do 8000 read seeks with 10 threads:

    disktest -R 8000 -r 10 /data/mytestfile

Run 32 read threads and 1 write thread on the device (device data will be trashed!):

    disktest -r 32 -w 1 -t 60 /dev/sdb

Seek within only the first 1 GB of the file/device

    disktest -r 32 -S 1000000000 -t 30 /data/mytestfile

### Authors:

+ Steve Jiang ([@sjiang](https://twitter.com/sjiang)) (email: gh at iamsteve.com)

### License

disktest is licensed under the [Apache License Version 2.0](https://raw.github.com/sjiang/disktest/master/LICENSE)
