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

static FILE *disk;
static int zero_block[BLOCK_SIZE / sizeof(int)];

typedef struct inode_t {
    int     magic;
    char    filename[64];
    int     size;
    int     fileblocks[110];
} inode_t;

typedef struct handle_t {
    inode_t *inode;
    int     inode_index;
} handle_t;

/*
static void print_zero_block()
{
    int buf_zero_block[128];
    fseek(disk, 0, SEEK_SET);
    fread(buf_zero_block, sizeof(int), 128, disk);
    int i;
    for (i = 0; i < 128; i++)
    {
        printf("zero_block[%d] = %d\n", i, buf_zero_block[i]);
    }
}
*/

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
    fseek(disk, (pos * sizeof(int)), SEEK_SET);
    fwrite(&inode_desc, sizeof(int), 1, disk);
}

static void write_block(int index, int *block)
{
    fseek(disk, index * BLOCK_SIZE, SEEK_SET);
    fwrite(block, sizeof(int), BLOCK_SIZE / sizeof(int), disk);
}

static int *read_block(int block_index)
{
    int *block = (int *) malloc(BLOCK_SIZE); /* must be free'd somewhere by a caller */
    fseek(disk, block_index * BLOCK_SIZE, SEEK_SET);
    size_t rd = fread(block, sizeof(int), BLOCK_SIZE / sizeof(int), disk);
    if (rd != 128)
    {
        printf("read_block error: Didn't read the whole block...\n");
        return NULL;
    }
    printf("Returning block!\n");
    return block;
}

static void update_first_free(int free_head)
{
    zero_block[4] = free_head; /* zero_block[4] is free list head */
    fseek(disk, (FREE_LIST_HEAD * sizeof(int)), SEEK_SET); /* go to zero_block[4] on the (disk) file */
    fwrite(&free_head, sizeof(int), 1, disk); /* write the new value, using a pointer to the argument as 'buffer' */
}

static int get_and_update_first_free()
{
    int res = zero_block[4];
    int *current_free = read_block(res);
    int next = current_free[0];
    free(current_free);
    zero_block[4] = next;
    fseek(disk, (FREE_LIST_HEAD * sizeof(int)), SEEK_SET);
    fwrite(&next, sizeof(int), 1, disk);
    return res;
}

static handle_t *handle_for_path(const char *path)
{
    int nmemb = BLOCK_SIZE / sizeof(int);
    int desc;
    int i;
    inode_t *inode;
    handle_t *handle;
    for (i = 5; i < nmemb; i++)
    {
        desc = zero_block[i];
        if (desc != -1)
        {
            inode = (inode_t *) read_block(desc);
            if (inode -> magic != MAGIC_INODE)
            {
                printf("inode's magic not correct. inconsistent fs\n");
                exit(-1);
            }
            printf("inode's filename %s at desc %d\n", inode -> filename, desc);
            if (!strcmp(inode -> filename, path))
            {
                handle = (handle_t *) malloc(sizeof(handle_t));
                handle -> inode = inode;
                handle -> inode_index = desc;
                return handle;
            }
            free(inode);
        }
    }
    return NULL; /* return value for nonexistent file */
}

static int sofs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    printf("Called create with path %s\n", path);
    int flh = zero_block[FREE_LIST_HEAD];
    int *block = read_block(flh);
    update_first_free(block[0]);
    free(block);
    /* create an inode */
    inode_t *inode = (inode_t *) malloc(sizeof(inode_t));
    memset(inode, -1, BLOCK_SIZE);
    inode -> magic = MAGIC_INODE;
    inode -> size = 0;
    strcpy(inode -> filename, path);
    write_block(flh, (int *) inode);
    /* update zero_block */
    new_inode_block_zero(get_first_free_inode_index(), flh);
    handle_t *handle = malloc(sizeof(handle_t));
    handle -> inode = inode;
    handle -> inode_index = flh;
    fi -> fh = (uint64_t) handle;
    return 0;
}

static int sofs_open(const char *path, struct fuse_file_info *fi)
{
    printf("Called open of path \"%s\" flags %d \n", path, fi->flags);
    handle_t *handle = handle_for_path(path);
    if (handle == NULL) /* if the file doesn't exist, create it */
    {
        printf("sofs_open returned -ENOENT\n");
        return -ENOENT;
    }
    fi -> fh = (uint64_t) handle;
    return 0;
}

static int sofs_release(const char *path, struct fuse_file_info *fi)
{
    printf("Called release with path \"%s\"\n", path);
    handle_t *hand = (handle_t *) fi -> fh;
    free(hand -> inode);
    free(hand);
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
        handle_t *handle = handle_for_path(path);
        if (handle != NULL)
        {
            inode_t *inode = handle -> inode;
            /* printf("getattr found!\n"); */
            stbuf -> st_size = inode -> size;
            stbuf -> st_nlink = 1;
            free(inode);
            free(handle);
        }
        else
        {
            /* printf("getattr not found!\n"); */
            res = -ENOENT;
        }
    }
    
    stbuf -> st_blksize = BLOCK_SIZE; /* blocksize for file system I/O */

	return res;
}

