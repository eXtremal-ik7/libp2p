# libp2p
Libraries and utilities for distributed (peer-to-peer) applications

## Linux & MacOS X build
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

## Force x86_64 architecture on MacOS X with Apple Silicon
```
mkdir x86_64-Darwin && cd x86_64-Darwin
cmake ../src -DCMAKE_OSX_ARCHITECTURES=x86_64
make -j
```
