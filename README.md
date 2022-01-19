# etcminer

Ethereum classic(ETC) miner with OpenCL, CUDA and stratum support

The etcminer is an ETC GPU miner, it originates from ethminer project.

### Donation
ETC address:
`0x23413a007da796875efa2f8c98fcc011c247f023`


### Features

- OpenCL and Nvidia CUDA support
- stratum mining without proxy
- farm failover (getwork + stratum)

## Table of Contents

- [Install](#install)
- [Usage](#usage)
- [Build from source](#build-from-source)
- [Maintainer](#maintainer)
- [Donation](#Donation)


## Install

Download an archive£¬ unpack it to a place accessible from command line. The etcminer is ready to go.

| Builds | Release | Date |
| ------ | ------- | ---- |
| Stable  | [Release](https://github.com/shenwude/etcminer/releases) | [Date](https://github.com/shenwude/etcminer/releases) |

## Usage

- Just launch **etcminer** either from a Windows cmd or Linux console. For help, please run `etcminer --help`

- Please try different schemes if you are not sure which scheme is supported by the pool.	
	And see [pool example](docs/POOL_EXAMPLES_ETC.md) for pool usage.

- In windows, if you cannot find your AMD cards:
	1. Go to `C:\Windows\System32` and search for `amdocl64.dll`, and then get the .dll file's path, just call it `theDllPath`
	2. Open regedit, and go to `HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenCL\Vendors`
	3. Create a new DWORD with the name of `theDllPath\amdocl64.dll`, and leave the key's value as 0

## Build from source

This project uses `CMake` and `Hunter` package manager.
See [docs/BUILD.md](docs/BUILD.md) for details.

### For Windows:
VS 2019 is recommended, it should work well with `CMake` and `Hunter`.
1. Open etcminer project folder
2. Generate `CMake` cache
3. Generate all
4. (Option) install etcminer

### For Linux:
1. Create a build directory by:   
   `mkdir build; cd build`

2. Configure the project with `CMake`. Check out additional
   [configuration options](#cmake-configuration-options).   
   e.g., `cmake .. -DETHASHCUDA=ON -DETHASHCL=ON`

3. Install it:	
   `make DESTDIR=/home/xxx install`

#### OpenCL support on Linux

If you're planning to use [OpenCL on Linux](https://github.com/ruslo/hunter/wiki/pkg.opencl#pitfalls)
you have to install OpenGL libraries.	
E.g. on Ubuntu run:	
`sudo apt-get install mesa-common-dev`	
(If it fails because of dependancy, just remove relevant packages and then try)

### CMake configuration options

CMake configuration command, e.g.
cmake .. -DETHASHCUDA=ON -DETHASHCL=OFF

- `-DETHASHCL=ON` - enable OpenCL mining, `ON` by default,
- `-DETHASHCUDA=ON` - enable CUDA mining, `OFF` by default.

## Maintainer

- [@etcminer1](https://github.com/etcminer1)


## License

Licensed under the [GNU General Public License, Version 3](LICENSE).
