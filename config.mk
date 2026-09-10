# Module Configuration for Minimal OS
# Each module can be enabled (1) or disabled (0)
# Default: all modules are enabled

# Audio subsystem (AC97 driver, audio player, music support)
ENABLE_AUDIO := 1

# GPU subsystem (graphics drivers and display management)
ENABLE_GPU := 1

# PCI subsystem (PCI device detection and management)
ENABLE_PCI := 1

# USB subsystem (USB controller and device support)
ENABLE_USB := 1

# ============================================================
# BUILD CONFIGURATION (automatically set based on above)
# Do not edit below this line manually - use config.sh instead
# ============================================================

# Module source files to include/exclude
ifeq ($(ENABLE_AUDIO),1)
	AUDIO_MODULE_INCLUDE := 1
else
	AUDIO_MODULE_INCLUDE := 0
endif

ifeq ($(ENABLE_GPU),1)
	GPU_MODULE_INCLUDE := 1
else
	GPU_MODULE_INCLUDE := 0
endif

ifeq ($(ENABLE_PCI),1)
	PCI_MODULE_INCLUDE := 1
else
	PCI_MODULE_INCLUDE := 0
endif

ifeq ($(ENABLE_USB),1)
	USB_MODULE_INCLUDE := 1
else
	USB_MODULE_INCLUDE := 0
endif
