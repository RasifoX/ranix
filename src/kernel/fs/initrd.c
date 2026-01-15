#include "initrd.h"
#include "string.h"
#include "stdio.h"
#include "heap.h"

fs_node_t *initrd_root;
fs_node_t *initrd_test_file;
struct dirent directory_entry;

uint32_t file_location;
uint32_t file_size;

uint32_t initrd_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{

    if (node != initrd_test_file)
        return 0;

    if (offset > node->length)
        return 0;
    if (offset + size > node->length)
        size = node->length - offset;

    memcpy(buffer, (void *)(file_location + offset), size);

    return size;
}

struct dirent *initrd_readdir(fs_node_t *node, uint32_t index)
{
    if (node == initrd_root && index == 0)
    {
        strcpy(directory_entry.name, "test.txt");
        directory_entry.ino = initrd_test_file->inode;
        return &directory_entry;
    }
    return 0;
}

fs_node_t *initrd_finddir(fs_node_t *node, char *name)
{
    if (node == initrd_root && strcmp(name, "test.txt") == 0)
    {
        return initrd_test_file;
    }
    return 0;
}

fs_node_t *initialise_initrd(uint32_t location, uint32_t end)
{
    file_location = location;
    file_size = end - location;

    initrd_root = (fs_node_t *)kmalloc(sizeof(fs_node_t));
    strcpy(initrd_root->name, "initrd");
    initrd_root->flags = FS_DIRECTORY;
    initrd_root->read = 0;
    initrd_root->write = 0;
    initrd_root->open = 0;
    initrd_root->close = 0;
    initrd_root->readdir = &initrd_readdir;
    initrd_root->finddir = &initrd_finddir;
    initrd_root->ptr = 0;
    initrd_root->inode = 0;
    initrd_root->length = 0;

    initrd_test_file = (fs_node_t *)kmalloc(sizeof(fs_node_t));
    strcpy(initrd_test_file->name, "test.txt");
    initrd_test_file->flags = FS_FILE;
    initrd_test_file->length = file_size;
    initrd_test_file->inode = 1;
    initrd_test_file->read = &initrd_read;
    initrd_test_file->write = 0;
    initrd_test_file->readdir = 0;
    initrd_test_file->finddir = 0;

    return initrd_root;
}