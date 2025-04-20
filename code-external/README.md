This folder contains all the external libraries we depend upon.
We shall attempt to pull code from original repository, preferably latest release branches.
TO-DO: Auto-build and test latest upstream release commit and check-in in our software.

To add a new submodule, go to root of Vishwakarma and give following commands. 
Ex: For Freetype Library with Release Tag No. VER-2-3-2

git submodule add https://gitlab.freedesktop.org/freetype/freetype.git code-external/freetype  
cd code-external/freetype  
git checkout tags/VER-2-13-3  

Most of these libraries expect to be built on the local machine after download. Usually this is done by following commands:

cd library-name  
mkdir build  
cd build  
cmake ..  
cmake --build . --config Debug  

Peculiarities specific to each of our dependencies are listed below.

**zlib**  
mkdir build
cd build
cmake ../
cmake --build . --config Debug  

Building zlib seems to delete zonf.h . Restore it from git controls? Further add a /build line in .gitignore file to suppress all the build file being reported as untracked modifications.

**libpng**  
Build zlib before building libpng.  

mkdir build
cd build
cmake -D ZLIB_INCLUDE_DIR=../../zlib -D ZLIB_LIBRARY=../../zlib/build/Debug ..  
cmake --build . --config Debug  

The default repository of libpng does not come with "pnglibconf.h" . It needs to be created as part of configuration step. For the time being, just copy the file "pnglibconf.h.prebuilt" in the "/libpng-code/scripts" folder to "/libpng-code" and rename it to "pnglibconf.h".

**freetype**
It needs to be build using cmake so as to generate the "freetype.lib" which shall be statically linked in the application.
Open the .sln file in folder Vishwakarma\code-external\freetype\builds\windows\visualc
Change the build target to Debug, Add x64 in the Configuration manager and than build.
TODO: Change these step to use CMAKE to generate fresh x64 sln files.


