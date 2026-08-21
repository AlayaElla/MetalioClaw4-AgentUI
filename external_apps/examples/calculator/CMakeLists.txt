cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(metalio_calculator)

include(elf_loader)

# Keep the external ELF PT_LOAD span compact for the host's executable-RAM
# loader.
set(ELF_LIBS "-Wl,-z,max-page-size=16")
project_elf(metalio_calculator)
