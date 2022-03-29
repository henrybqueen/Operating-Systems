#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sfs_api.h"
#include "disk_emu.h"
#include <unistd.h>
#include <math.h>


typedef struct SuperBlock {
    int magicNumber;
    int block_size;
    int num_blocks;
    int iNode_table_length;
    int rootDir;

} SuperBlock;

typedef struct iNode {
    int size;
    int num_blocks_allocated;
    int direct_pointers[12];
    int indirect_pointer;
} iNode;

#define MAX_FNAME_LENGTH 32

typedef struct dirEntry {
    char file_name[MAX_FNAME_LENGTH];
    int inode_num;
    char available;
} dirEntry;

typedef struct fileDesc {
    int inode_num;
    int read_write_pointer;
    char available;
} fileDesc;

int current;

struct SuperBlock *super_block;

/* bit maps for blocks and inodes */
char free_blocks[NUM_BLOCKS];
char free_inodes[NUM_iNODES];

struct iNode iNodeTable[NUM_iNODES];
struct fileDesc fileDescTable[NUM_iNODES];

#define DIR_SIZE 12*BLOCK_SIZE
struct dirEntry directory[DIR_SIZE];

void init_super();
void write_to_disk();
void read_from_disk();
int get_free_inode();
int get_free_block();
int get_dir_spot();
int get_fd(int);
int secure_block(int, int, int);


/* assumes super block has been allocated */
void init_super() {
    super_block->magicNumber=0xACBD0005;
    super_block->block_size=BLOCK_SIZE;
    super_block->num_blocks=NUM_BLOCKS;

    super_block->iNode_table_length = ((sizeof(iNode) * NUM_iNODES) / BLOCK_SIZE) + 1;
    super_block->rootDir=0;
}

// initialize all iNode pointers to -1
void init_iNodes() {
    for (int i=0; i<NUM_iNODES; i++) {
        iNodeTable[i].indirect_pointer=-1;
        for (int j = 0; j<12; j++) {
            iNodeTable[i].direct_pointers[j] = -1;
        }
    }
}

void init_fd_table() {
    for (int i=0; i<NUM_iNODES; i++) {
        fileDescTable[i].available='1';
    }
}

/* check if a file with filename (name) is in the directory, returns 
the index of the file in the directory if so */
int exists_name(char * name) {

    for (int i=0; i<DIR_SIZE; i++) {
        if (strncmp(directory[i].file_name, name, MAX_FNAME_LENGTH) == 0) {
            return i;
        } 
    }

    return -1;
}

void mksfs(int fresh) {

    super_block = (SuperBlock*) malloc(sizeof(SuperBlock));
    current = -1;

    init_fd_table();

    if (fresh) {

        init_fresh_disk("sfs.file", BLOCK_SIZE, NUM_BLOCKS);

        init_super();
        init_iNodes();

        memset(free_blocks, '1', NUM_BLOCKS);
        for (int i=0; i<=super_block->iNode_table_length+14; i++) {
            free_blocks[i] = '0';
        }

        memset(free_inodes, '1', NUM_iNODES);
        free_inodes[0] = '0';

        // initialize the direct pointers for the dir inode
        for (int i=0; i < 12; i++) {
            iNodeTable[super_block->rootDir].direct_pointers[i]=3+i;
        }

        write_to_disk();

    } else {
        
        init_disk("sfs.file", BLOCK_SIZE, NUM_BLOCKS);
        read_from_disk();
    }
}

/*

    Method for writing in memory data structures to the disk 

    0: super block
    1: free block bitmap
    2: free_inodes bitmap
    3, ..., 14: directory blocks
    15, ..., iNode_table_length+14: iNode table blocks
*/
void write_to_disk() {

    char buf[1024];

    memcpy(&buf, super_block, sizeof(SuperBlock));
    write_blocks(0, 1, &buf);

    memcpy(&buf, free_blocks, sizeof(free_blocks));
    write_blocks(1, 1, &buf);

    memcpy(&buf, free_inodes, sizeof(free_inodes));
    write_blocks(2, 1, &buf);

    write_blocks(3, 12, directory);

    char buf2[BLOCK_SIZE*super_block->iNode_table_length];
    memcpy(&buf2, iNodeTable, sizeof(iNodeTable));
    write_blocks(15, super_block->iNode_table_length, &buf2);

}

