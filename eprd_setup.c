#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#include "eprd.h"

#define die_ioctlerr(f...) { fprintf(stderr,(f)); flock(fd, LOCK_UN); exit(-1); }
#define die_syserr() { fprintf(stderr,"Fatal system error : %s",strerror(errno)); exit(-2); }

int errno;

struct option_info {
	int create;
	int sectorsize;
	unsigned long cachesize;
	unsigned int commit_interval;
	char *datafile;
	off64_t dtasize;
	char *device;
	int barrier;
};

void *s_malloc(size_t size)
{
	void *retval;
	retval = malloc(size);
	if (!retval)
		die_syserr();
	return retval;
}

void *s_realloc(void *ptr, size_t size)
{
	void *retval;
	retval = realloc(ptr, size);

	if (!retval)
		die_syserr();
	return retval;
}

void *as_sprintf(const char *fmt, ...)
{
	/* Guess we need no more than 100 bytes. */
	int n, size = 100;
	void *p;
	va_list ap;
	p = s_malloc(size);
	while (1) {
		/* Try to print in the allocated space. */
		va_start(ap, fmt);
		n = vsnprintf(p, size, fmt, ap);
		va_end(ap);
		/* If that worked, return the string. */
		if (n > -1 && n < size)
			return p;
		/* Else try again with more space. */
		if (n > -1)	/* glibc 2.1 */
			size = n + 1;	/* precisely what is needed */
		else		/* glibc 2.0 */
			size *= 2;	/* twice the old size */
		p = s_realloc(p, size);
	}
}

static struct option_info mkoptions;

