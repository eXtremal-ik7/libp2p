# libp2p
Libraries and utilities for distributed (peer-to-peer) applications

## Linux build
```
mkdir x86_64-Linux && cd x86_64-Linux
cmake ../src
make -j
```

## Windows build with visual studio
- Install latest cmake from https://cmake.org/download
- Clone git repository
- Run 'x64 Native Tools Command Prompt for VS 20xx' from start menu
- Run 'cmake-gui' from terminal
- Select source ("src" subdirectory in git repo) and your build directory, run 'Configure' (can take a long time!) and 'Generate'.
- Open Visual Studio solution and build all targets

## Mac OS X build
### Intel CPU
```
mkdir x86_64-Darwin && cd x86_64-Darwin
cmake ../src
make -j
```

### M1 CPU x86_64
```
mkdir x86_64-Darwin && cd x86_64-Darwin
cmake ../src -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_TOOLCHAIN_FILE=`pwd`/../src/cmake/toolchain-x86_64-Darwin.cmake
make -j
```

### M1 CPU native
```
mkdir x86_64-Darwin && cd x86_64-Darwin
cmake ../src -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_TOOLCHAIN_FILE=`pwd`/../src/cmake/toolchain-arm64-Darwin.cmake
make -j
```