int get_block(int pointer) {
    return pointer / BLOCK_SIZE;
}

int sfs_remove(char* file_name) {

    int dir_spot = exists_name(file_name);

    if (dir_spot <0) {
        printf("Error[sfs_romve]: file %s doesnt exist\n", file_name);
        return -1;
    }

    struct dirEntry *entry = &directory[dir_spot];
    struct iNode *node = &iNodeTable[entry->inode_num];

    /* flush all data for this file */

    directory[dir_spot].available='1';
    for (int i=0; i<12;i++) {
        if (node->direct_pointers[i] != -1) {
            free_blocks[node->direct_pointers[i]] = '1';
            node->direct_pointers[i] = -1;
        }
        
    }
    if (node->indirect_pointer != -1) {
        int indirectPointer[BLOCK_SIZE/sizeof(int)];
        char buf[BLOCK_SIZE];
        read_blocks(node->indirect_pointer, 1, &buf);
        memcpy(indirectPointer, &buf, sizeof(indirectPointer));

        for (int i=0; i<BLOCK_SIZE/sizeof(int); i++) {
            if (indirectPointer[i] != 0) {
                free_blocks[indirectPointer[i]] = '1';
                indirectPointer[i] = 0;
            }
        }

        free_blocks[node->indirect_pointer] = '1';
        node->indirect_pointer = -1;
    }

    free_inodes[entry->inode_num] = '1';
    iNodeTable[entry->inode_num].num_blocks_allocated = 0;
    iNodeTable[entry->inode_num].size = 0;
    
    entry->available = '1';

    write_to_disk();

    return 0;

}

int sfs_fread(int fd, char* buf, int length) {

    /* dont allow reads from unopen files */
    if (fileDescTable[fd].available == '1') {
        return -1;
    }

    struct fileDesc *entry = &fileDescTable[fd];
    struct iNode *node = &iNodeTable[entry->inode_num];

    // calculate which block to start reading from and which block to end at
    int start_block = get_block(entry->read_write_pointer);
    int end_block = get_block(entry->read_write_pointer+length);

    char *buffer = malloc(BLOCK_SIZE);
    int block_adress;

    int buf_pointer =0;
    int num_bytes_read=0;
    int end_loop = 0;
    int a, b;
    int num_bytes_to_read;
    for (int i=start_block; i<= end_block; i++) {


        block_adress = secure_block(entry->inode_num, i, 0);

        // if the block we're trying to read from isnt allocated, break read loop
        /*
        if (block_adress < 0 && buf_pointer < length) {
            break;
        }
        */
        if (block_adress < 0) {
            break;
        }
        
        read_blocks(block_adress, 1, buffer);
        
        // the byte inside this block to start reading from
        int start_offset = (i == start_block) ? (entry->read_write_pointer % BLOCK_SIZE) : 0;

        // the bytes inside this block to stop reading from
        int end_offset;
        // if we are at the end of the file, special care is needed
        if (get_block(entry->read_write_pointer+node->size) == i) {
            
            a = (entry->read_write_pointer+node->size) % BLOCK_SIZE;
            b = (i == end_block) ? ((entry->read_write_pointer+length) % BLOCK_SIZE) : BLOCK_SIZE;
            end_offset = (a < b) ? a : b;
    
            // end the read loop, since this is the end of the file
            end_loop = 1;
        } else {
            end_offset = (i == end_block) ? ((entry->read_write_pointer+length) % BLOCK_SIZE) : BLOCK_SIZE;
        }
        
        num_bytes_to_read = end_offset-start_offset;

        // copy the block read into the buffer
        memcpy(&buf[buf_pointer], &buffer[start_offset], num_bytes_to_read);

        buf_pointer += num_bytes_to_read;
        num_bytes_read += num_bytes_to_read;

        if (end_loop) {
            break;
        }
    }

    entry->read_write_pointer += num_bytes_read;
    write_to_disk();

    free(buffer);

    // the number of bytes read is just the pointer for the buf after reading
    return num_bytes_read;
}

