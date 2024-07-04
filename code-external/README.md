This folder contains all the external libraries we depend upon.
We shall attempt to pull code from original repository, preferably latest release branches.
TO-DO: Auto-build and test latest upstream release commit and check-in in our software.

To add a new submodule, go to root of Vishwakarma and give following commands. 
Ex: For Freetype Library with Release Tag No. VER-2-3-2

git submodule add https://gitlab.freedesktop.org/freetype/freetype.git code-external/freetype  
cd code-external/freetype  
git checkout tags/VER-2-13-2  

Most of these libraries expect to be built on the local machine after download. Usually this is done by following commands:

cd library-name  
mkdir build  
cd build  
cmake ..  
cmake --build . --config Debug  

Peculiarities specific to each of our dependencies are listed below.

**zlib**  
Building zlib seems to delete zonf.h . Restore it from git controls? Further add a /build line in .gitignore file to suppress all the build file being reported as untracked modifications.

**libpng**  
Build zlib before building libpng.  
cmake -D ZLIB_INCLUDE_DIR=../../zlib -D ZLIB_LIBRARY=../../zlib/build/Debug ..  
cmake --build . --config Debug  