static int sofs_truncate(const char* path, off_t size)
{
    printf("Called truncate\n");
    /* TODO stub */
    return 0;
}

static int sofs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    printf("Called readdir\n");
    /* TODO stub */
    return 0;
}

static int sofs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("Called read with size = %zu and offset = %lld\n", size, offset);
    handle_t *handle = (handle_t *) fi -> fh;
    int block_i = offset / BLOCK_SIZE;
    int offset_in_block = offset - block_i * BLOCK_SIZE;
    int *fileblocks = (handle -> inode) -> fileblocks;
    char *block_buffer;
    int many_blocks = (size + offset) / BLOCK_SIZE + ((size + offset) % BLOCK_SIZE ? 1 : 0);
    int disk_block;
    int i;
    for (i = 0; i < many_blocks; i++)
    {
        disk_block = fileblocks[18 + block_i + i];
        if (disk_block != -1)
        {
            block_buffer = (char *) read_block(disk_block);
        }
        else
        {
            block_buffer = malloc(BLOCK_SIZE);
            memset(block_buffer, 0, BLOCK_SIZE);
        }
        memcpy(
            buf + (i ? ((i - 1) * BLOCK_SIZE + offset_in_block) : 0),
            block_buffer + (i ? 0 : offset_in_block),
            BLOCK_SIZE - (i ? (size - (i - 1) * BLOCK_SIZE - offset_in_block >= BLOCK_SIZE ? 0 : BLOCK_SIZE - size - (i - 1) * BLOCK_SIZE - offset_in_block) : offset_in_block)
                );
        free(block_buffer);
    }
    /*
    int disk_block = fileblocks[18 + block_i];
    if (disk_block != -1)
    {
        block_buffer = (char *) read_block(disk_block);
    }
    else
    {
        block_buffer = malloc(BLOCK_SIZE);
        memset(block_buffer, 0, BLOCK_SIZE);
    }
    memcpy(buf, block_buffer + offset_in_block, size);
    free(block_buffer);
    */
    return size;
}

static int sofs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("Called write with size %zu and offset %lld\n", size, offset);
    handle_t *handle = (handle_t *) fi -> fh;
    int block_i = offset / BLOCK_SIZE;
    int offset_in_block = offset - block_i * BLOCK_SIZE;
    int *fileblocks = (handle -> inode) -> fileblocks;
    char *block_buffer;
    int many_blocks = (size + offset) / BLOCK_SIZE + ((size + offset) % BLOCK_SIZE ? 1 : 0);
    int disk_block;
    int i;
    for (i = 0; i < many_blocks; i++)
    {
        disk_block = fileblocks[18 + block_i + i];
        if (disk_block != -1)
        {
            block_buffer = (char *) read_block(disk_block);
        }
        else
        {
            disk_block = get_and_update_first_free();
            fileblocks[18 + block_i + i] = disk_block;
            block_buffer = malloc(BLOCK_SIZE);
            memset(block_buffer, 0, BLOCK_SIZE);
        }
        memcpy(
            block_buffer + (i ? 0 : offset_in_block),
            buf + i * BLOCK_SIZE,
            BLOCK_SIZE - (i ? (size - (i - 1) * BLOCK_SIZE - offset_in_block >= BLOCK_SIZE ? 0 : BLOCK_SIZE - size - (i - 1) * BLOCK_SIZE - offset_in_block) : offset_in_block)
                );
        write_block(disk_block, (int *) block_buffer);
        free(block_buffer);
    }
    /*
    int disk_block = fileblocks[18 + block_i];
    if (disk_block != -1)
    {
        block_buffer = (char *) read_block(disk_block);
    }
    else
    {
        disk_block = get_and_update_first_free();
        fileblocks[18 + block_i] = disk_block;
        block_buffer = malloc(BLOCK_SIZE);
        memset(block_buffer, 0, BLOCK_SIZE);
    }
    memcpy(block_buffer + offset_in_block, buf, size);
    write_block(disk_block, (int *) block_buffer);
    free(block_buffer);
    */
    int file_size = (handle -> inode) -> size;
    if (offset + size > file_size)
    {
        file_size = offset + size;
    }
    (handle -> inode) -> size = file_size;
    write_block(handle -> inode_index, (int *) handle -> inode);
    return size;
}

static void sofs_destroy(void *private_data)
{
    fclose(disk);
}

static struct fuse_operations sofs_oper = {
    .getattr    = sofs_getattr,
    .readdir    = sofs_readdir,
    .open       = sofs_open,
    .release    = sofs_release,
    .read       = sofs_read,
    .truncate   = sofs_truncate,
    .write      = sofs_write,
    .create     = sofs_create,
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
    
    if (fsck()) /* inconsistent file system */
    {
        printf("Error: inconsistent file system\n");
        return -1;
    }
    
	return fuse_main(fuse_main_argc, fuse_main_argv, &sofs_oper, NULL);
}
