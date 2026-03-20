#ifndef MINIMAFSHANDLER_H
#define MINIMAFSHANDLER_H

#include "x86_64/ahci.h"
#include "x86_64/minimafs.h"

typedef struct {
    ahci_drive_t* ahci_drive;
    uint32_t sector_size;
} minimafs_disk_device_t;

void exampleinit(void);
minimafs_disk_device_t* initializeminimafs(int driveindex);
ahci_drive_t* getadrive();
minimafs_disk_device_t* setdrive(ahci_drive_t* disk);
int mountdrive(minimafs_disk_device_t* device, int deviceindex);
minimafs_disk_device_t* getminimadrive();

#endif //MINIMAFSHANDLER_H