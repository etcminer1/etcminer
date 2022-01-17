# etcminer

> Ethereum classical(ETC) miner with OpenCL, CUDA and stratum support

The etcminer is an ETC GPU mining worker. It originates from ethminer project.

### Features

- OpenCL mining
- Nvidia CUDA mining
- realistic benchmarking against arbitrary epoch/DAG/blocknumber
- on-GPU DAG generation (no more DAG files on disk)
- stratum mining without proxy
- OpenCL devices picking
- farm failover (getwork + stratum)


## Table of Contents

- [Install](#install)
- [Usage](#usage)
- [Build](#build)
  - [Continuous Integration and development builds](#continuous-integration-and-development-builds)
  - [Building from source](#building-from-source)
  - [CMake configuration options](#cmake-configuration-options)
- [Maintainer](#maintainer)
- [Contribute](#contribute)
- [F.A.Q.](#faq)


## Install

[![Releases](https://img.shields.io/github/downloads/ethereum-mining/ethminer/total.svg)][Releases]

Download an archive for your operating system and unpack the content to a place
accessible from command line. The etcminer is ready to go.

| Builds | Release | Date |
| ------ | ------- | ---- |
| Stable  | [![GitHub release](https://img.shields.io/github/release/ethereum-mining/ethminer.svg)](https://github.com/ethereum-mining/ethminer/releases) | [![GitHub Release Date](https://img.shields.io/github/release-date/ethereum-mining/ethminer.svg)](https://github.com/ethereum-mining/ethminer/releases) |


## Usage

The **etcminer** is a command line program. Just launch it either
from a Windows cmd or Linux console, for hlep, please run
etcminer --help


## Build


### Building from source

This project uses [CMake] and [Hunter] package manager.

In Windows, VS 2019 is recommended, it should work well with CMake and Hunter.

For Linux:
1. Create a build directory.

   ```sh
   mkdir build; cd build
   ```

2. Configure the project with CMake. Check out additional
   [configuration options](#cmake-configuration-options).

   ```sh
   cmake ..
   ```


3. Build the project using [CMake Build Tool Mode]. This is a portable variant
   of `make`.

   ```sh
   cmake --build .
   ```
4. _(Optional, Linux only)_ Install the built executable.

   ```sh
   sudo make install
   ```

#### OpenCL support on Linux

If you're planning to use [OpenCL on Linux](https://github.com/ruslo/hunter/wiki/pkg.opencl#pitfalls)
you have to install OpenGL libraries. E.g. on Ubuntu run:

```sh
sudo apt-get install mesa-common-dev
```

### CMake configuration options

Pass these options to CMake configuration command, e.g.

```sh
cmake .. -DETHASHCUDA=ON -DETHASHCL=OFF
```

- `-DETHASHCL=ON` - enable OpenCL mining, `ON` by default,
- `-DETHASHCUDA=ON` - enable CUDA mining, `OFF` by default.


## Maintainer

- Shen [@shenwude]


## Contribute




## License

Licensed under the [GNU General Public License, Version 3](LICENSE).


## F.A.Q

8. Can I CPU Mine?
   Not supported by etcminer.