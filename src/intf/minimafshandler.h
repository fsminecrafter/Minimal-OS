#ifndef MINIMAFSHANDLER_H
#define MINIMAFSHANDLER_H

#include "x86_64/ahci.h"
#include "x86_64/minimafs.h"

typedef struct {
    ahci_drive_t* ahci_drive;
    uint32_t sector_size;
} minimafs_disk_device_t;

void exampleinit(void);
void initializeminimafs(void);
ahci_drive_t* getdrive(int driveindex);
minimafs_disk_device_t* setdrive(ahci_drive_t* disk);
bool mountdrive(minimafs_disk_device_t* device, int deviceindex);
minimafs_disk_device_t* getminimadrive();

#endif //MINIMAFSHANDLER_H