int eprd_setup(int op, int fd)
{
	int ffd, i;
	char *pass;
	char *filename;
	struct stat stbuf;
	unsigned long fsize;
	int mode;
	int ret = 0;

	switch (op) {
	case EPRD_SET_DTAFD:
		if (mkoptions.create)
			mode = O_CREAT | O_RDWR;
		else
			mode = O_RDWR;
		if ((ffd = open(mkoptions.datafile, mode, 0600)) < 0) {
			if (ffd < 0) {
				fprintf(stderr, "Failed to open file %s",
					mkoptions.datafile);
				return 1;
			}
		}
		if (mkoptions.create)
			ret = ftruncate(ffd, mkoptions.dtasize);
		if (ret != 0) {
			fprintf(stderr, "ftruncate %s failed : %s\n",
				mkoptions.datafile, strerror(errno));
			close(ffd);
			return ret;
		}
		if (ioctl(fd, EPRD_SET_DTAFD, ffd) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_SET_DTAFD failed on /dev/eprdcontrol\n");
			close(ffd);
			return rc;
		}
		if (-1 == fstat(ffd, &stbuf)) {
			fprintf(stderr, "Failed to stat %s",
				mkoptions.datafile);
			close(ffd);
			return -1;
		}
		if (ioctl(fd, EPRD_SET_DEVSZ, mkoptions.dtasize) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_SET_DEVSZ failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_SET_COMMIT_INTERVAL:
		if (ioctl
		    (fd, EPRD_SET_COMMIT_INTERVAL,
		     mkoptions.commit_interval) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_SET_COMMIT_INTERVAL failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_CACHESIZE:
		if (ioctl(fd, EPRD_CACHESIZE, mkoptions.cachesize) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_CACHESIZE failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_SET_SECTORSIZE:
		if (ioctl(fd, EPRD_SET_SECTORSIZE, mkoptions.sectorsize) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_SET_SECTORSIZE failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_REGISTER:
		if (ioctl(fd, EPRD_REGISTER, 0) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_REGISTER failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_DEREGISTER:
		if (ioctl(fd, EPRD_DEREGISTER, (unsigned long)mkoptions.device)
		    < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_DEREGISTER failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_BARRIER:
		if (ioctl(fd, EPRD_BARRIER, (unsigned long)mkoptions.barrier) <
		    0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_BARRIER failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	case EPRD_INIT:
		if (ioctl(fd, EPRD_INIT, 0) < 0) {
			int rc = 1;
			fprintf(stderr,
				"ioctl EPRD_INIT failed on /dev/eprdcontrol\n");
			return rc;
		}
		break;
	default:
		abort();
	}
	close(ffd);
	return 0;
}

void usage(char *name)
{
	printf
	    ("Create : %s -f datafile -s size_of_blockdevice[MG] -m commit_interval [-z sectorsize(512..4096) -b(barrier) -c(create) -h(help) -p(cachesize)]\n",
	     name);
	printf("Detach : %s -d /dev/eprd_device_name\n", name);
	exit(-1);
}

int get_opts(int argc, char *argv[])
{

	int c, ret = 0;

	while ((c = getopt(argc, argv, "d:bs:hcf:m:p:z:")) != -1)
		switch (c) {
		case 'c':
			mkoptions.create = 1;
			break;
		case 'b':
			mkoptions.barrier = 1;
			break;
		case 'm':
			if (optopt == 'm')
				printf
				    ("Option -%m requires commit interval (sec) as argument.\n",
				     optopt);
			else
				sscanf(optarg, "%u",
				       &mkoptions.commit_interval);
			break;
		case 'f':
			if (optopt == 'f')
				printf
				    ("Option -%c requires a lessfs configuration file as argument.\n",
				     optopt);
			else
				mkoptions.datafile = optarg;
			break;
		case 'd':
			if (optopt == 'd')
				printf
				    ("Option -%c requires a device as argument.\n",
				     optopt);
			else {
				mkoptions.device = optarg;
			}
			break;
		case 's':
			if (optopt == 's')
				printf
				    ("Option -%c requires file size as argument.\n",
				     optopt);
			else {
				sscanf(optarg, "%lu", &mkoptions.dtasize);
				if (optarg[strlen(optarg) - 1] == 'M')
					mkoptions.dtasize *= 1024 * 1024;
				if (optarg[strlen(optarg) - 1] == 'G')
					mkoptions.dtasize *= 1024 * 1024 * 1024;
			}
			break;
		case 'p':
			if (optopt == 'p')
				printf
				    ("Option -%c requires cache size as argument.\n",
				     optopt);
			else {
				sscanf(optarg, "%lu", &mkoptions.cachesize);
				if (optarg[strlen(optarg) - 1] == 'M')
					mkoptions.cachesize *= 1024 * 1024;
				if (optarg[strlen(optarg) - 1] == 'G')
					mkoptions.cachesize *=
					    1024 * 1024 * 1024;
			}
			break;
		case 'z':
			if (optopt == 'z')
				printf
				    ("Option -%c requires sector size as argument.\n",
				     optopt);
			else {
				sscanf(optarg, "%i", &mkoptions.sectorsize);
				if (mkoptions.sectorsize > 4096)
					mkoptions.sectorsize = 0;
				if (mkoptions.sectorsize < 0)
					mkoptions.sectorsize = 0;
				if (mkoptions.sectorsize > 0) {
					ret = mkoptions.sectorsize / 512;
					ret *= 512;
					if (ret != mkoptions.sectorsize) {
						ret = -1;
						printf
						    ("The sectorsize has to be a multiple of 512 bytes\n");
					} else
						ret = 0;
				}
			}
			break;
		case 'h':
			usage(argv[0]);
			break;
		default:
			abort();
		}
	printf("\n");
	return ret;
}

int main(int argc, char *argv[])
{
	int ret = -1, dtaexists, idxexists, blkexists;
	mkoptions.create = 0;
	mkoptions.barrier = 0;
	mkoptions.commit_interval = 0;
	mkoptions.datafile = NULL;
	mkoptions.cachesize = 0;
	mkoptions.sectorsize = 0;
	struct stat stdta;
	struct stat device;
	int mode = O_RDWR;
	int fd;
	int dev;

	if (argc < 3)
		usage(argv[0]);
	if (0 != get_opts(argc, argv))
		exit(-1);

	if ((fd = open("/dev/eprdcontrol", mode)) < 0) {
		fprintf(stderr,
			"Failed to open /dev/eprdcontrol, is eprd.ko loaded?\n");
		exit(-1);
	}

	if (-1 == flock(fd, LOCK_EX)) {
		fprintf(stderr, "Failed to lock /dev/eprdcontrol\n");
		exit(-1);
	}

	if (mkoptions.device) {
		if (-1 == stat(mkoptions.device, &device)) {
			fprintf(stderr, "No such device : %s\n",
				mkoptions.device);
			exit(-1);
		}
		if (!S_ISBLK(device.st_mode)) {
			fprintf(stderr, "Not a blockdevice : %s\n",
				mkoptions.device);
			exit(-1);
		}
		ret = eprd_setup(EPRD_DEREGISTER, fd);
		if (0 != ret)
			die_ioctlerr("ioctl EPRD_DEREGISTER failed\n");
		exit(ret);
	}
	if (mkoptions.create)
		printf("creating a new blockdevice\n");
	else
		printf("Using an existing blockdevice\n");
	printf("datafile=%s\n", mkoptions.datafile);
	dtaexists = stat(mkoptions.datafile, &stdta);

	if (mkoptions.create && 0 == dtaexists) {
		fprintf(stderr,
			"Data file %s already exists, cowardly refusing to overwrite.\n",
			mkoptions.datafile);
		goto end_exit;
	}

	fprintf(stderr, "commit_interval = %u\n", mkoptions.commit_interval);

	if (mkoptions.create) {
		mkoptions.dtasize /= 4096;
		mkoptions.dtasize *= 4096;
		printf("Blockdevice size = %li\n", mkoptions.dtasize);
	} else {
		if (S_ISBLK(stdta.st_mode)) {
			if ((dev = open(mkoptions.datafile, O_RDONLY)) < 0) {
				fprintf(stderr, "Failed to open %s\n",
					mkoptions.datafile);
				exit(-1);
			}
			mkoptions.dtasize = lseek64(dev, 0, SEEK_END);
			if (-1 == mkoptions.dtasize) {
				fprintf(stderr, "Error while opening %s : %s\n",
					mkoptions.datafile, strerror(errno));
				goto end_exit;
			}
			mkoptions.dtasize /= 4096;
			mkoptions.dtasize *= 4096;
			close(dev);
		} else {
			mkoptions.dtasize = (unsigned long)stdta.st_size;
		}
		printf("Blockdevice size = %li\n", mkoptions.dtasize);
	}

	if (mkoptions.dtasize < 1048576) {
		fprintf(stderr, "Blockdevice %s with size %li is to small\n",
			mkoptions.datafile, mkoptions.dtasize);
		goto end_exit;
	}

	ret = eprd_setup(EPRD_INIT, fd);
	if (0 != ret)
		die_ioctlerr("ioctl EPRD_INIT failed\n");

	if (NULL != mkoptions.datafile) {
		ret = eprd_setup(EPRD_SET_DTAFD, fd);
		if (0 != ret)
			die_ioctlerr("ioctl EPRD_SET_DTAFD failed\n");
	}
	if (mkoptions.barrier) {
		ret = eprd_setup(EPRD_BARRIER, fd);
		if (0 != ret)
			die_ioctlerr("ioctl EPRD_BARRIER failed\n");
	}

	if (0 != mkoptions.cachesize) {
		printf("Cache size = %lu\n", mkoptions.cachesize);
		ret = eprd_setup(EPRD_CACHESIZE, fd);
		if (0 != ret)
			die_ioctlerr("ioctl EPRD_CACHESIZE failed\n");
	}

	if (0 != mkoptions.sectorsize) {
		printf("Sector size = %u\n", mkoptions.sectorsize);
		ret = eprd_setup(EPRD_SET_SECTORSIZE, fd);
		if (0 != ret)
			die_ioctlerr("ioctl EPRD_SET_SECTORSIZE failed\n");
	}

	ret = eprd_setup(EPRD_SET_COMMIT_INTERVAL, fd);
	if (0 != ret)
		die_ioctlerr("ioctl EPRD_SET_COMMIT_INTERVAL failed\n");

	ret = eprd_setup(EPRD_REGISTER, fd);
	if (0 != ret)
		die_ioctlerr("ioctl EPRD_REGISTER failed\n");
      end_exit:
	flock(fd, LOCK_UN);
	close(fd);
	exit(ret);
}
