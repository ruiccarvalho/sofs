/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall sofs.c `pkg-config fuse --cflags --libs` -o sofs
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define BLOCK_SIZE          512 /* bytes */
#define FILENAME_SIZE       64
#define FILE_BLOCKS         110
#define N_OF_INODES         123
#define MODE                "r+"
#define MAGIC_1             0x9aa9aa9a
#define MAGIC_2             0x6d5fa7c3
#define MAGIC_INODE         0xf9fe9eef
#define FREE_LIST_HEAD      4
#define FIRST_FD_INDEX      5
#define BLOCK_CNT_INDEX     3
#define MAGIC_1_INDEX       0
#define MAGIC_2_INDEX       1

/* static char *disk_file_name; */
static FILE *disk;
static int fd_disk; /* the int file descriptor of the FILE *disk stream */
static int zero_block[BLOCK_SIZE / sizeof(int)];
static pthread_mutex_t lock;

/*typedef struct fhandle {
    int fd;
    int *inode;
} fhandle;*/

static void print_zero_block()
{
    int i;
    for (i = 0; i < 128; i++)
    {
        printf("zero_block[%d] = %d\n", i, zero_block[i]);
    }
}

static int fsck()
{
    int nmemb = BLOCK_SIZE / sizeof(int);
    
    size_t rd = fread(zero_block, sizeof(int), nmemb, disk);
    
    if (rd != nmemb)
    {
        return -1;
    }
    
    if (zero_block[MAGIC_1_INDEX] != MAGIC_1 || zero_block[MAGIC_2_INDEX] != MAGIC_2)
    {
        printf("Error: magic numbers don't match.\n");
        return -1;
    }
    
    return 0;
}

static int get_first_free_inode_index()
{
    int nmemb = BLOCK_SIZE / sizeof(int);
    int i;
    for (i = 5; i < nmemb; i++)
    {
        if (zero_block[i] == -1)
        {
            return i;
        }
    }
    return -1;
}

static void new_inode_block_zero(int pos, int inode_desc)
{
    zero_block[pos] = inode_desc;
    pthread_mutex_lock(&lock);
    lseek(fd_disk, (pos * sizeof(int)), SEEK_SET);
    fwrite(&inode_desc, sizeof(int), 1, disk);
    pthread_mutex_unlock(&lock);
}

static void update_first_free(int free_head)
{
    printf("INSIDE update_first_free(%d)\n", free_head);
    zero_block[4] = free_head; /* zero_block[4] is free list head */
    pthread_mutex_lock(&lock);
    lseek(fd_disk, (4 * sizeof(int)), SEEK_SET); /* go to zero_block[4] on the (disk) file */
    fwrite(&free_head, sizeof(int), 1, disk); /* write the new value, using a pointer to the argument as 'buffer' */
    pthread_mutex_unlock(&lock);
    print_zero_block();
}

// static void write_block_to_disk(int i, int *block)
// {
//     printf("Entered write block to disk with i = %d\n", i);
//     pthread_mutex_lock(&lock);(fd_disk, (i * BLOCK_SIZE), SEEK_SET);
//     fwrite(block, sizeof(int), 128, disk);
// }

static int *read_block(int block_index)
{
    int *block = (int *) malloc(BLOCK_SIZE); /* must be free'd somewhere by a caller */
    printf("block_index * BLOCK_SIZE = %d\n", block_index * BLOCK_SIZE);
    pthread_mutex_lock(&lock);
    lseek(fd_disk, (block_index * BLOCK_SIZE), SEEK_SET);
    size_t rd = fread(block, 4, 128, disk);
    pthread_mutex_unlock(&lock);
    printf("rd = %lu\n", rd);
    printf("rd is such because of an error? %d\n", ferror(disk));
    printf("rd is such because of EOF? %d\n", feof(disk));
    if (rd != 128)
    {
        return NULL;
    }
    printf("Returning block!\n");
    return block;
}

static int *inode_for_path(const char *path)
{
    printf("Hi from inode_for_path with path \"%s\"\n", path);
    int nmemb = BLOCK_SIZE / sizeof(int);
    int *inode;
    int desc;
    int i;
    for (i = 5; i < nmemb; i++)
    {
        desc = zero_block[i];
        if (desc != -1)
        {
            printf("Desc = %d\n", desc);
            inode = (int *) read_block(desc);
            printf("inode != null? %d\n", inode != NULL);
            if (inode[0] != MAGIC_INODE)
            {
                printf("inode's magic not correct (it's %d). inconsistent fs\n", inode[0]);
                exit(-1);
            }
            if (!strcmp((char *) inode + 1, path))
            {
                printf("inode_for_path found filename %s at desc %d\n", (char *) inode + 1, desc);
                return inode;
            }
            free(inode);
        }
    }
    return NULL; /* return value for nonexistent file */
}

