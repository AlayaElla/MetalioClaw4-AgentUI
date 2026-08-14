cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(metalio_pet_demo)

include(elf_loader)

# Keep PT_LOAD segments compact. The ESP ELF loader allocates their complete
# virtual-address span as one internal executable block, so the GNU ld default
# 4 KiB page alignment wastes scarce contiguous executable memory.
set(ELF_LIBS "-Wl,-z,max-page-size=16")
project_elf(metalio_pet_demo)
