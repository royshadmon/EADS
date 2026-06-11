#ifndef UTIL_H
#define UTIL_H

//
// The following code was adapted from Jeremy P Bentham's blog post
// https://iosoft.blog/2020/05/25/raspberry-pi-dma-programming/
//
//
// Copyright (c) 2020 Jeremy P Bentham
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Raspberry Pi hardware version (0 to 4)
#define RPI_VERSION     3

// Location of peripheral registers in physical memory
#define PI_01_REG_BASE  0x20000000  // Pi Zero or 1
#define PI_23_REG_BASE  0x3F000000  // Pi 2 or 3
#define PI_4_REG_BASE   0xFE000000  // Pi 4

#if RPI_VERSION == 0
#define PHYS_REG_BASE   PI_01_REG_BASE
#define CLOCK_HZ        250000000
#define SPI_CLOCK_HZ    400000000
#elif RPI_VERSION == 1
#define PHYS_REG_BASE   PI_01_REG_BASE
#define CLOCK_HZ        250000000
#define SPI_CLOCK_HZ    250000000
#elif RPI_VERSION==2 || RPI_VERSION==3
#define PHYS_REG_BASE   PI_23_REG_BASE
#define CLOCK_HZ        250000000
#define SPI_CLOCK_HZ    250000000
#elif RPI_VERSION==4
#define PHYS_REG_BASE   PI_4_REG_BASE
#define CLOCK_HZ        375000000
#define SPI_CLOCK_HZ    200000000
#endif

// Location of peripheral registers in bus memory
#define BUS_REG_BASE 0x7E000000

// Size of memory page
#define PAGE_SIZE 0x1000

// VC flags for unchached DMA memory
#define DMA_MEM_FLAGS (MEM_FLAG_COHERENT|MEM_FLAG_ZERO)

#endif // UTIL_H