static int sofs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    printf("Entered create with path %s\n", path);
    int flh = zero_block[FREE_LIST_HEAD];
    int *block = read_block(flh);
    printf("free head's fst position has %d\n", block[0]);
    update_first_free(block[0]);
    free(block);
    /* create an inode */
    int *inode = (int *) malloc(BLOCK_SIZE);
    memset(inode, -1, BLOCK_SIZE);
    inode[0] = MAGIC_INODE;
    printf("Will strcpy inode's filename, path w/ path = %s\n", path);
    strcpy((char *) inode + 1, path);
    printf("strcpyd inode's filename, path. inode->filename = %s\n", (char *) inode + 1);
    //write_block_to_disk(flh, inode);
    printf("flh = %d\n", flh);
    pthread_mutex_lock(&lock);
    lseek(fd_disk, 32, SEEK_SET);
    fwrite(inode, sizeof(int), 128, disk);
    pthread_mutex_unlock(&lock);
    /* update zero_block */
    int pos = get_first_free_inode_index();
    new_inode_block_zero(pos, flh);
    fi -> fh = (uint64_t) inode;
    return 0;
}

static int sofs_open(const char *path, struct fuse_file_info *fi)
{
    printf("Called open of path \"%s\" flags %d \n", path, fi->flags);
    int *inode = inode_for_path(path);
    if (inode == NULL) /* if the file doesn't exist, create it */
    {
        //int flh = zero_block[FREE_LIST_HEAD];
        //int *block = read_block(flh);
        //update_first_free(block[0]);
        //free(block);
        ///* create an inode */
        //inode = malloc(nmemb * sizeof(int));
        //memset(inode, -1, BLOCK_SIZE);
        //inode[0] = MAGIC_INODE;
        //strcpy((char *) inode + 1, path);
        //write_block_to_disk(flh, inode);
        return -ENOENT;
    }
    fi -> fh = (uint64_t) inode;
    return 0;
}

static int sofs_release(const char *path, struct fuse_file_info *fi)
{
    printf("Called release with path \"%s\"\n", path);
    free((int *) fi -> fh);
    return 0;
}

static int sofs_getattr(const char *path, struct stat *stbuf)
{
    printf("Called getattr with path \"%s\"\n", path);
	int res = 0;

    stbuf -> st_dev = 0;                /* ID of device containing file */
    stbuf -> st_ino = 0;                /* inode number */
    if (!strcmp(path, "/"))             /* root directory */
    {
        stbuf -> st_mode = S_IFDIR | 0777;
    }
    else
    {
        stbuf -> st_mode = S_IFREG | 0777;
        int *inode = inode_for_path(path);
        if (inode != NULL)
        {
            printf("getattr found!\n");
            stbuf -> st_size = inode[17];
            stbuf -> st_nlink = 1;
            free(inode);
        }
        else
        {
            printf("getattr not found!\n");
            res = -ENOENT;
        }
    }
    
    stbuf -> st_blksize = BLOCK_SIZE; /* blocksize for file system I/O */

	return res;
}

static int sofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    printf("Called readdir\n");
	/* TODO stub */
    return 0;
}

static int sofs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("Called read\n");
	/* TODO stub */
	return 0;
}

static int sofs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("Called write\n");
    return 1;
}

/*
static void *sofs_init(struct fuse_conn_info *conn)
{
    return NULL;
}
*/

static void sofs_destroy(void *private_data)
{
    fclose(disk);
    pthread_mutex_destroy(&lock);
}

static struct fuse_operations sofs_oper = {
    .getattr    = sofs_getattr,
    .readdir    = sofs_readdir,
    .open       = sofs_open,
    .release    = sofs_release,
    .read       = sofs_read,
    .write      = sofs_write,
    //.mknod      = sofs_mknod,
    .create     = sofs_create,
    //.init       = sofs_init,
    .destroy    = sofs_destroy,
};

int main(int argc, char *argv[])
{
	if (argc != 4 && argc != 3)
	{
		printf("Usage:\n");
		printf("sofs [fuse switches] filename mountpoint\n");
        return -1;
	}

	int fuse_main_argc = argc - 1;

	char *fuse_main_argv[3];
	fuse_main_argv[0] = argv[0];
	if (argc == 3)
	{
		fuse_main_argv[1] = argv[2];
	}
	else
	{
		fuse_main_argv[1] = argv[1];
		fuse_main_argv[2] = argv[3];
	}
    
    const char *disk_file_name = argv[argc == 3 ? 1 : 2];

	disk = fopen(disk_file_name, MODE);
    
    if (pthread_mutex_init(&lock, NULL) != 0)
    {
        printf("mutex init failed\n");
        return 1;
    }
    
    if (fsck()) /* inconsistent file system */
    {
        printf("Error: inconsistent file system\n");
        return -1;
    }
    
    print_zero_block();
    
    printf("Welcome. Mounting. Oh, btw, sizeof(int) = %lu\n", sizeof(int));

	return fuse_main(fuse_main_argc, fuse_main_argv, &sofs_oper, NULL);
}
