Tapir/LLVM
================================

This directory and its subdirectories contain source code for Tapir/LLVM, a prototype compiler based on LLVM that implements the Tapir compiler IR extensions for fork-join parallelism.

Tapir/LLVM is under active development.  This directory contains prototype implementations of compiler technologies that take advantage of the Tapir compiler IR.  These prototype technologies include the Rhino extensions to Tapir (unpublished).

Tapir/LLVM is open source software.  You may freely distribute it under the terms of the license agreement found in LICENSE.txt.

[![CircleCI](https://circleci.com/gh/sfu-arch/Parallel-IR.svg?style=svg)](https://circleci.com/gh/sfu-arch/Parallel-IR)

#### Build

```sh
$ git clone "https://github.com/sfu-arch/Parallel-IR.git"
$ cd Parallel-IR
$ cd git submodule update --init --recursive
$ mkdir build; cd build
$ cmake .. -G "Ninja" 
$ ninja
``` 

##### Generate .deb packages 
After compiling Tapir run the following commands in your build directory: 
```sh
$ cpack -G DEB ..
``` 
Be careful, installing this deb requires you to uninstall the llvm* and clang* packages from ubuntu/debian.

# References

T. B. Schardl, W. S. Moses, C. E. Leiserson.  "Tapir: Embedding Fork-Join Parallelism into LLVM's Intermediate Representation."  ACM PPoPP, February 2017, pp. 249-265.  Won Best Paper Award. http://dl.acm.org/citation.cfm?id=3018758


