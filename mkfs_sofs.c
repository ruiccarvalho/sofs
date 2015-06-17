/*
  MKFS_SOFS: Utility to create a well-formated virtual disk
  to use with SOFS, a filesystem to run in userspace using
  FUSE.

  Copyright (C) 2015  Rui Caravlho <rm.carvalho@campus.fct.unl.pt>

  gcc -Wall -o mkfs_sofs mkfs_sofs.c
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE      512 /* bytes */
#define MODE            "w"
#define MAGIC_1         0x9aa9aa9a
#define MAGIC_2         0x6d5fa7c3
#define FREE_LIST_HEAD  1

/* pre: blocks >=3 */
int create_fs(int blocks, const char *filename)
{    
    /* printf("Entered create_fs with blocks = %d and filename = %s\n", blocks, filename); */
    
    FILE *disk = fopen(filename, MODE);
    
    int nmemb = BLOCK_SIZE / sizeof(int);
    
    int buf[nmemb];
    
    /* Create BLOCK ZERO */
    
    memset(buf, -1, BLOCK_SIZE);    /* set to '-1' all 128 int positions of a buffer of 512 bytes */

    buf[0] = MAGIC_1;               /* magic 1 for fsck */
    buf[1] = MAGIC_2;               /* magic 1 for fsck */
    buf[2] = BLOCK_SIZE;            /* size of blocks, constant */
    buf[3] = blocks;                /* total number of blocks in this disk */
    buf[4] = FREE_LIST_HEAD;        /* number of the first free block */
    
    size_t wrt = fwrite(buf, sizeof(int), nmemb, disk);
    
    if (wrt != nmemb)
    {
        fclose(disk);
        return -1;
    }

    memset(buf, 0, BLOCK_SIZE); /* zero-out a buffer of 512 bytes */

    int i;
    for (i = 1; i < blocks; i++)
    {
        buf[0] = (i < blocks - 1) ? (i + 1) : -1;
        wrt = fwrite(buf, sizeof(int), nmemb, disk);
        if (wrt != nmemb)
        {
            printf("Not equal\n");
            fclose(disk);
            return -1;
        }
    }
    
    return fclose(disk); /* close the stream and return. 0 for success, else propagate error to caller */
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("Usage: mkfs_sofs n device\n");
        printf("with n number of 512 bytes blocks\n");
        printf("and device path of file to be created as SOFS disk\n");
        return -1;
    }
    
    char *end;
    
    int n_of_blocks = (int) strtol(argv[1], &end, 10);
    
    if (*end)
    {
        printf("First argument is NaN\n");
        return -1;
    }
    else if (n_of_blocks < 3)
    {
        printf("Disk must be of, at least, 3 blocks\n");
        return -1;
    }
    
    const char *filename = argv[2];

    int res = create_fs(n_of_blocks, filename);

    if (!res)
    {
        printf("A formated disk has been created with filename \"%s\"\n", filename);
    }
    else
    {
        printf("An error occurred\n");
    }
    
    return res;
}