int sfs_fseek(int fd, int loc) {
    struct fileDesc *entry = &fileDescTable[fd];
    entry->read_write_pointer = loc;
    return 0;
}

int sfs_fwrite(int fd, char*buf, int length) {

    /* dont allow writes to unopen files */
    if (fileDescTable[fd].available == '1') {
        return -1;
    }

    struct fileDesc *entry = &fileDescTable[fd];
    struct iNode *node = &iNodeTable[entry->inode_num];

    // compute which blocks we're writing to
    int start_block = get_block(entry->read_write_pointer);
    int end_block = get_block(entry->read_write_pointer+length);

    char *buffer = malloc(BLOCK_SIZE);
    int block_adress;

    int buf_pointer =0;

    for (int i=start_block; i<= end_block; i++) {

        
        block_adress = secure_block(entry->inode_num, i, 1);

        if (block_adress < 0) {
            break;
        }
        
        read_blocks(block_adress, 1, buffer);
        
        // the byte index in the block to start writing
        int start_offset = (i == start_block) ? (entry->read_write_pointer % BLOCK_SIZE) : 0;
        int end_offset = (i == end_block) ? ((entry->read_write_pointer+length) % BLOCK_SIZE) : BLOCK_SIZE;
        int num_bytes_to_write = end_offset-start_offset;
        
        memcpy(&buffer[start_offset], &buf[buf_pointer], num_bytes_to_write);
        write_blocks(block_adress, 1, buffer);
        
        buf_pointer += num_bytes_to_write;
    }

    entry->read_write_pointer += buf_pointer;
    node->size += buf_pointer;
    write_to_disk();

    free(buffer);

    // the number of bytes written is just the pointer for the buf after writing
    return buf_pointer;
}

/* check if block is allocated, if not, allocate it (if allocate is set to true)*/
/* return the physical adress of the block */
int secure_block(int inode_num, int block, int allocate) {
    
    struct iNode *node = &iNodeTable[inode_num];

    /* if block has a direct pointer */
    if (block < 12) {
        if (node->direct_pointers[block] == -1) {

            if (allocate) {
                int new_block = get_free_block();
                node->direct_pointers[block] = new_block;
                node->num_blocks_allocated += 1;
                write_to_disk();
            } else {
                return -1;
            }    
        } 

        return node->direct_pointers[block];
    } 
    /* if block has an indirect pointer */
    else {

        /* allocate the indirect pointer if needed */
        if (node->indirect_pointer == -1) {
            if (allocate) {
                int indirect_block = get_free_block();
                node->indirect_pointer = indirect_block;
            } else {
                return -1;
            }    
        }

        /* this code traverses the indirect pointer block, looking for block */
        int indirectPointer[BLOCK_SIZE/sizeof(int)];
        char buf[BLOCK_SIZE];
        read_blocks(node->indirect_pointer, 1, &buf);
        memcpy(indirectPointer, &buf, sizeof(indirectPointer));

        if (indirectPointer[block] == 0) {
            if (allocate) {
                int new_block = get_free_block();
                indirectPointer[block] = new_block;
                memcpy(&buf, indirectPointer, sizeof(indirectPointer));  
                write_blocks(node->indirect_pointer, 1, &buf);  

                node->num_blocks_allocated += 1; 
                write_to_disk();
            }

            else {
                return -1;
            }    
        }
        return indirectPointer[block];
    }
}

