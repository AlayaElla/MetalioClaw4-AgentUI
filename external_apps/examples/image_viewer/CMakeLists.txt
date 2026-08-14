cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(metalio_image_viewer)

# The official Espressif helper turns this small IDF project into a relocatable
# .app.elf instead of a flashable firmware image.
include(elf_loader)

# GNU ld otherwise aligns each PT_LOAD segment to a 4 KiB page. The ESP ELF
# loader allocates the complete virtual-address span as one executable block,
# so that default creates several kilobytes of unused padding and can exhaust
# the ESP32-P4 internal executable heap. External apps do not use MMU pages;
# 16-byte segment alignment is sufficient for their code and data.
set(ELF_LIBS "-Wl,-z,max-page-size=16")
project_elf(metalio_image_viewer)
