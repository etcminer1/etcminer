# Building from source

## Table of Contents

* [Common](#common)    
* [Windows](#windows)
* [Linux](#linux)
    * [OpenCL support on Linux](#opencl-support-on-linux)    
* [CMake configuration options](#cmake-configuration-options)

## Common
1. [CMake] >= 3.5, GCC version >= 4.8
2. [Git](https://git-scm.com/downloads)
3. [Perl](https://www.perl.org/get.html), needed to build OpenSSL
4. [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads) >= 9.0 (optional, install if need NVidia CUDA)

## Windows
VS 2019 is recommended, it should work well with `CMake` and `Hunter`.
1. Open etcminer project folder
2. Generate `CMake` cache, [configuration options](#cmake-configuration-options).
3. Generate all
4. (Option) install etcminer

## Linux
1. Create a build directory by:   
   `mkdir build; cd build`

2. Configure the project with `CMake`. Check out additional
   [configuration options](#cmake-configuration-options).   
   e.g., `cmake .. -DETHASHCUDA=ON -DETHASHCL=ON`

3. Install it:	
   `make DESTDIR=/home/xxx install`

### OpenCL support on Linux

If you're planning to use [OpenCL on Linux](https://github.com/ruslo/hunter/wiki/pkg.opencl#pitfalls)
you have to install OpenGL libraries.	
E.g. on Ubuntu run:	
`sudo apt-get install mesa-common-dev`	
(If it fails because of dependancy, just remove relevant packages and then try)

## CMake configuration options

CMake configuration command, e.g.
cmake .. -DETHASHCUDA=ON -DETHASHCL=OFF

- `-DETHASHCL=ON` - enable OpenCL mining, `ON` by default,
- `-DETHASHCUDA=ON` - enable CUDA mining, `OFF` by default.