/* close a file, if it is in the fd table*/
int sfs_fclose(int fd) {
    if (fileDescTable[fd].available == '0') {
        fileDescTable[fd].available = '1';
        return 0;
    } else {
        return -1;
    }
}

/* we only only file names of length <= MAX_FNAME_LENGTH */
int sfs_fopen(char* file_name) {

    if (strlen(file_name) > MAX_FNAME_LENGTH) {
        printf("file name too long\n");
        return -1;
    }

    struct dirEntry *entry;
    int fd;
    // if file doesnt exist, create it
    int dir_spot = exists_name(file_name);

    if (dir_spot < 0) {

        int inode_number = get_free_inode();

        dir_spot = get_dir_spot();
        entry = &directory[dir_spot];
        strncpy(entry->file_name, file_name, MAX_FNAME_LENGTH);
        // Im not sure if it needs to be null terminated
        //entry->file_name[MAX_FNAME_LENGTH] = '\0';
        entry->inode_num = inode_number;
        entry->available = '0';

        fd = get_fd(entry->inode_num);

    } else {
        entry = &directory[dir_spot];
    }

    fd = get_fd(entry->inode_num);
    write_to_disk();

    return fd;
}

void read_from_disk() {
    
    char buf[1024];

    read_blocks(0, 1, &buf);
    memcpy(super_block, &buf, sizeof(SuperBlock));

    read_blocks(1, 1, &buf);
    memcpy(free_blocks, &buf, NUM_BLOCKS);

    read_blocks(2, 1, &buf);
    memcpy(free_inodes, &buf, NUM_iNODES);

    read_blocks(3, 12, directory);

    read_blocks(15, super_block->iNode_table_length-1, iNodeTable);
    //read_blocks(15, super_block->iNode_table_length, iNodeTable);

}

/* Helper methods to find free block */
int get_free_block() {
    int i = 0;
    while (i < NUM_BLOCKS) {
        if (free_blocks[i] == '1') { 
            free_blocks[i] = '0';
            return i;
        }
        i++;
    }
    return -1;
}

// available = 0 means that the enrty is not available, anything
// else indicates that the enrty is available
/* finds an availble slot in the directory if it exists */
int get_dir_spot() {
    for (int i =0; i < DIR_SIZE; i++) {
        if (directory[i].available != '0') {
            directory[i].available = '0';
            return i;
        }
    }
    return -1;
}

int get_free_inode() {
    int i = 0;
    while (i < NUM_iNODES) {            
        if (free_inodes[i] == '1') { 
            free_inodes[i] = '0';
            return i;
        }
        i++;
    }
    printf("no free inodes\n");
    return -1;
}

int sfs_getnextfilename(char* file_name) {

    // im stopping at inodes, since we cant have more files than inodes
    for (int i = current+1; i < NUM_iNODES; i++) {
        if (directory[i].available == '0') {
            strncpy(file_name, directory[i].file_name, MAX_FNAME_LENGTH);
            current = i;
            return 1;
        }
    }
    // reset pointer to start
    current = -1;
    return 0;

}

/* 
iterate over directory, if we find a file with the same name,
and it hasnt been removed, then return its size.
*/
int sfs_getfilesize(char* file_name) {
    for (int i = 0; i< NUM_iNODES; i++) {
        if (strcmp(directory[i].file_name, file_name) == 0) {
            if (directory[i].available == '0') {
                return iNodeTable[directory[i].inode_num].size;
            } else {
                break;
            }
        }
    }

    return -1;
}


/* given an inode number, returns the fd corresponding the file if open,
and creates a new fd if file is not yet open */
int get_fd(int inode_num){
    for (int i = 0; i<NUM_iNODES; i++) {
        if (fileDescTable[i].available == '1') {
            fileDescTable[i].available = '0';
            fileDescTable[i].inode_num = inode_num;
            fileDescTable[i].read_write_pointer = iNodeTable[inode_num].size;
            return i;
        } else {
            if (fileDescTable[i].inode_num == inode_num) {
                return i;
            }
        }
    }

    return -1;
}
